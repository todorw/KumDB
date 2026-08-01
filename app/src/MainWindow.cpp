#include "MainWindow.h"
#include "NewTableDialog.h"
#include "QueryPanel.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QSplitter>
#include <QListWidget>
#include <QTableView>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QHeaderView>

extern "C" {
#include "types.h" // for kdb_type_infer -- same inference the CLI/engine already use
}

static QVariant inferredVariant(const QString &raw) {
    QByteArray utf8 = raw.toUtf8();
    KdbType t = kdb_type_infer(utf8.constData());
    switch (t) {
        case KDB_TYPE_INT:   return QVariant::fromValue<qint64>(raw.toLongLong());
        case KDB_TYPE_FLOAT: return QVariant(raw.toDouble());
        case KDB_TYPE_BOOL:  return QVariant(raw.compare("true", Qt::CaseInsensitive) == 0 || raw == "1");
        case KDB_TYPE_NULL:  return QVariant();
        default:             return QVariant(raw);
    }
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("KumDB Studio");
    resize(1000, 650);

    auto *menuFile = menuBar()->addMenu("&File");
    menuFile->addAction("&Open Database...", this, &MainWindow::openDatabase);
    menuFile->addAction("&New Database...", this, &MainWindow::newDatabase);
    menuFile->addAction("&Close Database", this, &MainWindow::closeDatabase);
    menuFile->addSeparator();
    menuFile->addAction("E&xit", this, &QWidget::close);

    auto *menuTable = menuBar()->addMenu("&Table");
    menuTable->addAction("&New Table...", this, &MainWindow::newTable);
    menuTable->addAction("&Drop Table", this, &MainWindow::dropTable);
    menuTable->addAction("&Compact Table", this, &MainWindow::compactTable);
    menuTable->addAction("&Refresh", this, &MainWindow::refreshTables);

    auto *menuHelp = menuBar()->addMenu("&Help");
    menuHelp->addAction("&About KumDB Studio", this, &MainWindow::showAbout);

    auto *splitter = new QSplitter(this);

    auto *leftPanel = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->addWidget(new QLabel("Tables", leftPanel));
    m_tableList = new QListWidget(leftPanel);
    leftLayout->addWidget(m_tableList);
    auto *leftButtons = new QHBoxLayout;
    auto *newTableBtn = new QPushButton("New", leftPanel);
    auto *dropTableBtn = new QPushButton("Drop", leftPanel);
    leftButtons->addWidget(newTableBtn);
    leftButtons->addWidget(dropTableBtn);
    leftLayout->addLayout(leftButtons);
    connect(newTableBtn, &QPushButton::clicked, this, &MainWindow::newTable);
    connect(dropTableBtn, &QPushButton::clicked, this, &MainWindow::dropTable);
    connect(m_tableList, &QListWidget::itemClicked, this, &MainWindow::onTableSelected);

    auto *tabs = new QTabWidget(splitter);

    auto *datasheetPage = new QWidget(tabs);
    auto *datasheetLayout = new QVBoxLayout(datasheetPage);
    auto *datasheetButtons = new QHBoxLayout;
    auto *addRowBtn = new QPushButton("+ Add row", datasheetPage);
    auto *deleteRowBtn = new QPushButton("- Delete row", datasheetPage);
    auto *refreshBtn = new QPushButton("Refresh", datasheetPage);
    datasheetButtons->addWidget(addRowBtn);
    datasheetButtons->addWidget(deleteRowBtn);
    datasheetButtons->addWidget(refreshBtn);
    datasheetButtons->addStretch();
    datasheetLayout->addLayout(datasheetButtons);

    m_datasheetModel = new RowTableModel(&m_db, this);
    m_datasheet = new QTableView(datasheetPage);
    m_datasheet->setModel(m_datasheetModel);
    m_datasheet->horizontalHeader()->setStretchLastSection(true);
    datasheetLayout->addWidget(m_datasheet);
    tabs->addTab(datasheetPage, "Datasheet");

    connect(addRowBtn, &QPushButton::clicked, this, &MainWindow::addRowViaDatasheet);
    connect(deleteRowBtn, &QPushButton::clicked, this, &MainWindow::deleteSelectedRow);
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::reloadDatasheet);
    connect(m_datasheetModel, &RowTableModel::editFailed, this, [this](const QString &err) {
        QMessageBox::warning(this, "Edit failed", err);
    });

    m_queryPanel = new QueryPanel(&m_db, tabs);
    tabs->addTab(m_queryPanel, "Query");
    connect(m_queryPanel, &QueryPanel::statusMessage, this, [this](const QString &msg) {
        m_statusLabel->setText(msg);
    });

    splitter->addWidget(leftPanel);
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 780});
    setCentralWidget(splitter);

    m_statusLabel = new QLabel("No database open.", this);
    statusBar()->addWidget(m_statusLabel);

    setDbLoadedState(false);
}

void MainWindow::setDbLoadedState(bool loaded) {
    m_tableList->setEnabled(loaded);
    m_datasheet->setEnabled(loaded);
    m_queryPanel->setEnabled(loaded);
}

void MainWindow::openDir(const QString &dir) {
    QString err;
    if (!m_db.open(dir, &err)) {
        QMessageBox::critical(this, "Open Database", err);
        return;
    }
    setDbLoadedState(true);
    m_statusLabel->setText("Open: " + dir);
    refreshTables();
}

void MainWindow::openDatabase() {
    QString dir = QFileDialog::getExistingDirectory(this, "Open KumDB Database Folder");
    if (dir.isEmpty()) return;
    openDir(dir);
}

void MainWindow::newDatabase() {
    QString dir = QFileDialog::getExistingDirectory(this, "Choose (or create) a Folder for the New Database");
    if (dir.isEmpty()) return;
    openDir(dir); // kdb_open() creates the directory/tables as needed
}

