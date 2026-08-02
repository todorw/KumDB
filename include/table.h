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
                               uint8_t         indexed);

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