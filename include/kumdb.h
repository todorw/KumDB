#ifndef KUMDB_H
#define KUMDB_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "internal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef KdbType KdbFieldType;

/* ARRAY elements have no name, so they're count-based on both the write
 * side (kdb_field_array) and the read side (kdb_row_get_array) -- a
 * NULL-terminator convention doesn't work when a real element can itself
 * be an unnamed NULL-type value. OBJECT fields always have a real name
 * (they're keys), so they stay NULL-name-terminated like every other
 * field list in this API (kdb_field_end()). */
typedef struct KdbField {
    const char  *name;
    KdbFieldType type;
    union {
        int64_t      as_int;
        double       as_float;
        int          as_bool;
        const char  *as_string;
        struct { const void *data; size_t len; } as_blob;
        struct { const struct KdbField *items; size_t count; } as_array;
        const struct KdbField *as_object;
    } v;
} KdbField;

typedef struct {
    uint64_t  id;
    uint64_t  created_at;
    uint64_t  updated_at;
    uint32_t  field_count;
    KdbField *fields;
} KdbRow;

typedef struct {
    KdbRow *rows;
    size_t  count;
} KdbRows;

typedef KdbStatus (*KdbValidator)(const KdbRow *row, void *user_data);

KumDB *kdb_open         (const char *data_dir);
KumDB *kdb_open_readonly(const char *data_dir);
void   kdb_close        (KumDB *db);

KdbStatus kdb_add(KumDB *db, const char *table_name, const KdbField *fields);

KdbStatus kdb_add_validated(KumDB          *db,
                            const char     *table_name,
                            const KdbField *fields,
                            KdbValidator    validator,
                            void           *user_data);

KdbStatus kdb_batch_import(KumDB           *db,
                           const char      *table_name,
                           const KdbField **rows,
                           size_t           count,
                           size_t          *inserted_out);

KdbRows *kdb_find      (KumDB *db, const char *table_name, const char **filters);
KdbRow  *kdb_find_one  (KumDB *db, const char *table_name, const char **filters);
KdbRow  *kdb_find_by_id(KumDB *db, const char *table_name, uint64_t id);
int64_t  kdb_count     (KumDB *db, const char *table_name, const char **filters);

typedef struct {
    const char *order_by;   /* column name ("id"/"created_at"/"updated_at" work too), or NULL */
    int         ascending;  /* 1 = ASC, 0 = DESC; ignored if order_by is NULL */
    size_t      limit;      /* 0 = no limit */
    size_t      offset;     /* rows to skip, applied before limit */
} KdbFindOpts;

KdbRows *kdb_find_ex(KumDB *db, const char *table_name, const char **filters,
                     const KdbFindOpts *opts);

KdbStatus kdb_update(KumDB          *db,
                     const char     *table_name,
                     const char    **where_filters,
                     const KdbField *set_fields,
                     size_t         *updated_out);

KdbStatus kdb_delete(KumDB      *db,
                     const char *table_name,
                     const char **filters,
                     size_t     *deleted_out);

#define KDB_TX_MAX_TABLES 32

