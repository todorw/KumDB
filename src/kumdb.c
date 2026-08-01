#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "../include/kumdb.h"
#include "../include/internal.h"
#include "../include/error.h"
#include "../include/types.h"
#include "../include/record.h"
#include "../include/table.h"
#include "../include/query.h"
#include "../include/storage.h"

static void kdb_row_free_internal(KdbRow *row);


const char *kdb_version(void) {
    return "1.0.0";
}


static KdbTable *kdb__get_table(KumDB *db, const char *table_name) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb__get_table");
        return NULL;
    }

    
    for (uint32_t i = 0; i < db->table_count; i++) {
        if (db->tables[i] && strcmp(db->tables[i]->name, table_name) == 0)
            return db->tables[i];
    }

    
    if (!kdb_storage_exists(db->data_dir, table_name)) {
        kdb_err_table_not_found(table_name);
        return NULL;
    }

    if (db->table_count >= KDB_MAX_TABLES) {
        kdb_set_error(KDB_ERR_FULL,
            "All %d table slots are in use. Close some tables before opening more.",
            KDB_MAX_TABLES);
        return NULL;
    }

    KdbTable *tbl = KDB_ALLOC(KdbTable);
    if (!tbl) { kdb_err_oom("KdbTable handle"); return NULL; }

    KdbStatus st = kdb_table_open(tbl, db->data_dir, table_name);
    if (st != KDB_OK) { free(tbl); return NULL; }

    tbl->read_only = db->read_only;
    db->tables[db->table_count++] = tbl;
    return tbl;
}


static KdbTable *kdb__get_or_create_table(KumDB *db, const char *table_name) {
    if (!db || !table_name) return NULL;

    
    for (uint32_t i = 0; i < db->table_count; i++) {
        if (db->tables[i] && strcmp(db->tables[i]->name, table_name) == 0)
            return db->tables[i];
    }

    if (!kdb_storage_exists(db->data_dir, table_name)) {
        
        KdbStatus st = kdb_storage_create(db->data_dir, table_name, NULL, 0);
        if (st != KDB_OK) return NULL;
    }

    return kdb__get_table(db, table_name);
}


static KdbRecord *kdb__fields_to_record(const KdbField *fields) {
    if (!fields) { kdb_err_null_arg("fields", "kdb__fields_to_record"); return NULL; }

    
    uint32_t count = 0;
    while (fields[count].name != NULL) count++;

    KdbRecord *r = kdb_record_new(count);
    if (!r) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        const KdbField *f = &fields[i];
        KdbValue val;
        memset(&val, 0, sizeof(val));

        switch (f->type) {
            case KDB_TYPE_INT:
                kdb_value_from_int(f->v.as_int, &val);
                break;
            case KDB_TYPE_FLOAT:
                kdb_value_from_float(f->v.as_float, &val);
                break;
            case KDB_TYPE_BOOL:
                kdb_value_from_bool((uint8_t)f->v.as_bool, &val);
                break;
            case KDB_TYPE_STRING:
                if (kdb_value_from_string(f->v.as_string, KDB_TYPE_STRING, &val) != KDB_OK) {
                    kdb_record_free(r);
                    return NULL;
                }
                break;
            case KDB_TYPE_NULL:
                kdb_value_from_null(&val);
                break;
            case KDB_TYPE_BLOB:
                if (kdb_value_from_blob(f->v.as_blob.data, f->v.as_blob.len, &val) != KDB_OK) {
                    kdb_record_free(r);
                    return NULL;
                }
                break;
            default:
                kdb_value_from_null(&val);
                break;
        }

        KdbStatus st = kdb_record_set_field(r, f->name, &val);
        kdb_value_free(&val);
        if (st != KDB_OK) { kdb_record_free(r); return NULL; }
    }
    return r;
}


