#pragma once

#include <QMainWindow>

#include "KumDbHandle.h"
#include "RowTableModel.h"

class QListWidget;
class QTableView;
class QTableWidget;
class QLabel;
class QueryPanel;
class QListWidgetItem;
class QStackedWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Opens dir at startup, same as picking it via File > Open Database --
    // what main.cpp calls when a database folder is passed on the command
    // line (see README.md's "Run it" section).
    void openInitialDatabase(const QString &dir) { openDir(dir); }

private slots:
    void openDatabase();
    void newDatabase();
    void closeDatabase();
    void newTable();
    void dropTable();
    void compactTable();
    void refreshTables();
    void onTableSelected(QListWidgetItem *item);
    void reloadDatasheet();
    void addRowViaDatasheet();
    void deleteSelectedRow();
    void exportDatasheetCsv();
    void filterTableList(const QString &text);
    void showAbout();

private:
    void openDir(const QString &dir);
    void setDbLoadedState(bool loaded);
    QString currentTableName() const;
    void reloadSchemaView();
    void setConnectionIndicator(bool connected, const QString &dir = QString());

    KumDbHandle     m_db;
    QStackedWidget  *m_centralStack;
    QListWidget     *m_tableList;
    QLabel          *m_tableCountLabel;
    QTableView      *m_datasheet;
    RowTableModel   *m_datasheetModel;
    QTableWidget    *m_schemaView;
    QueryPanel      *m_queryPanel;
    QLabel          *m_statusLabel;
    QLabel          *m_connIndicator;
};