/* Groups kdb_tx_add/update/delete calls into one all-or-nothing unit,
 * scoped to a single writer -- this is NOT a multi-writer isolation
 * mechanism, KumDB's concurrency model is still "one writer at a time"
 * same as everywhere else in this API. What it gives you:
 *
 *   - Rollback: kdb_tx_rollback() undoes every change the transaction
 *     made, across however many tables it touched.
 *   - Savepoints: kdb_tx_savepoint()/kdb_tx_rollback_to_savepoint()/
 *     kdb_tx_release_savepoint() nest checkpoints within a transaction --
 *     undo back to one without ending the whole transaction, or discard
 *     one without undoing anything. See KdbSavepoint below.
 *   - Crash safety: if the process dies anywhere between kdb_tx_begin()
 *     and a completed kdb_tx_commit(), the next kdb_open() (read-write)
 *     automatically finishes the job -- either rolling back an
 *     interrupted transaction, or finishing cleanup of one that had
 *     already committed. A table is never left half-migrated (this
 *     covers savepoints too -- any of their own leftover backup files
 *     are just orphaned garbage after a crash either way, and get
 *     cleaned up the same pass).
 *
 * How: the first time a transaction touches a table, it backs up that
 * table's file (or, if the table didn't exist yet, remembers to drop it
 * on rollback instead). Operations apply immediately through the normal
 * kdb_add/kdb_update/kdb_delete underneath -- each already has its own
 * durability guarantees. Commit writes a small marker recording which
 * backups are now safe to discard, deletes them, then deletes the
 * marker; that marker is what recovery uses to tell "committed, just
 * finish cleanup" apart from "never committed, roll back".
 *
 * Costs a full copy of each touched table's file on first touch, so
 * this is meant for coordinating a handful of tables, not wrapping
 * bulk operations on huge ones.
 *
 * kdb_tx_commit()/kdb_tx_rollback() end the transaction and free tx --
 * don't reuse it afterward. If any kdb_tx_* call fails, the transaction
 * is marked failed; kdb_tx_commit() will then refuse (call
 * kdb_tx_rollback() instead). If kdb_tx_commit() itself fails (e.g. disk
 * full while writing the marker), tx is NOT freed -- nothing was lost,
 * retry the commit or roll back.
 */
#define KDB_TX_MAX_SAVEPOINTS 8

/* A named nested checkpoint within a transaction -- tracks, separately
 * from the transaction's own whole-tx backups, every table first touched
 * since THIS savepoint was created (own backup file each, suffixed by
 * this savepoint's stack depth). A table touched under several nested
 * savepoints gets a separate snapshot at each depth, each reflecting the
 * data right before that depth's own first post-creation touch -- so
 * rolling back to any one of them undoes exactly what happened from
 * there on, including anything nested savepoints did in between. */
typedef struct {
    char     name[KDB_MAX_NAME_LEN];
    char     tables[KDB_TX_MAX_TABLES][128];
    uint8_t  is_new_table[KDB_TX_MAX_TABLES];
    uint32_t table_count;
} KdbSavepoint;

typedef struct {
    KumDB   *db;
    char     tables[KDB_TX_MAX_TABLES][128];
    uint8_t  is_new_table[KDB_TX_MAX_TABLES];
    uint32_t table_count;
    int      failed;
    int      active;
    KdbSavepoint savepoints[KDB_TX_MAX_SAVEPOINTS];
    uint32_t     savepoint_count;
} KdbTx;

KdbTx *kdb_tx_begin(KumDB *db);

KdbStatus kdb_tx_add   (KdbTx *tx, const char *table_name, const KdbField *fields);
KdbStatus kdb_tx_update(KdbTx *tx, const char *table_name, const char **where_filters,
                        const KdbField *set_fields, size_t *updated_out);
KdbStatus kdb_tx_delete(KdbTx *tx, const char *table_name, const char **filters, size_t *deleted_out);

KdbStatus kdb_tx_commit  (KdbTx *tx);
KdbStatus kdb_tx_rollback(KdbTx *tx);

/* Establishes a named nested checkpoint within tx, up to
 * KDB_TX_MAX_SAVEPOINTS deep. Rejects a name already used by an active
 * savepoint in this same transaction (no shadowing -- pick a different
 * name instead). */
KdbStatus kdb_tx_savepoint(KdbTx *tx, const char *name);

/* Undoes every change made since the named savepoint was established
 * (including anything done under savepoints nested inside it, which are
 * discarded along with it), without ending the transaction -- tx is still
 * open afterward, at exactly the state it was in when the savepoint was
 * created, and the savepoint itself is still there to roll back to again
 * or release. Clears tx's failed flag, same recovery-escape-hatch
 * reasoning kdb_tx_rollback() (the whole-transaction form) already has. */
KdbStatus kdb_tx_rollback_to_savepoint(KdbTx *tx, const char *name);

/* Discards the named savepoint (and anything nested inside it) without
 * undoing anything -- keeps every change made since it was created,
 * just gives up the ability to roll back to that specific point. */
