#pragma once

#include <QDialog>
#include <QVector>

#include "KumDbHandle.h"

class QLineEdit;
class QTableWidget;

// Lets you define a table's schema before any data goes in: name, type,
// NOT NULL, and index, per column -- the same thing "CREATE TABLE" does
// in the SQL console, just with widgets instead of typing SQL.
class NewTableDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewTableDialog(QWidget *parent = nullptr);

    QString tableName() const;
    QVector<KColumnMeta> columns() const;

private slots:
    void addColumnRow();
    void removeSelectedColumnRow();

private:
    QLineEdit    *m_nameEdit;
    QTableWidget *m_colTable;
};
