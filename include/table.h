#ifndef KUMDB_TABLE_H
#define KUMDB_TABLE_H

#include "internal.h"
#include "storage.h"
#include "index.h"

KdbStatus kdb_table_create(const char      *data_dir,
                           const char      *table_name,
                           const KdbColumn *columns,
                           uint32_t         column_count);

KdbStatus kdb_table_open(KdbTable   *tbl,
                         const char *data_dir,
                         const char *table_name);

void kdb_table_close(KdbTable *tbl);

KdbStatus kdb_table_drop(KdbTable   *tbl,
                         const char *data_dir,
                         const char *table_name);

int kdb_storage_table_exists(const char *data_dir, const char *table_name);

KdbStatus kdb_table_add_column(KdbTable       *tbl,
                               const char     *col_name,
                               KdbType         type,
                               uint8_t         nullable,
                               uint8_t         indexed,
                               uint8_t         unique);

KdbStatus kdb_table_drop_column(KdbTable   *tbl,
                                const char *col_name);

/* Indexes an already-existing column (KDB_ERR_EXISTS if it's already
 * indexed) -- unlike the indexed flag on CREATE TABLE/ALTER TABLE ADD
 * COLUMN, which only ever takes effect at column-creation time. Rebuilds
 * the index from every row already in the table before returning. */
KdbStatus kdb_table_create_index(KdbTable *tbl, const char *col_name);

/* Removes an existing column's index (KDB_ERR_NOT_FOUND if it isn't
 * indexed). The column and its data are untouched -- only the index. */
KdbStatus kdb_table_drop_index(KdbTable *tbl, const char *col_name);

/* A real composite (multi-column) index -- one index hashing all n_cols
 * columns' values together (n_cols must be 2..KDB_MAX_COMPOSITE_COLS),
 * not n_cols independent single-column indexes. KDB_ERR_EXISTS if a
 * composite index on exactly this column set (any order) already exists;
 * KDB_ERR_FULL if the table already has KDB_MAX_COMPOSITE_INDEXES of
 * them. Rebuilds from every row already in the table before returning. */
KdbStatus kdb_table_create_composite_index(KdbTable *tbl, const char **col_names, uint32_t n_cols);

/* Drops a composite index matching exactly this column set (any order) --
 * KDB_ERR_NOT_FOUND if none does. The columns and their data are
 * untouched -- only the index. */
KdbStatus kdb_table_drop_composite_index(KdbTable *tbl, const char **col_names, uint32_t n_cols);

/* Renames a column: schema, its index (if any), and every row's field
 * name for it -- the last part is a full table rewrite, same cost as
 * kdb_table_drop_column/kdb_compact. KDB_ERR_EXISTS if new_name is
 * already a column on this table. */
KdbStatus kdb_table_rename_column(KdbTable *tbl, const char *old_name, const char *new_name);

/* Toggles a column's declared nullable flag. Setting it to 0 (NOT NULL)
 * only affects rows written from here on -- existing rows that already
 * have a NULL/missing value for this column aren't retroactively
 * validated or rewritten. */
KdbStatus kdb_table_set_nullable(KdbTable *tbl, const char *col_name, int nullable);

/* Toggles a column's declared unique flag the same way -- only affects
 * rows written from here on; existing duplicate values already in the
 * table aren't retroactively checked or rejected. Setting it on forces
 * the column indexed too (see KdbColumnDef.unique). */
KdbStatus kdb_table_set_unique(KdbTable *tbl, const char *col_name, int unique);

/* Converts every existing row's value for col_name to new_type (via
 * kdb_value_cast) and updates the column's declared type -- a real data
 * migration, same cost as kdb_table_drop_column/kdb_compact. If any
 * existing value can't convert, the whole change is aborted and the
 * table is left completely untouched (KDB_ERR_BAD_TYPE). Rebuilds any
 * index touching this column afterward. A no-op (KDB_OK, no rewrite) if
 * new_type is already the column's type. */
KdbStatus kdb_table_alter_column_type(KdbTable *tbl, const char *col_name, KdbType new_type);

/* Sets col_name's FK metadata only -- doesn't verify ref_table/ref_col
 * exist (this handle can't look up another table; kdb_add_foreign_key in
 * kumdb.c does that first). KDB_ERR_EXISTS if col_name already has a FK
 * (one per column, no composite foreign keys). on_delete/on_update are
 * KDB_FK_RESTRICT/CASCADE/SET_NULL -- KDB_ERR_BAD_ARG if either is
 * SET_NULL but col_name isn't nullable. */