static KdbStatus kdb__build_query(const char **filters, KdbQuery *q) {
    kdb_query_init(q);
    if (!filters) return KDB_OK; 

    for (int i = 0; filters[i] != NULL; i++) {
        const char *filter = filters[i];

        /* "OR:" prefix starts a new AND-group, OR'd against what came
         * before it -- e.g. {"age__lt=18", "OR:age__gt=65", NULL} means
         * age < 18 OR age > 65. The rest of the string after "OR:" is a
         * normal filter and falls through to the parsing below. */
        if (strncmp(filter, "OR:", 3) == 0) {
            KdbStatus st = kdb_query_start_or_group(q);
            if (st != KDB_OK) { kdb_query_free(q); return st; }
            filter += 3;
        }

        const char *eq = strchr(filter, '=');
        if (!eq) {
            
            KdbStatus st = kdb_query_add_filter(q, filter, NULL, NULL);
            if (st != KDB_OK) { kdb_query_free(q); return st; }
            continue;
        }

        
        char key[KDB_MAX_NAME_LEN * 2];
        size_t key_len = (size_t)(eq - filter);
        if (key_len >= sizeof(key)) {
            kdb_err_bad_filter(filter, "filter key is too long");
            kdb_query_free(q);
            return KDB_ERR_BAD_FILTER;
        }
        memcpy(key, filter, key_len);
        key[key_len] = '\0';

        const char *value = eq + 1;

        

        KdbOperator op;
        char col_name[KDB_MAX_NAME_LEN];
        if (kdb_parse_filter_key(key, col_name, &op) != KDB_OK) {
            kdb_query_free(q);
            return KDB_ERR_BAD_FILTER;
        }

        const char *value2 = NULL;
        char value_buf[KDB_MAX_STRING_LEN];
        if (op == KDB_OP_BETWEEN) {
            
            const char *comma = strchr(value, ',');
            if (!comma) {
                kdb_err_bad_filter(filter, "BETWEEN requires two values separated by ','  e.g. age__between=10,30");
                kdb_query_free(q);
                return KDB_ERR_BAD_FILTER;
            }
            size_t lo_len = (size_t)(comma - value);
            if (lo_len >= sizeof(value_buf)) lo_len = sizeof(value_buf) - 1;
            memcpy(value_buf, value, lo_len);
            value_buf[lo_len] = '\0';
            value  = value_buf;
            value2 = comma + 1;
        }

        KdbStatus st = kdb_query_add_filter(q, key, value, value2);
        if (st != KDB_OK) { kdb_query_free(q); return st; }
    }
    return KDB_OK;
}


static KdbRow *kdb__record_to_row(const KdbRecord *r) {
    if (!r) return NULL;

    KdbRow *row = (KdbRow *)calloc(1, sizeof(KdbRow));
    if (!row) { kdb_err_oom("KdbRow"); return NULL; }

    row->id          = r->id;
    row->created_at  = r->created_at;
    row->updated_at  = r->updated_at;
    row->field_count = r->field_count;

    if (r->field_count > 0) {
        row->fields = (KdbField *)calloc(r->field_count, sizeof(KdbField));
        if (!row->fields) { free(row); kdb_err_oom("KdbRow fields"); return NULL; }

        for (uint32_t i = 0; i < r->field_count; i++) {
            const KdbRecordField *src = &r->fields[i];
            KdbField       *dst = &row->fields[i];
            
            char *name_copy = (char *)malloc(KDB_MAX_NAME_LEN);
            if (!name_copy) {
                
                for (uint32_t j = 0; j < i; j++) free((void *)row->fields[j].name);
                free(row->fields);
                free(row);
                kdb_err_oom("KdbRow field name");
                return NULL;
            }
            KDB_STRLCPY(name_copy, src->col_name, KDB_MAX_NAME_LEN);
            dst->name = name_copy;

            
            switch (src->value.type) {
                case KDB_TYPE_INT:
                    dst->type       = KDB_TYPE_INT;
                    dst->v.as_int   = src->value.v.as_int;
                    break;
                case KDB_TYPE_FLOAT:
                    dst->type       = KDB_TYPE_FLOAT;
                    dst->v.as_float = src->value.v.as_float;
                    break;
                case KDB_TYPE_BOOL:
                    dst->type      = KDB_TYPE_BOOL;
                    dst->v.as_bool = src->value.v.as_bool;
                    break;
                case KDB_TYPE_STRING: {
                    dst->type = KDB_TYPE_STRING;
                    char *str_copy = NULL;
                    if (src->value.v.as_string.data) {
                        str_copy = (char *)malloc(src->value.v.as_string.len + 1);
                        if (str_copy) {
                            memcpy(str_copy, src->value.v.as_string.data,
                                   src->value.v.as_string.len + 1);
                        }
                    }
                    dst->v.as_string = str_copy;
                    break;
                }
                case KDB_TYPE_BLOB: {
                    dst->type = KDB_TYPE_BLOB;
                    void *blob_copy = NULL;
                    if (src->value.v.as_blob.len > 0 && src->value.v.as_blob.data) {
                        blob_copy = malloc(src->value.v.as_blob.len);
                        if (blob_copy) {
                            memcpy(blob_copy, src->value.v.as_blob.data,
                                   src->value.v.as_blob.len);
                        }
                    }
                    dst->v.as_blob.data = blob_copy;
                    dst->v.as_blob.len  = src->value.v.as_blob.len;
                    break;
                }
                case KDB_TYPE_NULL:
                default:
                    dst->type = KDB_TYPE_NULL;
                    break;
            }
        }
    }
    return row;
}


