#include "KumDbHandle.h"

KumDbHandle::~KumDbHandle() { close(); }

bool KumDbHandle::open(const QString &dir, QString *errorOut) {
    close();
    m_db = kdb_open(dir.toUtf8().constData());
    if (!m_db) {
        if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error());
        return false;
    }
    m_dataDir = dir;
    return true;
}

void KumDbHandle::close() {
    if (m_db) {
        kdb_close(m_db);
        m_db = nullptr;
    }
    m_dataDir.clear();
}

/* Compact JSON-ish text, read-only display only -- the datasheet grid edits
 * scalar cells in place, it doesn't offer a nested-value editor. Good enough
 * to see what's there without silently showing a blank cell. */
static QString fieldToDisplayString(const KdbField &f);

static QString arrayToDisplayString(const KdbField &f) {
    QStringList parts;
    for (size_t i = 0; i < f.v.as_array.count; i++)
        parts << fieldToDisplayString(f.v.as_array.items[i]);
    return "[" + parts.join(", ") + "]";
}

static QString objectToDisplayString(const KdbField &f) {
    QStringList parts;
    if (f.v.as_object) {
        for (const KdbField *sub = f.v.as_object; sub->name != nullptr; sub++)
            parts << QString("%1: %2").arg(QString::fromUtf8(sub->name), fieldToDisplayString(*sub));
    }
    return "{" + parts.join(", ") + "}";
}

static QString fieldToDisplayString(const KdbField &f) {
    switch (f.type) {
        case KDB_TYPE_INT:    return QString::number(f.v.as_int);
        case KDB_TYPE_FLOAT:  return QString::number(f.v.as_float);
        case KDB_TYPE_BOOL:   return f.v.as_bool ? "true" : "false";
        case KDB_TYPE_STRING: return QString("\"%1\"").arg(QString::fromUtf8(f.v.as_string ? f.v.as_string : ""));
        case KDB_TYPE_BLOB:   return QString("<blob: %1 bytes>").arg((qulonglong)f.v.as_blob.len);
        case KDB_TYPE_ARRAY:  return arrayToDisplayString(f);
        case KDB_TYPE_OBJECT: return objectToDisplayString(f);
        case KDB_TYPE_NULL:
        default:              return "null";
    }
}

QVariant KumDbHandle::valueFromField(const KdbField &f) {
    switch (f.type) {
        case KDB_TYPE_INT:    return QVariant::fromValue<qint64>(f.v.as_int);
        case KDB_TYPE_FLOAT:  return QVariant(f.v.as_float);
        case KDB_TYPE_BOOL:   return QVariant(f.v.as_bool != 0);
        case KDB_TYPE_STRING: return QVariant(QString::fromUtf8(f.v.as_string ? f.v.as_string : ""));
        case KDB_TYPE_BLOB:   return QVariant(QString("<blob: %1 bytes>").arg((qulonglong)f.v.as_blob.len));
        case KDB_TYPE_ARRAY:  return QVariant(arrayToDisplayString(f));
        case KDB_TYPE_OBJECT: return QVariant(objectToDisplayString(f));
        case KDB_TYPE_NULL:
        default:              return QVariant();
    }
}

QVector<KRow> KumDbHandle::rowsFromKdbRows(KdbRows *rows) {
    QVector<KRow> out;
    if (!rows) return out;
    out.reserve((int)rows->count);
    for (size_t i = 0; i < rows->count; i++) {
        const KdbRow &r = rows->rows[i];
        KRow kr;
        kr.id = r.id;
        kr.createdAt = r.created_at;
        kr.updatedAt = r.updated_at;
        kr.fields.reserve((int)r.field_count);
        for (uint32_t j = 0; j < r.field_count; j++) {
            kr.fields.append({QString::fromUtf8(r.fields[j].name ? r.fields[j].name : ""),
                               valueFromField(r.fields[j])});
        }
        out.append(kr);
    }
    return out;
}

QStringList KumDbHandle::listTables() const {
    QStringList out;
    if (!m_db) return out;
    const char *names[256];
    size_t count = 0;
    if (kdb_list_tables(m_db, names, 256, &count) != KDB_OK) return out;
    out.reserve((int)count);
    for (size_t i = 0; i < count; i++) out << QString::fromUtf8(names[i]);
    return out;
}

