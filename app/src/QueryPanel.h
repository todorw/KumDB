#pragma once

#include <QWidget>

#include "KumDbHandle.h"
#include "RowTableModel.h"

class QComboBox;
class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QTableView;
class QStackedWidget;

// Run a query either the NoSQL way (pick a table, type filter lines like
// "age__gt=21") or the SQL way (type a whole statement) -- same engine
// either way, this panel just decides which parsing path kdb gets to see.
class QueryPanel : public QWidget {
    Q_OBJECT
public:
    explicit QueryPanel(KumDbHandle *db, QWidget *parent = nullptr);

    void refreshTableList();

signals:
    void statusMessage(const QString &msg);

private slots:
    void runQuery();
    void onModeChanged(int index);

private:
    KumDbHandle    *m_db;
    QComboBox      *m_modeBox;
    QStackedWidget *m_inputStack;
    QComboBox      *m_tableBox;
    QPlainTextEdit *m_filterEdit;
    QPlainTextEdit *m_sqlEdit;
    QTableView     *m_resultsView;
    RowTableModel  *m_resultsModel;
    QLabel         *m_infoLabel;
};