KdbStatus kdb_tx_release_savepoint(KdbTx *tx, const char *name);

KdbStatus kdb_drop_table  (KumDB *db, const char *table_name);
KdbStatus kdb_compact     (KumDB *db, const char *table_name);
int       kdb_table_exists(KumDB *db, const char *table_name);

typedef struct {
    const char  *name;
    KdbFieldType type;
    int          nullable;
    int          indexed;
    /* Real uniqueness enforcement (not just an indexing hint): kdb_add/
     * kdb_update reject a value that already exists elsewhere in this
     * column (NULLs never conflict with each other, same convention real
     * SQL UNIQUE constraints use). Implies indexed -- a unique column
     * always gets a real index to check against, whether or not indexed
     * is also set explicitly. */
    int          unique;
} KdbColumnDef;

/* Create a table with an explicit schema up front (indexed/unique columns
 * only take effect when declared here). Optional: kdb_add() on a table
 * that doesn't exist yet will still create one and infer its schema from
 * the first row, same as always -- this is for when you want to nail the
 * schema down, including indexes and uniqueness, before any data goes in. */
KdbStatus kdb_create_table(KumDB *db, const char *table_name,
                           const KdbColumnDef *columns, uint32_t column_count);

typedef struct {
    char         name[128];  /* matches the engine's internal column-name limit */
    KdbFieldType type;
    int          nullable;
    int          indexed;
    int          unique;
} KdbColumnInfo;

/* Structured schema introspection (kdb_print_schema() only gives you text).
 * columns_out is caller-owned; copies at most max_columns entries in. */
KdbStatus kdb_get_schema(KumDB *db, const char *table_name,
                         KdbColumnInfo *columns_out, uint32_t max_columns,
                         uint32_t *count_out);

/* Add a column to an existing table's schema. Existing rows just don't have
 * a value for it until you set one -- same as any other missing field.
 * unique on a column added to a table that already has rows only affects
 * rows written from here on -- existing NULL/missing values for the new
 * column never conflict with each other (see KdbColumnDef.unique). */
KdbStatus kdb_add_column(KumDB *db, const char *table_name, const char *col_name,
                         KdbFieldType type, int nullable, int indexed, int unique);

/* Drop a column: removes it from the schema and rewrites every row to
 * strip that field. Like kdb_compact(), this touches the whole table file. */
KdbStatus kdb_drop_column(KumDB *db, const char *table_name, const char *col_name);

/* Indexes an already-existing column (fails with KDB_ERR_EXISTS if it's
 * already indexed) -- unlike the indexed flag on kdb_create_table/
 * kdb_add_column, which only ever takes effect for a column at the
 * moment it's created. Rebuilds the index from every row already in the
 * table before returning. */
KdbStatus kdb_create_index(KumDB *db, const char *table_name, const char *col_name);

/* Removes an existing column's index (KDB_ERR_NOT_FOUND if it isn't
 * indexed). The column and its data are untouched -- only the index. */
KdbStatus kdb_drop_index(KumDB *db, const char *table_name, const char *col_name);

/* A real composite (multi-column) index -- one index hashing all n_cols
 * columns' values together (2..KDB_MAX_COMPOSITE_COLS), not n_cols
 * independent single-column indexes. KDB_ERR_EXISTS if a composite index
 * on exactly this column set (any order) already exists; KDB_ERR_FULL
 * past KDB_MAX_COMPOSITE_INDEXES per table. Rebuilds from every row
 * already in the table before returning. */
KdbStatus kdb_create_composite_index(KumDB *db, const char *table_name, const char **col_names, uint32_t n_cols);

/* Drops a composite index matching exactly this column set (any order) --
 * KDB_ERR_NOT_FOUND if none does. The columns and their data are
 * untouched -- only the index. */
KdbStatus kdb_drop_composite_index(KumDB *db, const char *table_name, const char **col_names, uint32_t n_cols);

