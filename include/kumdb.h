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

typedef struct {
    const char  *name;
    KdbFieldType type;
    union {
        int64_t      as_int;
        double       as_float;
        int          as_bool;
        const char  *as_string;
        struct { const void *data; size_t len; } as_blob;
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

KdbStatus kdb_update(KumDB          *db,
                     const char     *table_name,
                     const char    **where_filters,
                     const KdbField *set_fields,
                     size_t         *updated_out);

KdbStatus kdb_delete(KumDB      *db,
                     const char *table_name,
                     const char **filters,
                     size_t     *deleted_out);

KdbStatus kdb_drop_table  (KumDB *db, const char *table_name);
KdbStatus kdb_compact     (KumDB *db, const char *table_name);
int       kdb_table_exists(KumDB *db, const char *table_name);

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

const char *kdb_last_error (void);
KdbStatus   kdb_last_status(void);
void        kdb_clear_error(void);

void        kdb_row_print  (const KdbRow  *row,  FILE *fp);
void        kdb_rows_print (const KdbRows *rows, FILE *fp);
KdbStatus   kdb_print_schema(KumDB *db, const char *table_name, FILE *fp);
const char *kdb_version    (void);

static inline KdbField kdb_field_int   (const char *n, int64_t     v) { KdbField f = {n, KDB_TYPE_INT,    .v.as_int    = v}; return f; }
static inline KdbField kdb_field_float (const char *n, double      v) { KdbField f = {n, KDB_TYPE_FLOAT,  .v.as_float  = v}; return f; }
static inline KdbField kdb_field_bool  (const char *n, int         v) { KdbField f = {n, KDB_TYPE_BOOL,   .v.as_bool   = v}; return f; }
static inline KdbField kdb_field_string(const char *n, const char *v) { KdbField f = {n, KDB_TYPE_STRING, .v.as_string = v}; return f; }
static inline KdbField kdb_field_null  (const char *n)                { KdbField f = {n, KDB_TYPE_NULL,   {0}           }; return f; }
static inline KdbField kdb_field_end   (void)                         { KdbField f = {NULL, KDB_TYPE_NULL, {0}           }; return f; }
static inline KdbField kdb_field_blob  (const char *n, const void *data, size_t len) {
    KdbField f = {n, KDB_TYPE_BLOB, {0}};
    f.v.as_blob.data = data;
    f.v.as_blob.len  = len;
    return f;
}

#ifdef __cplusplus
}
#endif

#endif 