static KumDB *kdb__open_internal(const char *data_dir, uint8_t read_only) {
    if (!data_dir) {
        kdb_err_null_arg("data_dir", "kdb_open");
        return NULL;
    }

    
    struct stat st;
    if (stat(data_dir, &st) != 0) {
        if (!read_only) {
            mkdir(data_dir, 0755);
        } else {
            kdb_set_error(KDB_ERR_NOT_FOUND,
                "Data directory '%s' doesn't exist and can't be created in read-only mode.",
                data_dir);
            return NULL;
        }
    }

    KumDB *db = KDB_ALLOC(KumDB);
    if (!db) { kdb_err_oom("KumDB handle"); return NULL; }

    KDB_STRLCPY(db->data_dir, data_dir, sizeof(db->data_dir));
    db->read_only   = read_only;
    db->table_count = 0;
    return db;
}

KumDB *kdb_open(const char *data_dir) {
    return kdb__open_internal(data_dir, 0);
}

KumDB *kdb_open_readonly(const char *data_dir) {
    return kdb__open_internal(data_dir, 1);
}

void kdb_close(KumDB *db) {
    if (!db) return;
    for (uint32_t i = 0; i < db->table_count; i++) {
        if (db->tables[i]) {
            kdb_table_close(db->tables[i]);
            free(db->tables[i]);
            db->tables[i] = NULL;
        }
    }
    free(db);
}


KdbStatus kdb_add(KumDB *db, const char *table_name, const KdbField *fields) {
    return kdb_add_validated(db, table_name, fields, NULL, NULL);
}

KdbStatus kdb_add_validated(KumDB            *db,
                            const char       *table_name,
                            const KdbField   *fields,
                            KdbValidator      validator,
                            void             *user_data) {
    if (!db || !table_name || !fields) {
        kdb_err_null_arg("db/table_name/fields", "kdb_add");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }

    KdbTable *tbl = kdb__get_or_create_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbRecord *r = kdb__fields_to_record(fields);
    if (!r) return kdb_last_status();

    
    if (validator) {
        KdbRow *row = kdb__record_to_row(r);
        if (!row) { kdb_record_free(r); return KDB_ERR_OOM; }
        KdbStatus vst = validator(row, user_data);
        kdb_row_free(row);
        if (vst != KDB_OK) {
            kdb_record_free(r);
            if (kdb_last_status() == KDB_OK)
                kdb_err_validation(table_name, "validator returned non-OK status");
            return KDB_ERR_VALIDATION;
        }
    }

    KdbStatus st = kdb_table_insert(tbl, r);
    kdb_record_free(r);
    return st;
}

