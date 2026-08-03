#include "RowTableModel.h"

RowTableModel::RowTableModel(KumDbHandle *db, QObject *parent)
    : QAbstractTableModel(parent), m_db(db) {}

void RowTableModel::setData(const QString &table, const QStringList &columns, const QVector<KRow> &rows,
                            bool editable, const QStringList &readOnlyColumns) {
    beginResetModel();
    m_table = table;
    m_columns = columns;
    m_rows = rows;
    m_editable = editable;
    m_readOnlyColumns = readOnlyColumns;
    endResetModel();
}

void RowTableModel::clear() {
    beginResetModel();
    m_table.clear();
    m_columns.clear();
    m_rows.clear();
    m_editable = false;
    m_readOnlyColumns.clear();
    endResetModel();
}

quint64 RowTableModel::idAt(int row) const {
    if (row < 0 || row >= m_rows.size()) return 0;
    return m_rows[row].id;
}

QString RowTableModel::columnNameAt(int col) const {
    if (col < 0 || col >= m_columns.size()) return {};
    return m_columns[col];
}

int RowTableModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return m_rows.size() + (m_editable ? 1 : 0); // +1 for the trailing blank new-record row
}

int RowTableModel::columnCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return m_columns.size() + 1; // +1 for the leading id column
}

static const QPair<QString, QVariant> *findField(const KRow &row, const QString &name) {
    for (const auto &f : row.fields)
        if (f.first == name) return &f;
    return nullptr;
}

QVariant RowTableModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};

    if (isNewRecordRow(index.row())) {
        // blank placeholder row -- nothing to show yet, it isn't a real record
        return {};
    }

    const KRow &row = m_rows[index.row()];

    if (index.column() == 0) {
        if (role == Qt::DisplayRole) return QVariant::fromValue<qulonglong>(row.id);
        return {};
    }

    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};

    const QString colName = m_columns[index.column() - 1];
    const auto *f = findField(row, colName);
    if (!f) return {};
    return f->second;
}

bool RowTableModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (role != Qt::EditRole || !index.isValid() || index.column() == 0) return false;
    if (!m_editable) return false;

    const QString colName = m_columns[index.column() - 1];
    if (m_readOnlyColumns.contains(colName)) return false;

    if (isNewRecordRow(index.row())) {
        if (!value.isValid() || value.isNull()) return false; // nothing typed -- don't insert an all-NULL row
        QString err;
        if (!m_db->addRow(m_table, {{colName, value}}, &err)) {
            emit editFailed(err);
            return false;
        }
        emit newRowInserted();
        return true;
    }

    quint64 id = idAt(index.row());

    QString err;
    if (!m_db->updateCell(m_table, id, colName, value, &err)) {
        emit editFailed(err);
        return false;
    }

    // reflect the edit locally rather than re-querying
    KRow &row = m_rows[index.row()];
    bool found = false;
    for (auto &f : row.fields) {
        if (f.first == colName) { f.second = value; found = true; break; }
    }
    if (!found) row.fields.append({colName, value});

    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    emit rowChanged();
    return true;
}

QVariant RowTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal) {
        if (role != Qt::DisplayRole) return {};
        if (section == 0) return QStringLiteral("id");
        if (section - 1 < m_columns.size()) return m_columns[section - 1];
        return {};
    }
    if (role == Qt::DisplayRole) return isNewRecordRow(section) ? QStringLiteral("*") : QString::number(section + 1);
    return {};
}

Qt::ItemFlags RowTableModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() != 0 && m_editable) {
        const QString colName = m_columns[index.column() - 1];
        if (!m_readOnlyColumns.contains(colName)) f |= Qt::ItemIsEditable;
    }
    return f;
}
