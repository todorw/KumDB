#include "MainWindow.h"
#include "NewTableDialog.h"
#include "QueryPanel.h"

#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QStyle>
#include <QSplitter>
#include <QListWidget>
#include <QTableView>
#include <QTableWidget>
#include <QItemSelectionModel>
#include <QHeaderView>
#include <QTabWidget>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QFile>
#include <QTextStream>

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

static const char *typeName(KdbFieldType t) {
    switch (t) {
        case KDB_TYPE_INT:    return "INT";
        case KDB_TYPE_FLOAT:  return "FLOAT";
        case KDB_TYPE_BOOL:   return "BOOL";
        case KDB_TYPE_STRING: return "TEXT";
        case KDB_TYPE_BLOB:   return "BLOB";
        case KDB_TYPE_ARRAY:  return "ARRAY";
        case KDB_TYPE_OBJECT: return "OBJECT";
        default:              return "?";
    }
}

// Writes a model's visible rows/columns out as CSV (RFC 4180-ish: quotes
// doubled, whole field quoted if it contains a comma/quote/newline) --
// shared shape for both the datasheet and query-results exports.
static bool writeModelToCsv(QAbstractItemModel *model, const QString &path, QString *errorOut) {
    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Truncate | QFile::Text)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
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

    int cols = model->columnCount();
    for (int c = 0; c < cols; c++) {
        if (c) out << ',';
        writeField(model->headerData(c, Qt::Horizontal).toString());
    }
    out << '\n';
    for (int r = 0; r < model->rowCount(); r++) {
        for (int c = 0; c < cols; c++) {
            if (c) out << ',';
            writeField(model->index(r, c).data(Qt::DisplayRole).toString());
        }
        out << '\n';
    }
    return true;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("KumDB Studio");
    resize(1100, 700);

    auto *menuFile = menuBar()->addMenu("&File");
    menuFile->addAction("&Open Database...", this, &MainWindow::openDatabase);
    menuFile->addAction("&New Database...", this, &MainWindow::newDatabase);
    menuFile->addAction("&Close Database", this, &MainWindow::closeDatabase);
    menuFile->addSeparator();
    menuFile->addAction("&Export Datasheet as CSV...", this, &MainWindow::exportDatasheetCsv);
    menuFile->addSeparator();
    menuFile->addAction("E&xit", this, &QWidget::close);

    auto *menuTable = menuBar()->addMenu("&Table");
    menuTable->addAction("&New Table...", this, &MainWindow::newTable);
    menuTable->addAction("&Drop Table", this, &MainWindow::dropTable);
    menuTable->addAction("&Compact Table", this, &MainWindow::compactTable);
    menuTable->addAction("&Refresh", this, &MainWindow::refreshTables);

    auto *menuHelp = menuBar()->addMenu("&Help");
    menuHelp->addAction("&About KumDB Studio", this, &MainWindow::showAbout);

    QStyle *st = style();
    auto *toolbar = addToolBar("Main");
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->addAction(st->standardIcon(QStyle::SP_DialogOpenButton), "Open", this, &MainWindow::openDatabase);
    toolbar->addAction(st->standardIcon(QStyle::SP_FileDialogNewFolder), "New DB", this, &MainWindow::newDatabase);
    toolbar->addAction(st->standardIcon(QStyle::SP_DialogCloseButton), "Close", this, &MainWindow::closeDatabase);
    toolbar->addSeparator();
    toolbar->addAction(st->standardIcon(QStyle::SP_FileIcon), "New Table", this, &MainWindow::newTable);
    toolbar->addAction(st->standardIcon(QStyle::SP_TrashIcon), "Drop Table", this, &MainWindow::dropTable);
    toolbar->addAction(st->standardIcon(QStyle::SP_BrowserReload), "Refresh", this, &MainWindow::refreshTables);

    m_centralStack = new QStackedWidget(this);

    // ---- Empty state: shown until a database is open ----
    auto *welcomePage = new QWidget(m_centralStack);
    auto *welcomeLayout = new QVBoxLayout(welcomePage);
    welcomeLayout->addStretch();
    auto *welcomeIcon = new QLabel(welcomePage);
    welcomeIcon->setPixmap(st->standardIcon(QStyle::SP_DriveHDIcon).pixmap(64, 64));
    welcomeIcon->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(welcomeIcon);
    auto *welcomeTitle = new QLabel("No database open", welcomePage);
    welcomeTitle->setObjectName("emptyStateTitle");
    welcomeTitle->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(welcomeTitle);
    auto *welcomeHint = new QLabel("Open an existing KumDB folder, or create a new one to get started.", welcomePage);
    welcomeHint->setObjectName("emptyStateHint");
    welcomeHint->setAlignment(Qt::AlignCenter);
    welcomeLayout->addWidget(welcomeHint);
    auto *welcomeButtons = new QHBoxLayout;
    welcomeButtons->addStretch();
    auto *openBtn = new QPushButton("Open Database...", welcomePage);
    openBtn->setObjectName("primaryButton");
    auto *newDbBtn = new QPushButton("New Database...", welcomePage);
    welcomeButtons->addWidget(openBtn);
    welcomeButtons->addWidget(newDbBtn);
    welcomeButtons->addStretch();
    welcomeLayout->addSpacing(12);
    welcomeLayout->addLayout(welcomeButtons);
    welcomeLayout->addStretch();
    connect(openBtn, &QPushButton::clicked, this, &MainWindow::openDatabase);
    connect(newDbBtn, &QPushButton::clicked, this, &MainWindow::newDatabase);
    m_centralStack->addWidget(welcomePage);

    // ---- Main workspace: shown once a database is open ----
    auto *splitter = new QSplitter(m_centralStack);

    auto *leftPanel = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    auto *tablesHeaderRow = new QHBoxLayout;
    auto *tablesHeader = new QLabel("TABLES", leftPanel);
    tablesHeader->setObjectName("sectionHeader");
    tablesHeaderRow->addWidget(tablesHeader);
    tablesHeaderRow->addStretch();
    m_tableCountLabel = new QLabel("0", leftPanel);
    m_tableCountLabel->setObjectName("sectionHeader");
    tablesHeaderRow->addWidget(m_tableCountLabel);
    leftLayout->addLayout(tablesHeaderRow);

    auto *filterEdit = new QLineEdit(leftPanel);
    filterEdit->setPlaceholderText("Filter tables...");
    filterEdit->setClearButtonEnabled(true);
    leftLayout->addWidget(filterEdit);
    connect(filterEdit, &QLineEdit::textChanged, this, &MainWindow::filterTableList);

    m_tableList = new QListWidget(leftPanel);
    leftLayout->addWidget(m_tableList);
    auto *leftButtons = new QHBoxLayout;
    auto *newTableBtn = new QPushButton("+ New", leftPanel);
    auto *dropTableBtn = new QPushButton("Drop", leftPanel);
    dropTableBtn->setObjectName("dangerButton");
    leftButtons->addWidget(newTableBtn);
    leftButtons->addWidget(dropTableBtn);
    leftLayout->addLayout(leftButtons);
    connect(newTableBtn, &QPushButton::clicked, this, &MainWindow::newTable);
    connect(dropTableBtn, &QPushButton::clicked, this, &MainWindow::dropTable);
    connect(m_tableList, &QListWidget::itemClicked, this, &MainWindow::onTableSelected);

    auto *tabs = new QTabWidget(splitter);

    // Datasheet tab
    auto *datasheetPage = new QWidget(tabs);
    auto *datasheetLayout = new QVBoxLayout(datasheetPage);
    auto *datasheetButtons = new QHBoxLayout;
    auto *addRowBtn = new QPushButton("+ Add row...", datasheetPage);
    addRowBtn->setToolTip("Fill several fields at once via a dialog -- or just start typing into the * row at the bottom of the grid.");
    auto *deleteRowBtn = new QPushButton("- Delete row", datasheetPage);
    deleteRowBtn->setObjectName("dangerButton");
    auto *refreshBtn = new QPushButton("Refresh", datasheetPage);
    auto *exportBtn = new QPushButton("Export CSV...", datasheetPage);
    auto *compactBtn = new QPushButton("Compact", datasheetPage);
    datasheetButtons->addWidget(addRowBtn);
    datasheetButtons->addWidget(deleteRowBtn);
    datasheetButtons->addWidget(refreshBtn);
    datasheetButtons->addStretch();
    m_datasheetSearch = new QLineEdit(datasheetPage);
    m_datasheetSearch->setPlaceholderText("Search this table...");
    m_datasheetSearch->setClearButtonEnabled(true);
    m_datasheetSearch->setMaximumWidth(220);
    datasheetButtons->addWidget(m_datasheetSearch);
    datasheetButtons->addWidget(exportBtn);
    datasheetButtons->addWidget(compactBtn);
    datasheetLayout->addLayout(datasheetButtons);

    m_datasheetModel = new RowTableModel(&m_db, this);
    m_datasheet = new QTableView(datasheetPage);
    m_datasheet->setModel(m_datasheetModel);
    m_datasheet->horizontalHeader()->setStretchLastSection(true);
    m_datasheet->setAlternatingRowColors(true);
    m_datasheet->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_datasheet->setSortingEnabled(false); // RowTableModel isn't a proxy-sortable model -- avoid a misleading sort arrow
    datasheetLayout->addWidget(m_datasheet);

    // Record navigator -- Access's own First/Prev/Next/Last + "Row N of M"
    // strip at the bottom of every datasheet.
    auto *navRow = new QHBoxLayout;
    QStyle *navSt = style();
    auto *navFirst = new QToolButton(datasheetPage);
    navFirst->setIcon(navSt->standardIcon(QStyle::SP_MediaSkipBackward));
    navFirst->setToolTip("First record");
    auto *navPrev = new QToolButton(datasheetPage);
    navPrev->setIcon(navSt->standardIcon(QStyle::SP_MediaSeekBackward));
    navPrev->setToolTip("Previous record");
    auto *navNext = new QToolButton(datasheetPage);
    navNext->setIcon(navSt->standardIcon(QStyle::SP_MediaSeekForward));
    navNext->setToolTip("Next record");
    auto *navLast = new QToolButton(datasheetPage);
    navLast->setIcon(navSt->standardIcon(QStyle::SP_MediaSkipForward));
    navLast->setToolTip("Last record");
    auto *navNew = new QToolButton(datasheetPage);
    navNew->setIcon(navSt->standardIcon(QStyle::SP_FileIcon));
    navNew->setToolTip("New record");
    m_recordNavLabel = new QLabel("No records", datasheetPage);
    navRow->addWidget(navFirst);
    navRow->addWidget(navPrev);
    navRow->addWidget(navNext);
    navRow->addWidget(navLast);
    navRow->addWidget(navNew);
    navRow->addWidget(m_recordNavLabel);
    navRow->addStretch();
    datasheetLayout->addLayout(navRow);

    tabs->addTab(datasheetPage, "Datasheet");

    connect(addRowBtn, &QPushButton::clicked, this, &MainWindow::addRowViaDatasheet);
    connect(deleteRowBtn, &QPushButton::clicked, this, &MainWindow::deleteSelectedRow);
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::reloadDatasheet);
    connect(exportBtn, &QPushButton::clicked, this, &MainWindow::exportDatasheetCsv);
    connect(compactBtn, &QPushButton::clicked, this, &MainWindow::compactTable);
    connect(m_datasheetSearch, &QLineEdit::textChanged, this, &MainWindow::filterDatasheetRows);
    connect(m_datasheetModel, &RowTableModel::editFailed, this, [this](const QString &err) {
        QMessageBox::warning(this, "Edit failed", err);
    });
    // Deferred (queued): a field just committed on the model's own blank
    // new-record row triggers this, and the model must finish its own
    // setData()/edit-commit cycle before we're allowed to reset it.
    connect(m_datasheetModel, &RowTableModel::newRowInserted, this, &MainWindow::reloadDatasheet, Qt::QueuedConnection);
    connect(m_datasheet->selectionModel(), &QItemSelectionModel::currentRowChanged, this, &MainWindow::updateRecordNav);

    auto moveRow = [this](int row) {
        if (row < 0 || row >= m_datasheetModel->rowCount()) return;
        m_datasheet->setCurrentIndex(m_datasheetModel->index(row, 1));
        m_datasheet->scrollTo(m_datasheetModel->index(row, 0));
    };
    connect(navFirst, &QToolButton::clicked, this, [this, moveRow]() { moveRow(0); });
    connect(navPrev, &QToolButton::clicked, this, [this, moveRow]() { moveRow(m_datasheet->currentIndex().row() - 1); });
    connect(navNext, &QToolButton::clicked, this, [this, moveRow]() { moveRow(m_datasheet->currentIndex().row() + 1); });
    connect(navLast, &QToolButton::clicked, this, [this, moveRow]() { moveRow(m_datasheetModel->rowCount() - 1); });
    connect(navNew, &QToolButton::clicked, this, [this, moveRow]() { moveRow(m_datasheetModel->rowCount() - 1); });

    // Schema tab -- read-only view of a table's full column metadata,
    // including constraints the datasheet grid has no room to show.
    auto *schemaPage = new QWidget(tabs);
    auto *schemaLayout = new QVBoxLayout(schemaPage);
    m_schemaView = new QTableWidget(0, 5, schemaPage);
    m_schemaView->setHorizontalHeaderLabels({"Column", "Type", "Nullable", "Indexed", "Unique"});
    m_schemaView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_schemaView->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_schemaView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_schemaView->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_schemaView->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_schemaView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_schemaView->setAlternatingRowColors(true);
    m_schemaView->verticalHeader()->setVisible(false);
    schemaLayout->addWidget(m_schemaView);
    tabs->addTab(schemaPage, "Schema");

    m_queryPanel = new QueryPanel(&m_db, tabs);
    tabs->addTab(m_queryPanel, "Query");
    connect(m_queryPanel, &QueryPanel::statusMessage, this, [this](const QString &msg) {
        m_statusLabel->setText(msg);
    });

    splitter->addWidget(leftPanel);
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({240, 860});
    m_centralStack->addWidget(splitter);

    setCentralWidget(m_centralStack);

    m_statusLabel = new QLabel("No database open.", this);
    statusBar()->addWidget(m_statusLabel, 1);
    m_connIndicator = new QLabel(this);
    statusBar()->addPermanentWidget(m_connIndicator);
    auto *versionLabel = new QLabel(QString("KumDB v%1").arg(QString::fromUtf8(kdb_version())), this);
    versionLabel->setStyleSheet("color: #6b6e73;");
    statusBar()->addPermanentWidget(versionLabel);

    setConnectionIndicator(false);
    setDbLoadedState(false);
}