KdbStatus kdb_batch_import(KumDB             *db,
                           const char        *table_name,
                           const KdbField   **rows,
                           size_t             count,
                           size_t            *inserted_out) {
    if (!db || !table_name || !rows) {
        kdb_err_null_arg("db/table_name/rows", "kdb_batch_import");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    if (count > KDB_MAX_BATCH_SIZE) {
        kdb_err_batch_too_large(count, KDB_MAX_BATCH_SIZE);
        return KDB_ERR_FULL;
    }
    if (inserted_out) *inserted_out = 0;

    KdbTable *tbl = kdb__get_or_create_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbRecord *records = (KdbRecord *)calloc(count, sizeof(KdbRecord));
    if (!records) { kdb_err_oom("batch record array"); return KDB_ERR_OOM; }

    size_t built = 0;
    for (size_t i = 0; i < count; i++) {
        KdbRecord *r = kdb__fields_to_record(rows[i]);
        if (!r) {
            
            for (size_t j = 0; j < built; j++) {
                if (records[j].fields) {
                    for (uint32_t k = 0; k < records[j].field_count; k++)
                        kdb_value_free(&records[j].fields[k].value);
                    free(records[j].fields);
                }
            }
            free(records);
            return kdb_last_status();
        }
        memcpy(&records[i], r, sizeof(KdbRecord));
        free(r); 
        built++;
    }

    KdbStatus st = kdb_table_insert_batch(tbl, records, count, inserted_out);

    
    for (size_t i = 0; i < built; i++) {
        if (records[i].fields) {
            for (uint32_t j = 0; j < records[i].field_count; j++)
                kdb_value_free(&records[i].fields[j].value);
            free(records[i].fields);
        }
    }
    free(records);
    return st;
}

static KdbRows *kdb__result_to_rows(KdbResult *res) {
    KdbRows *rows = (KdbRows *)calloc(1, sizeof(KdbRows));
    if (!rows) { kdb_result_free(res); kdb_err_oom("KdbRows"); return NULL; }

    rows->count = res->count;
    if (res->count > 0) {
        rows->rows = (KdbRow *)calloc(res->count, sizeof(KdbRow));
        if (!rows->rows) {
            kdb_result_free(res);
            free(rows);
            kdb_err_oom("KdbRow array");
            return NULL;
        }
        for (size_t i = 0; i < res->count; i++) {
            KdbRow *row = kdb__record_to_row(&res->rows[i]);
            if (row) {
                memcpy(&rows->rows[i], row, sizeof(KdbRow));
                free(row);
            }
        }
    }

    kdb_result_free(res);
    return rows;
}

KdbRows *kdb_find(KumDB *db, const char *table_name, const char **filters) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_find");
        return NULL;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return NULL;

    KdbQuery q;
    if (kdb__build_query(filters, &q) != KDB_OK) return NULL;

    KdbResult res;
    KdbStatus st = kdb_query_execute(tbl, &q, &res);
    kdb_query_free(&q);
    if (st != KDB_OK) return NULL;

    return kdb__result_to_rows(&res);
}

KdbRows *kdb_find_ex(KumDB *db, const char *table_name, const char **filters,
                     const KdbFindOpts *opts) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_find_ex");
        return NULL;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return NULL;

    KdbQuery q;
    if (kdb__build_query(filters, &q) != KDB_OK) return NULL;

    KdbResult res;
    KdbStatus st = kdb_query_execute(tbl, &q, &res);
    kdb_query_free(&q);
    if (st != KDB_OK) return NULL;

    if (opts) {
        if (opts->order_by) kdb_result_sort(&res, opts->order_by, opts->ascending);
        if (opts->offset > 0) kdb_result_offset(&res, opts->offset);
        if (opts->limit > 0) kdb_result_limit(&res, opts->limit);
    }

    return kdb__result_to_rows(&res);
}