/* Renames a column: schema, its index (if any), and every row's field
 * name -- the last part rewrites the whole table file, same cost as
 * kdb_drop_column/kdb_compact. KDB_ERR_EXISTS if new_col already names a
 * column on this table. */
KdbStatus kdb_rename_column(KumDB *db, const char *table_name, const char *old_col, const char *new_col);

/* Toggles a column's declared nullable flag. Setting it to 0 (NOT NULL)
 * only affects rows written from here on -- kdb_add/kdb_update will
 * reject a NULL/missing value for it from that point, but existing rows
 * that already have one aren't retroactively validated or rewritten. */
KdbStatus kdb_alter_column_nullable(KumDB *db, const char *table_name, const char *col_name, int nullable);

/* Toggles a column's declared unique flag the same way -- only affects
 * rows written from here on. Setting it on forces the column indexed too
 * (a unique column always needs a real index to check against). */
KdbStatus kdb_alter_column_unique(KumDB *db, const char *table_name, const char *col_name, int unique);

/* Converts every existing row's value for col_name to new_type (a real
 * data migration, not just a metadata flip) and updates the column's
 * declared type. Uses the same coercions CAST(x AS type) does in SQL
 * (NULL stays NULL; otherwise INT/FLOAT/BOOL/STRING conversions); an
 * existing value that doesn't convert (e.g. a STRING column holding
 * "abc" changed to INT) aborts the whole change with KDB_ERR_BAD_TYPE
 * and leaves the table completely untouched -- never left
 * half-migrated. Rebuilds any index on this column afterward (its old
 * hash buckets no longer match the new-typed values). A no-op if
 * new_type is already the column's current type. */
KdbStatus kdb_alter_column_type(KumDB *db, const char *table_name, const char *col_name, KdbFieldType new_type);

/* Adds a foreign key: col_name on table_name must always be NULL or equal
 * to some existing ref_table.ref_col value. Both ref_table and ref_col
 * must already exist (checked right away, not deferred). Enforced from
 * here on: kdb_add/kdb_update reject a non-NULL col_name value with no
 * matching row in ref_table.ref_col; deleting or updating away a row in
 * ref_table that col_name still points to is rejected too (RESTRICT --
 * no ON DELETE/UPDATE CASCADE or SET NULL). One FK per column
 * (KDB_ERR_EXISTS if col_name already has one); no composite
 * (multi-column) foreign keys. kdb_batch_import bypasses this, same as
 * it bypasses NOT NULL/UNIQUE. */
KdbStatus kdb_add_foreign_key(KumDB *db, const char *table_name, const char *col_name,
                              const char *ref_table, const char *ref_col);

/* Removes col_name's foreign key. KDB_ERR_NOT_FOUND if it doesn't have one. */
KdbStatus kdb_drop_foreign_key(KumDB *db, const char *table_name, const char *col_name);

/* Adds a CHECK (col_name op literal) constraint -- op must be one of
 * KDB_OP_EQ/NEQ/GT/GTE/LT/LTE (no BETWEEN/IN/LIKE/etc); literal.type must
 * be INT/FLOAT/BOOL/STRING (literal.name is ignored). Enforced from here
 * on the same way NOT NULL/UNIQUE are -- a NULL value never violates a
 * CHECK, existing rows aren't retroactively checked. KDB_ERR_FULL past
 * KDB_MAX_CHECK_CONSTRAINTS constraints on one table. */
KdbStatus kdb_add_check_constraint(KumDB *db, const char *table_name, const char *col_name,
                                   KdbOperator op, const KdbField *literal);

/* Renames the table itself (the file on disk, and the header's own
 * stored name). KDB_ERR_EXISTS if new_name already names a table. */
KdbStatus kdb_rename_table(KumDB *db, const char *old_name, const char *new_name);

/* names_out entries point into thread-local storage owned by KumDB and are
 * valid until the next call to kdb_list_tables() on this thread. Copy them
 * if you need to keep them around longer. */
KdbStatus kdb_list_tables(KumDB      *db,
                          const char **names_out,
                          size_t      max_tables,
                          size_t     *count_out);