void MainWindow::setConnectionIndicator(bool connected, const QString &dir) {
    if (connected) {
        m_connIndicator->setObjectName("connIndicatorOn");
        m_connIndicator->setText("● " + dir);
    } else {
        m_connIndicator->setObjectName("connIndicatorOff");
        m_connIndicator->setText("○ Disconnected");
    }
    // objectName-based QSS rules only re-apply after a style polish
    m_connIndicator->style()->unpolish(m_connIndicator);
    m_connIndicator->style()->polish(m_connIndicator);
}

void MainWindow::setDbLoadedState(bool loaded) {
    m_centralStack->setCurrentIndex(loaded ? 1 : 0);
}

void MainWindow::openDir(const QString &dir) {
    QString err;
    if (!m_db.open(dir, &err)) {
        QMessageBox::critical(this, "Open Database", err);
        return;
    }
    setDbLoadedState(true);
    setConnectionIndicator(true, dir);
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
    m_datasheetSearch->clear();
    m_datasheetModel->clear();
    m_schemaView->setRowCount(0);
    updateRecordNav();
    setDbLoadedState(false);
    setConnectionIndicator(false);
    m_statusLabel->setText("No database open.");
}

void MainWindow::refreshTables() {
    if (!m_db.isOpen()) return;
    QString current = currentTableName();
    m_tableList->clear();
    QStringList tables = m_db.listTables();
    m_tableList->addItems(tables);
    m_tableCountLabel->setText(QString::number(tables.size()));

    // setCurrentItem() alone doesn't fire itemClicked, so restoring (or
    // picking) a selection here used to leave the datasheet/schema tabs
    // showing whatever they last had -- stale data after a rename/drop, or
    // just an empty "id" placeholder on first opening a database that
    // already has tables. Drive the same reload onTableSelected does
    // whenever a selection actually gets (re)established.
    QListWidgetItem *toSelect = nullptr;
    if (!current.isEmpty()) {
        auto items = m_tableList->findItems(current, Qt::MatchExactly);
        if (!items.isEmpty()) toSelect = items.first();
    }
    if (!toSelect && m_tableList->count() > 0) toSelect = m_tableList->item(0);

    if (toSelect) {
        m_tableList->setCurrentItem(toSelect);
        onTableSelected(toSelect);
    } else {
        m_datasheetSearch->clear();
        m_datasheetModel->clear();
        m_schemaView->setRowCount(0);
    }

    m_queryPanel->refreshTableList();
}