KdbRow *kdb_find_one(KumDB *db, const char *table_name, const char **filters) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_find_one");
        return NULL;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return NULL;

    KdbQuery q;
    if (kdb__build_query(filters, &q) != KDB_OK) return NULL;

    KdbResult res;
    KdbStatus st = kdb_query_execute_one(tbl, &q, &res);
    kdb_query_free(&q);
    if (st != KDB_OK || res.count == 0) { kdb_result_free(&res); return NULL; }

    KdbRow *row = kdb__record_to_row(&res.rows[0]);
    kdb_result_free(&res);
    return row;
}

KdbRow *kdb_find_by_id(KumDB *db, const char *table_name, uint64_t id) {
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)id);
    const char *filters[] = { NULL, NULL };
    char filter_str[64];
    snprintf(filter_str, sizeof(filter_str), "id=%llu", (unsigned long long)id);
    filters[0] = filter_str;
    return kdb_find_one(db, table_name, filters);
}

int64_t kdb_count(KumDB *db, const char *table_name, const char **filters) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_count");
        return -1;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return -1;

    KdbQuery q;
    if (kdb__build_query(filters, &q) != KDB_OK) return -1;

    size_t    count = 0;
    KdbStatus st    = kdb_query_count(tbl, &q, &count);
    kdb_query_free(&q);
    return (st == KDB_OK) ? (int64_t)count : -1;
}

KdbStatus kdb_update(KumDB            *db,
                     const char       *table_name,
                     const char      **where_filters,
                     const KdbField   *set_fields,
                     size_t           *updated_out) {
    if (!db || !table_name || !set_fields) {
        kdb_err_null_arg("db/table_name/set_fields", "kdb_update");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbQuery q;
    KdbStatus st = kdb__build_query(where_filters, &q);
    if (st != KDB_OK) return st;

    KdbRecord *patch = kdb__fields_to_record(set_fields);
    if (!patch) { kdb_query_free(&q); return kdb_last_status(); }

    if (updated_out) *updated_out = 0;
    st = kdb_table_update(tbl, &q, patch, updated_out);

    kdb_record_free(patch);
    kdb_query_free(&q);
    return st;
}

KdbStatus kdb_delete(KumDB       *db,
                     const char  *table_name,
                     const char **filters,
                     size_t      *deleted_out) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_delete");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbQuery q;
    KdbStatus st = kdb__build_query(filters, &q);
    if (st != KDB_OK) return st;

    if (deleted_out) *deleted_out = 0;
    st = kdb_table_delete(tbl, &q, deleted_out);
    kdb_query_free(&q);
    return st;
}


KdbStatus kdb_create_table(KumDB *db, const char *table_name,
                           const KdbColumnDef *columns, uint32_t column_count) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_create_table");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    if (column_count > KDB_MAX_COLUMNS) {
        kdb_err_bad_arg("column_count", "exceeds KDB_MAX_COLUMNS");
        return KDB_ERR_FULL;
    }
    if (column_count > 0 && !columns) {
        kdb_err_null_arg("columns", "kdb_create_table");
        return KDB_ERR_BAD_ARG;
    }

    KdbColumn cols[KDB_MAX_COLUMNS];
    memset(cols, 0, sizeof(cols));
    for (uint32_t i = 0; i < column_count; i++) {
        if (!columns[i].name) {
            kdb_err_bad_arg("columns[i].name", "must not be NULL");
            return KDB_ERR_BAD_ARG;
        }
        KDB_STRLCPY(cols[i].name, columns[i].name, KDB_MAX_NAME_LEN);
        cols[i].type     = (KdbType)columns[i].type;
        cols[i].nullable = columns[i].nullable ? 1 : 0;
        cols[i].indexed  = columns[i].indexed  ? 1 : 0;
    }

    return kdb_table_create(db->data_dir, table_name, cols, column_count);
}