void kdb_rows_free(KdbRows *rows);
void kdb_row_free (KdbRow  *row);

const KdbField *kdb_row_get(const KdbRow *row, const char *col_name);

KdbStatus kdb_row_get_int   (const KdbRow *row, const char *col, int64_t    *out);
KdbStatus kdb_row_get_float (const KdbRow *row, const char *col, double     *out);
KdbStatus kdb_row_get_bool  (const KdbRow *row, const char *col, int        *out);
KdbStatus kdb_row_get_string(const KdbRow *row, const char *col, const char **out);
KdbStatus kdb_row_get_blob  (const KdbRow *row, const char *col, const void **data_out, size_t *len_out);
/* *items_out points into row-owned memory, valid until kdb_row_free()/kdb_rows_free(). */
KdbStatus kdb_row_get_array (const KdbRow *row, const char *col, const KdbField **items_out, size_t *count_out);
/* *fields_out is NULL-name-terminated, same convention as everywhere else. */
KdbStatus kdb_row_get_object(const KdbRow *row, const char *col, const KdbField **fields_out);

const char *kdb_last_error (void);
KdbStatus   kdb_last_status(void);
void        kdb_clear_error(void);

void        kdb_row_print  (const KdbRow  *row,  FILE *fp);
void        kdb_rows_print (const KdbRows *rows, FILE *fp);
KdbStatus   kdb_print_schema(KumDB *db, const char *table_name, FILE *fp);
const char *kdb_version    (void);

/* Plain member assignment on purpose, not designated initializers: nested
 * designators like ".v.as_int = v" are a GNU C extension, not valid in any
 * C++ standard -- and this header is explicitly meant to work from C++
 * (see the extern "C" guard above). */
static inline KdbField kdb_field_int   (const char *n, int64_t     v) { KdbField f = {n, KDB_TYPE_INT,    {0}}; f.v.as_int    = v; return f; }
static inline KdbField kdb_field_float (const char *n, double      v) { KdbField f = {n, KDB_TYPE_FLOAT,  {0}}; f.v.as_float  = v; return f; }
static inline KdbField kdb_field_bool  (const char *n, int         v) { KdbField f = {n, KDB_TYPE_BOOL,   {0}}; f.v.as_bool   = v; return f; }
static inline KdbField kdb_field_string(const char *n, const char *v) { KdbField f = {n, KDB_TYPE_STRING, {0}}; f.v.as_string = v; return f; }
static inline KdbField kdb_field_null  (const char *n)                { KdbField f = {n, KDB_TYPE_NULL,   {0}           }; return f; }
static inline KdbField kdb_field_end   (void)                         { KdbField f = {NULL, KDB_TYPE_NULL, {0}           }; return f; }
static inline KdbField kdb_field_blob  (const char *n, const void *data, size_t len) {
    KdbField f = {n, KDB_TYPE_BLOB, {0}};
    f.v.as_blob.data = data;
    f.v.as_blob.len  = len;
    return f;
}

/* items is a plain array of KdbField (count-based, see the KdbField comment
 * above) -- each element's own .name is ignored. Example:
 *   KdbField tags[] = { kdb_field_string(NULL, "vip"), kdb_field_string(NULL, "new") };
 *   kdb_field_array("tags", tags, 2)
 */
static inline KdbField kdb_field_array(const char *n, const KdbField *items, size_t count) {
    KdbField f = {n, KDB_TYPE_ARRAY, {0}};
    f.v.as_array.items = items;
    f.v.as_array.count = count;
    return f;
}

/* fields is a NULL-name-terminated list, same convention as kdb_add()'s
 * top-level fields array. Example:
 *   KdbField address[] = { kdb_field_string("city", "NYC"), kdb_field_end() };
 *   kdb_field_object("address", address)
 */
static inline KdbField kdb_field_object(const char *n, const KdbField *fields) {
    KdbField f = {n, KDB_TYPE_OBJECT, {0}};
    f.v.as_object = fields;
    return f;
}

#ifdef __cplusplus
}
#endif

#endif 