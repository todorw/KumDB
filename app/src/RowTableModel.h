#pragma once

#include <QAbstractTableModel>
#include <QVector>
#include <QStringList>

#include "KumDbHandle.h"

// Displays a set of KRow records as an editable grid. Column set is fixed
// at construction (from the table's schema, or from whatever fields the
// first row happens to have for a SQL SELECT with no schema handy).
// Editing a cell round-trips through KumDbHandle::updateCell() immediately
// -- there's no separate "save" step, same as Access's datasheet view.
//
// When editable, an extra blank row is appended at the end (Access's own
// "*" new-record row): typing a value into any of its cells inserts a new
// row via KumDbHandle::addRow() with just that one field set, then asks
// the owner to reload (newRowInserted()) so the freshly-inserted row takes
// its real place and a fresh blank row appears below it. Not a literal
// clone of Access's behavior (which stages a whole record before
// committing on row-exit) -- this reuses the same "every edit commits
// immediately" rule the rest of the grid already follows, just extended
// to the blank row too.
class RowTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit RowTableModel(KumDbHandle *db, QObject *parent = nullptr);

    // readOnlyColumns: columns that must never be edited in place even when
    // editable is true (ARRAY/OBJECT columns -- the grid shows them as text,
    // but writing that text back would silently replace the structured
    // value with a plain string).
    void setData(const QString &table, const QStringList &columns, const QVector<KRow> &rows,
                bool editable, const QStringList &readOnlyColumns = {});
    void clear();

    quint64 idAt(int row) const;
    QString columnNameAt(int col) const;
    int realRowCount() const { return m_rows.size(); } // excludes the trailing blank new-record row
    bool isNewRecordRow(int row) const { return m_editable && row == m_rows.size(); }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

signals:
    void editFailed(const QString &error);
    void rowChanged();
    void newRowInserted(); // a field was committed on the blank new-record row -- caller should reload

private:
    KumDbHandle  *m_db;
    QString       m_table;
    QStringList   m_columns;
    QVector<KRow> m_rows;
    bool          m_editable = false;
    QStringList   m_readOnlyColumns;
};