KdbStatus kdb_get_schema(KumDB *db, const char *table_name,
                         KdbColumnInfo *columns_out, uint32_t max_columns,
                         uint32_t *count_out) {
    if (!db || !table_name || !columns_out || !count_out) {
        kdb_err_null_arg("db/table_name/columns_out/count_out", "kdb_get_schema");
        return KDB_ERR_BAD_ARG;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    *count_out = 0;
    for (uint32_t i = 0; i < tbl->header.column_count && *count_out < max_columns; i++) {
        const KdbColumn *c = &tbl->header.columns[i];
        KdbColumnInfo   *out = &columns_out[*count_out];
        KDB_STRLCPY(out->name, c->name, sizeof(out->name));
        out->type     = (KdbFieldType)c->type;
        out->nullable = c->nullable ? 1 : 0;
        out->indexed  = c->indexed  ? 1 : 0;
        (*count_out)++;
    }
    return KDB_OK;
}

KdbStatus kdb_drop_table(KumDB *db, const char *table_name) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_drop_table");
        return KDB_ERR_BAD_ARG;
    }


    for (uint32_t i = 0; i < db->table_count; i++) {
        if (db->tables[i] && strcmp(db->tables[i]->name, table_name) == 0) {
            kdb_table_close(db->tables[i]);
            free(db->tables[i]);
            db->tables[i] = db->tables[--db->table_count];
            db->tables[db->table_count] = NULL;
            break;
        }
    }
    return kdb_storage_drop(db->data_dir, table_name);
}

KdbStatus kdb_compact(KumDB *db, const char *table_name) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_compact");
        return KDB_ERR_BAD_ARG;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_compact(tbl);
}

int kdb_table_exists(KumDB *db, const char *table_name) {
    if (!db || !table_name) return 0;
    return kdb_storage_exists(db->data_dir, table_name);
}

static _Thread_local char kdb__list_tables_buf[KDB_MAX_TABLES][KDB_MAX_NAME_LEN];

KdbStatus kdb_list_tables(KumDB       *db,
                          const char **names_out,
                          size_t       max_tables,
                          size_t      *count_out) {
    if (!db || !names_out || !count_out) {
        kdb_err_null_arg("db/names_out/count_out", "kdb_list_tables");
        return KDB_ERR_BAD_ARG;
    }

    uint32_t found = 0;
    KdbStatus st = kdb_storage_list_tables(db->data_dir, kdb__list_tables_buf, &found);
    if (st != KDB_OK) return st;

    *count_out = 0;
    for (uint32_t i = 0; i < found && *count_out < max_tables; i++) {
        names_out[(*count_out)++] = kdb__list_tables_buf[i];
    }
    return KDB_OK;
}


void kdb_rows_free(KdbRows *rows) {
    if (!rows) return;
    if (rows->rows) {
        for (size_t i = 0; i < rows->count; i++)
            kdb_row_free_internal(&rows->rows[i]);
        free(rows->rows);
    }
    free(rows);
}

void kdb_row_free(KdbRow *row) {
    if (!row) return;
    kdb_row_free_internal(row);
    free(row);
}


void kdb_row_free_internal(KdbRow *row) {
    if (!row || !row->fields) return;
    for (uint32_t i = 0; i < row->field_count; i++) {
        free((void *)row->fields[i].name);
        if (row->fields[i].type == KDB_TYPE_STRING)
            free((void *)row->fields[i].v.as_string);
        else if (row->fields[i].type == KDB_TYPE_BLOB)
            free((void *)row->fields[i].v.as_blob.data);
    }
    free(row->fields);
    row->fields      = NULL;
    row->field_count = 0;
}

const KdbField *kdb_row_get(const KdbRow *row, const char *col_name) {
    if (!row || !col_name) return NULL;
    for (uint32_t i = 0; i < row->field_count; i++) {
        if (row->fields[i].name && strcmp(row->fields[i].name, col_name) == 0)
            return &row->fields[i];
    }
    return NULL;
}

KdbStatus kdb_row_get_int(const KdbRow *row, const char *col, int64_t *out) {
    const KdbField *f = kdb_row_get(row, col);
    if (!f)                          { kdb_err_field_not_found(col, "row"); return KDB_ERR_NOT_FOUND; }
    if (f->type != KDB_TYPE_INT)     { kdb_err_bad_type(col, KDB_TYPE_INT,    (KdbType)f->type); return KDB_ERR_BAD_TYPE; }
    *out = f->v.as_int;
    return KDB_OK;
}

