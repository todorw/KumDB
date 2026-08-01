#include "QueryPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QStackedWidget>
#include <QGroupBox>

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
    nosqlLayout->addWidget(new QLabel("Filters, one per line (e.g. age__gt=21) -- empty = all rows:", nosqlPage));
    m_filterEdit = new QPlainTextEdit(nosqlPage);
    m_filterEdit->setPlaceholderText("age__gt=21\nname__startswith=Al");
    m_filterEdit->setMaximumHeight(90);
    nosqlLayout->addWidget(m_filterEdit);
    m_inputStack->addWidget(nosqlPage);

    // SQL page: one statement
    auto *sqlPage = new QWidget(this);
    auto *sqlLayout = new QVBoxLayout(sqlPage);
    sqlLayout->addWidget(new QLabel("SQL statement (one per run -- no JOIN/subqueries/OR):", sqlPage));
    m_sqlEdit = new QPlainTextEdit(sqlPage);
    m_sqlEdit->setPlaceholderText("SELECT * FROM users WHERE age > 21 ORDER BY age DESC LIMIT 20");
    m_sqlEdit->setMaximumHeight(90);
    sqlLayout->addWidget(m_sqlEdit);
    m_inputStack->addWidget(sqlPage);

    layout->addWidget(m_inputStack);

    auto *runRow = new QHBoxLayout;
    auto *runBtn = new QPushButton("Run", this);
    runRow->addWidget(runBtn);
    runRow->addStretch();
    layout->addLayout(runRow);

    m_infoLabel = new QLabel(this);
    layout->addWidget(m_infoLabel);

    m_resultsModel = new RowTableModel(m_db, this);
    m_resultsView = new QTableView(this);
    m_resultsView->setModel(m_resultsModel);
    m_resultsView->setEditTriggers(QAbstractItemView::NoEditTriggers); // query results are read-only
    layout->addWidget(m_resultsView, 1);

    connect(runBtn, &QPushButton::clicked, this, &QueryPanel::runQuery);
    connect(m_modeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &QueryPanel::onModeChanged);

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
        for (const auto &f : r.fields)
            if (!cols.contains(f.first)) cols << f.first;
    return cols;
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
        QStringList filters = m_filterEdit->toPlainText().split('\n', Qt::SkipEmptyParts);
        QString err;
        QVector<KRow> rows = m_db->find(table, filters, &err);
        if (!err.isEmpty()) {
            m_infoLabel->setText("Error: " + err);
            m_resultsModel->clear();
            emit statusMessage(err);
            return;
        }
        m_resultsModel->setData(table, collectColumns(rows), rows, false);
        m_infoLabel->setText(QString("%1 row(s)").arg(rows.size()));
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
            emit statusMessage(res.error);
            return;
        }
        if (res.hasRows) {
            m_resultsModel->setData(QString(), collectColumns(res.rows), res.rows, false);
            m_infoLabel->setText(QString("%1 row(s)").arg(res.rows.size()));
        } else {
            m_resultsModel->clear();
            m_infoLabel->setText(QString("OK. %1 row(s) affected.").arg((qulonglong)res.affected));
        }
    }
}
