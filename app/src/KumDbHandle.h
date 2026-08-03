#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QPair>

extern "C" {
#include "kumdb.h"
#include "sql.h"
}

struct KColumnMeta {
    QString      name;
    KdbFieldType type = KDB_TYPE_STRING;
    bool         nullable = true;
    bool         indexed = false;
    bool         unique = false;
};

struct KRow {
    quint64 id = 0;
    quint64 createdAt = 0;
    quint64 updatedAt = 0;
    QVector<QPair<QString, QVariant>> fields;
};

// Thin C++/Qt wrapper around the C API. Keeps all kumdb.h/sql.h interop
// (encoding, pointer lifetimes, error-string plumbing) in one place so the
// rest of the app only ever deals in QString/QVariant/KRow.
class KumDbHandle {
public:
    KumDbHandle() = default;
    ~KumDbHandle();
    KumDbHandle(const KumDbHandle &) = delete;
    KumDbHandle &operator=(const KumDbHandle &) = delete;

    bool open(const QString &dir, QString *errorOut = nullptr);
    void close();
    bool isOpen() const { return m_db != nullptr; }
    QString dataDir() const { return m_dataDir; }

    QStringList listTables() const;
    bool tableExists(const QString &name) const;
    QVector<KColumnMeta> schema(const QString &table) const;

    bool createTable(const QString &name, const QVector<KColumnMeta> &cols, QString *errorOut = nullptr);
    bool dropTable(const QString &name, QString *errorOut = nullptr);
    bool compactTable(const QString &name, QString *errorOut = nullptr);

    QVector<KRow> find(const QString &table, const QStringList &filters, QString *errorOut = nullptr);
    bool addRow(const QString &table, const QVector<QPair<QString, QVariant>> &fields, QString *errorOut = nullptr);
    bool updateCell(const QString &table, quint64 id, const QString &col, const QVariant &value, QString *errorOut = nullptr);
    bool deleteRow(const QString &table, quint64 id, QString *errorOut = nullptr);

    struct SqlResult {
        bool ok = false;
        bool hasRows = false;
        QVector<KRow> rows;
        size_t affected = 0;
        QString error;
    };
    SqlResult execSql(const QString &stmt);

private:
    KumDB  *m_db = nullptr;
    QString m_dataDir;

    static QVector<KRow> rowsFromKdbRows(KdbRows *rows);
    static QVariant valueFromField(const KdbField &f);
};
