#include "QueryPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QHeaderView>
#include <QStackedWidget>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QVariantMap>

static const int kMaxHistory = 25;

QueryPanel::QueryPanel(KumDbHandle *db, QWidget *parent)
    : QWidget(parent), m_db(db) {
    auto *layout = new QVBoxLayout(this);

    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel("Mode:", this));
    m_modeBox = new QComboBox(this);
    m_modeBox->addItem("NoSQL (filters)");
    m_modeBox->addItem("SQL");
    modeRow->addWidget(m_modeBox);
    modeRow->addStretch();
    modeRow->addWidget(new QLabel("History:", this));
    m_historyBox = new QComboBox(this);
    m_historyBox->setMinimumWidth(260);
    m_historyBox->addItem("(none yet)");
    modeRow->addWidget(m_historyBox);
    layout->addLayout(modeRow);

    m_inputStack = new QStackedWidget(this);

    // NoSQL page: table picker + one filter per line
    auto *nosqlPage = new QWidget(this);
    auto *nosqlLayout = new QVBoxLayout(nosqlPage);
    auto *tableRow = new QHBoxLayout;
    tableRow->addWidget(new QLabel("Table:", nosqlPage));
    m_tableBox = new QComboBox(nosqlPage);
    m_tableBox->setEditable(true);
    tableRow->addWidget(m_tableBox);
    tableRow->addStretch();
    nosqlLayout->addLayout(tableRow);
    nosqlLayout->addWidget(new QLabel(
        "Filters, one per line (e.g. age__gt=21) -- empty = all rows. AND by default; "
        "prefix a line with OR: to start a new OR'd group:", nosqlPage));
    m_filterEdit = new QPlainTextEdit(nosqlPage);
    m_filterEdit->setObjectName("filterEditor");
    m_filterEdit->setPlaceholderText("age__gt=21\nname__startswith=Al\nOR:vip=true");
    m_filterEdit->setMaximumHeight(90);
    nosqlLayout->addWidget(m_filterEdit);
    m_inputStack->addWidget(nosqlPage);

    // SQL page: one statement
    auto *sqlPage = new QWidget(this);
    auto *sqlLayout = new QVBoxLayout(sqlPage);
    sqlLayout->addWidget(new QLabel(
        "SQL statement (one per run) -- SELECT/INSERT/UPDATE/DELETE, JOINs, subqueries, "
        "CTEs, window functions, transactions, and DDL all work here:", sqlPage));
    m_sqlEdit = new QPlainTextEdit(sqlPage);
    m_sqlEdit->setObjectName("sqlEditor");
    m_sqlEdit->setPlaceholderText("SELECT * FROM users WHERE age > 21 ORDER BY age DESC LIMIT 20");
    m_sqlEdit->setMaximumHeight(90);
    sqlLayout->addWidget(m_sqlEdit);
    m_inputStack->addWidget(sqlPage);

    layout->addWidget(m_inputStack);

    auto *runRow = new QHBoxLayout;
    auto *runBtn = new QPushButton("▶  Run", this);
    runBtn->setObjectName("primaryButton");
    runRow->addWidget(runBtn);
    runRow->addStretch();
    m_exportBtn = new QPushButton("Export CSV...", this);
    m_exportBtn->setEnabled(false);
    runRow->addWidget(m_exportBtn);
    layout->addLayout(runRow);

    m_infoLabel = new QLabel(this);
    layout->addWidget(m_infoLabel);

    m_resultsModel = new RowTableModel(m_db, this);
    m_resultsView = new QTableView(this);
    m_resultsView->setModel(m_resultsModel);
    m_resultsView->setEditTriggers(QAbstractItemView::NoEditTriggers); // query results are read-only
    m_resultsView->setAlternatingRowColors(true);
    m_resultsView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsView->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_resultsView, 1);

    connect(runBtn, &QPushButton::clicked, this, &QueryPanel::runQuery);
    connect(m_exportBtn, &QPushButton::clicked, this, &QueryPanel::exportResultsCsv);
    connect(m_modeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &QueryPanel::onModeChanged);
    connect(m_historyBox, QOverload<int>::of(&QComboBox::activated), this, &QueryPanel::onHistoryPicked);

    onModeChanged(0);
}

void QueryPanel::onModeChanged(int index) {
    m_inputStack->setCurrentIndex(index);
}

void QueryPanel::refreshTableList() {
    QString current = m_tableBox->currentText();
    m_tableBox->clear();
    m_tableBox->addItems(m_db->listTables());
    if (!current.isEmpty()) m_tableBox->setCurrentText(current);
}

static QStringList collectColumns(const QVector<KRow> &rows) {
    QStringList cols;
    for (const auto &r : rows)
        for (const auto &f : r.fields) {
            // column 0 is always "id" (from KRow::id, see RowTableModel) --
            // an explicit "SELECT id" now carries id as a real field too,
            // same value, so skip it here rather than showing it twice.
            // Only when the values actually agree, though -- a RETURNING
            // row doesn't populate KRow::id itself (a separate,
            // pre-existing quirk, see kdb_row_print's own version of this
            // same check in kumdb.c), so its "id" field can legitimately
            // be the only place carrying the real value.
            if (f.first == QStringLiteral("id") && f.second.canConvert<qint64>() &&
                (quint64)f.second.value<qint64>() == r.id)
                continue;
            if (!cols.contains(f.first)) cols << f.first;
        }
    return cols;
}