void MainWindow::closeDatabase() {
    m_db.close();
    m_tableList->clear();
    m_datasheetModel->clear();
    setDbLoadedState(false);
    m_statusLabel->setText("No database open.");
}

void MainWindow::refreshTables() {
    if (!m_db.isOpen()) return;
    QString current = currentTableName();
    m_tableList->clear();
    m_tableList->addItems(m_db.listTables());
    if (!current.isEmpty()) {
        auto items = m_tableList->findItems(current, Qt::MatchExactly);
        if (!items.isEmpty()) m_tableList->setCurrentItem(items.first());
    }
    m_queryPanel->refreshTableList();
}

QString MainWindow::currentTableName() const {
    auto *item = m_tableList->currentItem();
    return item ? item->text() : QString();
}

void MainWindow::onTableSelected(QListWidgetItem *) {
    reloadDatasheet();
}

void MainWindow::reloadDatasheet() {
    QString table = currentTableName();
    if (table.isEmpty() || !m_db.isOpen()) { m_datasheetModel->clear(); return; }

    QVector<KColumnMeta> schema = m_db.schema(table);
    QStringList columns;
    for (const auto &c : schema) columns << c.name;

    QString err;
    QVector<KRow> rows = m_db.find(table, {}, &err);
    if (!err.isEmpty()) {
        QMessageBox::warning(this, "Find", err);
        return;
    }
    if (columns.isEmpty()) {
        // no declared schema (e.g. freshly created, empty table) -- fall back
        // to whatever fields actually show up in the data
        for (const auto &r : rows)
            for (const auto &f : r.fields)
                if (!columns.contains(f.first)) columns << f.first;
    }

    m_datasheetModel->setData(table, columns, rows, true);
    m_datasheet->resizeColumnsToContents();
    m_statusLabel->setText(QString("%1: %2 row(s)").arg(table).arg(rows.size()));
}

void MainWindow::addRowViaDatasheet() {
    QString table = currentTableName();
    if (table.isEmpty()) { QMessageBox::information(this, "Add row", "Select a table first."); return; }

    bool ok = false;
    QString text = QInputDialog::getMultiLineText(
        this, "Add Row to " + table,
        "One field per line, key=value (matches the CLI's 'add' syntax):",
        "", &ok);
    if (!ok || text.trimmed().isEmpty()) return;

    QVector<QPair<QString, QVariant>> fields;
    for (const QString &lineRaw : text.split('\n', Qt::SkipEmptyParts)) {
        QString line = lineRaw.trimmed();
        if (line.isEmpty()) continue;
        int eq = line.indexOf('=');
        if (eq < 0) {
            QMessageBox::warning(this, "Add row", "Bad field (need key=value): " + line);
            return;
        }
        QString key = line.left(eq).trimmed();
        QString val = line.mid(eq + 1).trimmed();
        fields.append({key, inferredVariant(val)});
    }
    if (fields.isEmpty()) return;

    QString err;
    if (!m_db.addRow(table, fields, &err)) {
        QMessageBox::warning(this, "Add row", err);
        return;
    }
    reloadDatasheet();
}

void MainWindow::deleteSelectedRow() {
    QString table = currentTableName();
    if (table.isEmpty()) return;

    auto sel = m_datasheet->selectionModel();
    if (!sel || !sel->hasSelection()) {
        QMessageBox::information(this, "Delete row", "Select a row first.");
        return;
    }
    int row = sel->selectedRows().isEmpty() ? sel->currentIndex().row() : sel->selectedRows().first().row();
    quint64 id = m_datasheetModel->idAt(row);
    if (id == 0) return;

    if (QMessageBox::question(this, "Delete row", QString("Delete row id=%1?").arg(id)) != QMessageBox::Yes)
        return;

    QString err;
    if (!m_db.deleteRow(table, id, &err)) {
        QMessageBox::warning(this, "Delete row", err);
        return;
    }
    reloadDatasheet();
}

void MainWindow::newTable() {
    if (!m_db.isOpen()) { QMessageBox::information(this, "New Table", "Open a database first."); return; }

    NewTableDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    QString err;
    if (!m_db.createTable(dlg.tableName(), dlg.columns(), &err)) {
        QMessageBox::warning(this, "New Table", err);
        return;
    }
    refreshTables();
}

void MainWindow::dropTable() {
    QString table = currentTableName();
    if (table.isEmpty()) { QMessageBox::information(this, "Drop Table", "Select a table first."); return; }

    if (QMessageBox::question(this, "Drop Table", QString("Drop table '%1'? This can't be undone.").arg(table))
        != QMessageBox::Yes)
        return;

    QString err;
    if (!m_db.dropTable(table, &err)) {
        QMessageBox::warning(this, "Drop Table", err);
        return;
    }
    m_datasheetModel->clear();
    refreshTables();
}

void MainWindow::compactTable() {
    QString table = currentTableName();
    if (table.isEmpty()) { QMessageBox::information(this, "Compact Table", "Select a table first."); return; }

    QString err;
    if (!m_db.compactTable(table, &err)) {
        QMessageBox::warning(this, "Compact Table", err);
        return;
    }
    m_statusLabel->setText("Compacted " + table + ".");
    reloadDatasheet();
}

void MainWindow::showAbout() {
    QMessageBox::about(this, "About KumDB Studio",
        QString("<b>KumDB Studio</b><br>"
                "A desktop front end for KumDB (engine v%1) -- browse and edit tables, "
                "run NoSQL filters or SQL, all against the same embedded database file.<br><br>"
                "No JOINs, no subqueries -- same limits as the engine itself.")
            .arg(QString::fromUtf8(kdb_version())));
}