KdbStatus kdb_row_get_float(const KdbRow *row, const char *col, double *out) {
    const KdbField *f = kdb_row_get(row, col);
    if (!f)                          { kdb_err_field_not_found(col, "row"); return KDB_ERR_NOT_FOUND; }
    if (f->type == KDB_TYPE_INT)     { *out = (double)f->v.as_int; return KDB_OK; }
    if (f->type != KDB_TYPE_FLOAT)   { kdb_err_bad_type(col, KDB_TYPE_FLOAT,  (KdbType)f->type); return KDB_ERR_BAD_TYPE; }
    *out = f->v.as_float;
    return KDB_OK;
}

KdbStatus kdb_row_get_bool(const KdbRow *row, const char *col, int *out) {
    const KdbField *f = kdb_row_get(row, col);
    if (!f)                          { kdb_err_field_not_found(col, "row"); return KDB_ERR_NOT_FOUND; }
    if (f->type != KDB_TYPE_BOOL)    { kdb_err_bad_type(col, KDB_TYPE_BOOL,   (KdbType)f->type); return KDB_ERR_BAD_TYPE; }
    *out = f->v.as_bool;
    return KDB_OK;
}

KdbStatus kdb_row_get_string(const KdbRow *row, const char *col, const char **out) {
    const KdbField *f = kdb_row_get(row, col);
    if (!f)                          { kdb_err_field_not_found(col, "row"); return KDB_ERR_NOT_FOUND; }
    if (f->type != KDB_TYPE_STRING)  { kdb_err_bad_type(col, KDB_TYPE_STRING, (KdbType)f->type); return KDB_ERR_BAD_TYPE; }
    *out = f->v.as_string;
    return KDB_OK;
}

KdbStatus kdb_row_get_blob(const KdbRow *row, const char *col, const void **data_out, size_t *len_out) {
    const KdbField *f = kdb_row_get(row, col);
    if (!f)                          { kdb_err_field_not_found(col, "row"); return KDB_ERR_NOT_FOUND; }
    if (f->type != KDB_TYPE_BLOB)    { kdb_err_bad_type(col, KDB_TYPE_BLOB, (KdbType)f->type); return KDB_ERR_BAD_TYPE; }
    *data_out = f->v.as_blob.data;
    *len_out  = f->v.as_blob.len;
    return KDB_OK;
}


void kdb_row_print(const KdbRow *row, FILE *fp) {
    if (!row || !fp) return;
    fprintf(fp, "{ id=%llu", (unsigned long long)row->id);
    for (uint32_t i = 0; i < row->field_count; i++) {
        const KdbField *f = &row->fields[i];
        fprintf(fp, ", %s=", f->name ? f->name : "?");
        switch (f->type) {
            case KDB_TYPE_INT:    fprintf(fp, "%lld",    (long long)f->v.as_int);   break;
            case KDB_TYPE_FLOAT:  fprintf(fp, "%g",      f->v.as_float);            break;
            case KDB_TYPE_BOOL:   fprintf(fp, "%s",      f->v.as_bool ? "true" : "false"); break;
            case KDB_TYPE_STRING: fprintf(fp, "\"%s\"",  f->v.as_string ? f->v.as_string : ""); break;
            case KDB_TYPE_NULL:   fprintf(fp, "null");                               break;
            default:              fprintf(fp, "<blob>");                             break;
        }
    }
    fprintf(fp, " }\n");
}

void kdb_rows_print(const KdbRows *rows, FILE *fp) {
    if (!rows || !fp) return;
    fprintf(fp, "%zu row(s)\n", rows->count);
    for (size_t i = 0; i < rows->count; i++)
        kdb_row_print(&rows->rows[i], fp);
}

KdbStatus kdb_print_schema(KumDB *db, const char *table_name, FILE *fp) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_print_schema");
        return KDB_ERR_BAD_ARG;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    kdb_table_print_schema(tbl, fp);
    return KDB_OK;
}