void QueryPanel::rememberHistory(int mode, const QString &table, const QString &text) {
    if (text.trimmed().isEmpty()) return;

    QVariantMap entry;
    entry["mode"] = mode;
    entry["table"] = table;
    entry["text"] = text;

    // de-dupe: drop any existing entry with the same mode+text before re-adding at the front
    for (int i = m_historyBox->count() - 1; i >= 0; i--) {
        QVariantMap existing = m_historyBox->itemData(i).toMap();
        if (existing["mode"] == mode && existing["text"] == text) m_historyBox->removeItem(i);
    }
    if (m_historyBox->count() == 1 && m_historyBox->itemText(0) == "(none yet)") m_historyBox->removeItem(0);

    QString oneLine = text.simplified();
    QString label = mode == 0 ? QString("[%1] %2").arg(table, oneLine) : QString("[SQL] %1").arg(oneLine);
    if (label.size() > 70) label = label.left(67) + "...";

    m_historyBox->insertItem(0, label, entry);
    while (m_historyBox->count() > kMaxHistory) m_historyBox->removeItem(m_historyBox->count() - 1);
    m_historyBox->setCurrentIndex(0);
}

void QueryPanel::onHistoryPicked(int index) {
    QVariantMap entry = m_historyBox->itemData(index).toMap();
    if (entry.isEmpty()) return;

    int mode = entry["mode"].toInt();
    m_modeBox->setCurrentIndex(mode);
    if (mode == 0) {
        m_tableBox->setCurrentText(entry["table"].toString());
        m_filterEdit->setPlainText(entry["text"].toString());
    } else {
        m_sqlEdit->setPlainText(entry["text"].toString());
    }
}

void QueryPanel::runQuery() {
    if (!m_db->isOpen()) {
        m_infoLabel->setText("No database open.");
        return;
    }

    if (m_modeBox->currentIndex() == 0) {
        QString table = m_tableBox->currentText().trimmed();
        if (table.isEmpty()) {
            m_infoLabel->setText("Pick a table first.");
            return;
        }
        QString filterText = m_filterEdit->toPlainText();
        QStringList filters = filterText.split('\n', Qt::SkipEmptyParts);
        QString err;
        QVector<KRow> rows = m_db->find(table, filters, &err);
        if (!err.isEmpty()) {
            m_infoLabel->setText("Error: " + err);
            m_resultsModel->clear();
            m_exportBtn->setEnabled(false);
            emit statusMessage(err);
            return;
        }
        m_resultsModel->setData(table, collectColumns(rows), rows, false);
        m_infoLabel->setText(QString("%1 row(s)").arg(rows.size()));
        m_exportBtn->setEnabled(!rows.isEmpty());
        rememberHistory(0, table, filterText);
    } else {
        QString stmt = m_sqlEdit->toPlainText().trimmed();
        if (stmt.isEmpty()) {
            m_infoLabel->setText("Type a SQL statement first.");
            return;
        }
        KumDbHandle::SqlResult res = m_db->execSql(stmt);
        if (!res.ok) {
            m_infoLabel->setText("Error: " + res.error);
            m_resultsModel->clear();
            m_exportBtn->setEnabled(false);
            emit statusMessage(res.error);
            return;
        }
        if (res.hasRows) {
            m_resultsModel->setData(QString(), collectColumns(res.rows), res.rows, false);
            m_infoLabel->setText(QString("%1 row(s)").arg(res.rows.size()));
            m_exportBtn->setEnabled(!res.rows.isEmpty());
        } else {
            m_resultsModel->clear();
            m_infoLabel->setText(QString("OK. %1 row(s) affected.").arg((qulonglong)res.affected));
            m_exportBtn->setEnabled(false);
        }
        rememberHistory(1, QString(), stmt);
    }
}

void QueryPanel::exportResultsCsv() {
    if (m_resultsModel->rowCount() == 0) return;
    QString path = QFileDialog::getSaveFileName(this, "Export Query Results as CSV",
                                                 "query_results.csv", "CSV files (*.csv)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Truncate | QFile::Text)) {
        QMessageBox::warning(this, "Export CSV", f.errorString());
        return;
    }
    QTextStream out(&f);
    auto writeField = [&out](const QString &s) {
        if (s.contains(',') || s.contains('"') || s.contains('\n')) {
            QString escaped = s;
            escaped.replace('"', "\"\"");
            out << '"' << escaped << '"';
        } else {
            out << s;
        }
    };
    int cols = m_resultsModel->columnCount();
    for (int c = 0; c < cols; c++) {
        if (c) out << ',';
        writeField(m_resultsModel->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString());
    }
    out << '\n';
    for (int r = 0; r < m_resultsModel->rowCount(); r++) {
        for (int c = 0; c < cols; c++) {
            if (c) out << ',';
            writeField(m_resultsModel->index(r, c).data(Qt::DisplayRole).toString());
        }
        out << '\n';
    }
    emit statusMessage("Exported to " + path);
}