bool KumDbHandle::tableExists(const QString &name) const {
    if (!m_db) return false;
    return kdb_table_exists(m_db, name.toUtf8().constData()) != 0;
}

QVector<KColumnMeta> KumDbHandle::schema(const QString &table) const {
    QVector<KColumnMeta> out;
    if (!m_db) return out;
    KdbColumnInfo cols[64];
    uint32_t count = 0;
    if (kdb_get_schema(m_db, table.toUtf8().constData(), cols, 64, &count) != KDB_OK) return out;
    out.reserve((int)count);
    for (uint32_t i = 0; i < count; i++) {
        KColumnMeta m;
        m.name = QString::fromUtf8(cols[i].name);
        m.type = cols[i].type;
        m.nullable = cols[i].nullable != 0;
        m.indexed = cols[i].indexed != 0;
        out.append(m);
    }
    return out;
}

bool KumDbHandle::createTable(const QString &name, const QVector<KColumnMeta> &cols, QString *errorOut) {
    if (!m_db) { if (errorOut) *errorOut = "No database open"; return false; }

    QVector<QByteArray> nameBufs;
    nameBufs.reserve(cols.size());
    QVector<KdbColumnDef> defs;
    defs.reserve(cols.size());

    for (const auto &c : cols) {
        nameBufs.append(c.name.toUtf8());
        KdbColumnDef d;
        d.name = nameBufs.last().constData();
        d.type = c.type;
        d.nullable = c.nullable ? 1 : 0;
        d.indexed  = c.indexed  ? 1 : 0;
        d.unique   = 0; /* not exposed in the New Table dialog yet -- indexed/nullable only */
        defs.append(d);
    }

    KdbStatus st = kdb_create_table(m_db, name.toUtf8().constData(), defs.data(), (uint32_t)defs.size());
    if (st != KDB_OK) { if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error()); return false; }
    return true;
}

bool KumDbHandle::dropTable(const QString &name, QString *errorOut) {
    if (!m_db) { if (errorOut) *errorOut = "No database open"; return false; }
    KdbStatus st = kdb_drop_table(m_db, name.toUtf8().constData());
    if (st != KDB_OK) { if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error()); return false; }
    return true;
}

bool KumDbHandle::compactTable(const QString &name, QString *errorOut) {
    if (!m_db) { if (errorOut) *errorOut = "No database open"; return false; }
    KdbStatus st = kdb_compact(m_db, name.toUtf8().constData());
    if (st != KDB_OK) { if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error()); return false; }
    return true;
}

QVector<KRow> KumDbHandle::find(const QString &table, const QStringList &filters, QString *errorOut) {
    if (!m_db) { if (errorOut) *errorOut = "No database open"; return {}; }

    QVector<QByteArray> filterBufs;
    filterBufs.reserve(filters.size());
    QVector<const char *> filterPtrs;
    filterPtrs.reserve(filters.size() + 1);
    for (const QString &f : filters) {
        if (f.trimmed().isEmpty()) continue;
        filterBufs.append(f.trimmed().toUtf8());
        filterPtrs.append(filterBufs.last().constData());
    }
    filterPtrs.append(nullptr);

    KdbRows *rows = kdb_find(m_db, table.toUtf8().constData(),
                             filterPtrs.size() > 1 ? filterPtrs.data() : nullptr);
    if (!rows) { if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error()); return {}; }
    QVector<KRow> out = rowsFromKdbRows(rows);
    kdb_rows_free(rows);
    return out;
}