void MainWindow::filterTableList(const QString &text) {
    for (int i = 0; i < m_tableList->count(); i++) {
        QListWidgetItem *item = m_tableList->item(i);
        item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive));
    }
}

QString MainWindow::currentTableName() const {
    auto *item = m_tableList->currentItem();
    return item ? item->text() : QString();
}

void MainWindow::onTableSelected(QListWidgetItem *) {
    m_datasheetSearch->clear(); // a stale filter from the previous table would just hide everything here
    reloadDatasheet();
    reloadSchemaView();
}

void MainWindow::reloadSchemaView() {
    QString table = currentTableName();
    m_schemaView->setRowCount(0);
    if (table.isEmpty() || !m_db.isOpen()) return;

    QVector<KColumnMeta> schema = m_db.schema(table);
    m_schemaView->setRowCount(schema.size());
    for (int i = 0; i < schema.size(); i++) {
        const auto &c = schema[i];
        m_schemaView->setItem(i, 0, new QTableWidgetItem(c.name));
        m_schemaView->setItem(i, 1, new QTableWidgetItem(typeName(c.type)));
        m_schemaView->setItem(i, 2, new QTableWidgetItem(c.nullable ? "yes" : "no"));
        m_schemaView->setItem(i, 3, new QTableWidgetItem(c.indexed ? "yes" : "no"));
        m_schemaView->setItem(i, 4, new QTableWidgetItem(c.unique ? "yes" : "no"));
    }
}

