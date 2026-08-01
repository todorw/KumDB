#pragma once

#include <QMainWindow>

#include "KumDbHandle.h"
#include "RowTableModel.h"

class QListWidget;
class QTableView;
class QLabel;
class QueryPanel;
class QListWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

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
    void showAbout();

private:
    void openDir(const QString &dir);
    void setDbLoadedState(bool loaded);
    QString currentTableName() const;

    KumDbHandle    m_db;
    QListWidget    *m_tableList;
    QTableView     *m_datasheet;
    RowTableModel  *m_datasheetModel;
    QueryPanel     *m_queryPanel;
    QLabel         *m_statusLabel;
};