bool KumDbHandle::addRow(const QString &table, const QVector<QPair<QString, QVariant>> &fields, QString *errorOut) {
    if (!m_db) { if (errorOut) *errorOut = "No database open"; return false; }

    QVector<QByteArray> nameBufs, strBufs;
    nameBufs.reserve(fields.size());
    strBufs.reserve(fields.size());
    QVector<KdbField> kfields;
    kfields.reserve(fields.size() + 1);

    for (const auto &pair : fields) {
        nameBufs.append(pair.first.toUtf8());
        const char *nameC = nameBufs.last().constData();
        const QVariant &v = pair.second;

        switch (v.metaType().id()) {
            case QMetaType::Bool:
                kfields.append(kdb_field_bool(nameC, v.toBool() ? 1 : 0));
                break;
            case QMetaType::Int:
            case QMetaType::LongLong:
            case QMetaType::UInt:
            case QMetaType::ULongLong:
                kfields.append(kdb_field_int(nameC, v.toLongLong()));
                break;
            case QMetaType::Double:
            case QMetaType::Float:
                kfields.append(kdb_field_float(nameC, v.toDouble()));
                break;
            default:
                if (!v.isValid() || v.isNull()) {
                    kfields.append(kdb_field_null(nameC));
                } else {
                    strBufs.append(v.toString().toUtf8());
                    kfields.append(kdb_field_string(nameC, strBufs.last().constData()));
                }
                break;
        }
    }
    kfields.append(kdb_field_end());

    KdbStatus st = kdb_add(m_db, table.toUtf8().constData(), kfields.data());
    if (st != KDB_OK) { if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error()); return false; }
    return true;
}

bool KumDbHandle::updateCell(const QString &table, quint64 id, const QString &col, const QVariant &value, QString *errorOut) {
    if (!m_db) { if (errorOut) *errorOut = "No database open"; return false; }

    QByteArray colUtf8 = col.toUtf8();
    QByteArray strBuf;
    KdbField patch[2];

    switch (value.metaType().id()) {
        case QMetaType::Bool:
            patch[0] = kdb_field_bool(colUtf8.constData(), value.toBool() ? 1 : 0);
            break;
        case QMetaType::Int:
        case QMetaType::LongLong:
        case QMetaType::UInt:
        case QMetaType::ULongLong:
            patch[0] = kdb_field_int(colUtf8.constData(), value.toLongLong());
            break;
        case QMetaType::Double:
        case QMetaType::Float:
            patch[0] = kdb_field_float(colUtf8.constData(), value.toDouble());
            break;
        default:
            if (!value.isValid() || value.isNull()) {
                patch[0] = kdb_field_null(colUtf8.constData());
            } else {
                strBuf = value.toString().toUtf8();
                patch[0] = kdb_field_string(colUtf8.constData(), strBuf.constData());
            }
            break;
    }
    patch[1] = kdb_field_end();

    QByteArray idFilterUtf8 = QStringLiteral("id=%1").arg(id).toUtf8();
    const char *filters[] = {idFilterUtf8.constData(), nullptr};

    size_t updated = 0;
    KdbStatus st = kdb_update(m_db, table.toUtf8().constData(), filters, patch, &updated);
    if (st != KDB_OK) { if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error()); return false; }
    if (updated == 0) { if (errorOut) *errorOut = "No row updated (id not found?)"; return false; }
    return true;
}

bool KumDbHandle::deleteRow(const QString &table, quint64 id, QString *errorOut) {
    if (!m_db) { if (errorOut) *errorOut = "No database open"; return false; }

    QByteArray idFilterUtf8 = QStringLiteral("id=%1").arg(id).toUtf8();
    const char *filters[] = {idFilterUtf8.constData(), nullptr};

    size_t deleted = 0;
    KdbStatus st = kdb_delete(m_db, table.toUtf8().constData(), filters, &deleted);
    if (st != KDB_OK) { if (errorOut) *errorOut = QString::fromUtf8(kdb_last_error()); return false; }
    if (deleted == 0) { if (errorOut) *errorOut = "No row deleted (id not found?)"; return false; }
    return true;
}

KumDbHandle::SqlResult KumDbHandle::execSql(const QString &stmt) {
    SqlResult res;
    if (!m_db) { res.error = "No database open"; return res; }

    KdbRows *rows = nullptr;
    size_t affected = 0;
    KdbStatus st = kdb_exec_sql(m_db, stmt.toUtf8().constData(), &rows, &affected);
    if (st != KDB_OK) {
        res.error = QString::fromUtf8(kdb_last_error());
        return res;
    }

    res.ok = true;
    if (rows) {
        res.hasRows = true;
        res.rows = rowsFromKdbRows(rows);
        kdb_rows_free(rows);
    } else {
        res.affected = affected;
    }
    return res;
}