void MainWindow::reloadDatasheet() {
    QString table = currentTableName();
    if (table.isEmpty() || !m_db.isOpen()) {
        m_datasheetModel->clear();
        updateRecordNav();
        return;
    }

    QVector<KColumnMeta> schema = m_db.schema(table);
    QStringList columns, readOnlyColumns;
    for (const auto &c : schema) {
        columns << c.name;
        if (c.type == KDB_TYPE_ARRAY || c.type == KDB_TYPE_OBJECT) readOnlyColumns << c.name;
    }

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

    m_datasheetModel->setData(table, columns, rows, true, readOnlyColumns);
    m_datasheet->resizeColumnsToContents();
    m_statusLabel->setText(QString("%1: %2 row(s)").arg(table).arg(rows.size()));
    filterDatasheetRows(m_datasheetSearch->text()); // model reset above drops any prior row-hidden state
    updateRecordNav();
}

// Access-style instant filter: hides any row where no cell contains text
// (case-insensitive), leaving the trailing blank new-record row visible no
// matter what's typed -- you should always be able to add a row while
// filtering. No proxy model needed since row indices stay stable (hidden
// rows are still real rows at the same index, just not painted).
void MainWindow::filterDatasheetRows(const QString &text) {
    int realRows = m_datasheetModel->realRowCount();
    for (int r = 0; r < realRows; r++) {
        bool hide = false;
        if (!text.isEmpty()) {
            hide = true;
            for (int c = 0; c < m_datasheetModel->columnCount() && hide; c++) {
                if (m_datasheetModel->index(r, c).data(Qt::DisplayRole).toString().contains(text, Qt::CaseInsensitive))
                    hide = false;
            }
        }
        m_datasheet->setRowHidden(r, hide);
    }
    updateRecordNav();
}