KdbStatus kdb_table_add_foreign_key(KdbTable *tbl, const char *col_name,
                                    const char *ref_table, const char *ref_col,
                                    KdbFkAction on_delete, KdbFkAction on_update);

/* Clears col_name's FK metadata. KDB_ERR_NOT_FOUND if it doesn't have one. */
KdbStatus kdb_table_drop_foreign_key(KdbTable *tbl, const char *col_name);

/* A composite (multi-column) foreign key -- one constraint spanning
 * n_cols columns (2..KDB_MAX_COMPOSITE_COLS) matched positionally against
 * n_ref_cols columns on ref_table (n_cols must equal n_ref_cols). Doesn't
 * verify ref_table/ref_cols exist (this handle can't look up another
 * table; kdb_add_composite_foreign_key in kumdb.c does that first).
 * KDB_ERR_EXISTS if a composite FK on exactly this column set (any order)
 * already exists; KDB_ERR_FULL past KDB_MAX_COMPOSITE_FKS. on_delete/
 * on_update are KDB_FK_RESTRICT/CASCADE/SET_NULL -- KDB_ERR_BAD_ARG if
 * either is SET_NULL but not every column in col_names is nullable.
 * Never references the pseudo-columns id/created_at/updated_at on either
 * side, unlike single-column FK. */
KdbStatus kdb_table_add_composite_foreign_key(KdbTable *tbl, const char **col_names, uint32_t n_cols,
                                              const char *ref_table, const char **ref_cols, uint32_t n_ref_cols,
                                              KdbFkAction on_delete, KdbFkAction on_update);

/* Drops a composite foreign key matching exactly this column set (any
 * order) -- KDB_ERR_NOT_FOUND if none does. */
KdbStatus kdb_table_drop_composite_foreign_key(KdbTable *tbl, const char **col_names, uint32_t n_cols);

/* Adds a CHECK (col_name op literal) constraint -- op must be one of
 * KDB_OP_EQ/NEQ/GT/GTE/LT/LTE, literal one of INT/FLOAT/BOOL/STRING.
 * Enforced from here on (kdb_table_insert/kdb_table_update reject a
 * violation), existing rows aren't retroactively checked.
 * KDB_ERR_FULL past KDB_MAX_CHECK_CONSTRAINTS. */
KdbStatus kdb_table_add_check(KdbTable *tbl, const char *col_name, KdbOperator op, const KdbValue *literal);

/* Sets col_name's DEFAULT value -- an INT/FLOAT/BOOL/STRING literal, same
 * restricted set as CHECK. Applied by kdb_table_insert/kdb_table_insert_
 * batch to fill in any row whose record has no field at all for col_name
 * (an explicit NULL is left alone -- DEFAULT only fires when the column
 * is omitted outright). Overwrites any DEFAULT col_name already had. */
KdbStatus kdb_table_set_default(KdbTable *tbl, const char *col_name, const KdbValue *default_val);

/* Clears col_name's DEFAULT. KDB_ERR_NOT_FOUND if it doesn't have one. */
KdbStatus kdb_table_drop_default(KdbTable *tbl, const char *col_name);

const KdbColumn *kdb_table_get_column(const KdbTable *tbl,
                                      const char     *col_name);

int kdb_table_has_column(const KdbTable *tbl, const char *col_name);

KdbStatus kdb_table_infer_schema(KdbTable        *tbl,
                                 const KdbRecord *r);

KdbStatus kdb_table_insert(KdbTable  *tbl,
                           KdbRecord *r);

KdbStatus kdb_table_insert_batch(KdbTable  *tbl,
                                 KdbRecord *records,
                                 size_t     count,
                                 size_t    *inserted_out);

KdbStatus kdb_table_update(KdbTable        *tbl,
                           const KdbQuery  *query,
                           const KdbRecord *patch,
                           size_t          *updated_out);

KdbStatus kdb_table_delete(KdbTable       *tbl,
                           const KdbQuery *query,
                           size_t         *deleted_out);

KdbStatus kdb_table_compact(KdbTable *tbl);

void kdb_table_print_schema(const KdbTable *tbl, FILE *fp);

void kdb_table_print_stats(KdbTable *tbl, FILE *fp);

uint64_t kdb_table_count(KdbTable *tbl);

#endif 