void MainWindow::updateRecordNav() {
    int total = m_datasheetModel->realRowCount();
    int row = m_datasheet->currentIndex().isValid() ? m_datasheet->currentIndex().row() : -1;
    if (total == 0) {
        m_recordNavLabel->setText(m_datasheetModel->isNewRecordRow(0) ? "No records yet -- type into the * row" : "No records");
    } else if (row < 0 || m_datasheetModel->isNewRecordRow(row)) {
        m_recordNavLabel->setText(QString("%1 record(s)").arg(total));
    } else {
        m_recordNavLabel->setText(QString("Row %1 of %2").arg(row + 1).arg(total));
    }
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

void MainWindow::exportDatasheetCsv() {
    if (m_datasheetModel->rowCount() == 0) {
        QMessageBox::information(this, "Export CSV", "Nothing to export -- select a table with rows first.");
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, "Export Datasheet as CSV",
                                                 currentTableName() + ".csv", "CSV files (*.csv)");
    if (path.isEmpty()) return;

    QString err;
    if (!writeModelToCsv(m_datasheetModel, path, &err)) {
        QMessageBox::warning(this, "Export CSV", err);
        return;
    }
    m_statusLabel->setText("Exported to " + path);
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
    refreshTables(); // picks a remaining table (if any) instead of leaving the dropped one's view up
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
                "run NoSQL filters or full SQL against the same embedded database file.<br><br>"
                "SQL supports JOINs, subqueries, CTEs, window functions, transactions, and more -- "
                "use the Query tab's SQL mode for anything the datasheet grid doesn't cover.")
            .arg(QString::fromUtf8(kdb_version())));
}
