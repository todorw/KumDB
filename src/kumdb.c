#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "../include/kumdb.h"
#include "../include/internal.h"
#include "../include/error.h"
#include "../include/types.h"
#include "../include/record.h"
#include "../include/table.h"
#include "../include/query.h"
#include "../include/storage.h"
#include "../include/platform.h"

static void kdb_row_free_internal(KdbRow *row);
static void kdb__evict_table(KumDB *db, const char *table_name);


const char *kdb_version(void) {
    return KDB_VERSION_MAJOR_STR "." KDB_VERSION_MINOR_STR "." KDB_VERSION_PATCH_STR;
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


/* Recursive KdbField -> KdbValue conversion (the write side). Array
 * elements/object members go through this same function, so nesting is
 * unbounded here -- kdb_value_from_array()/kdb_value_from_object() enforce
 * the real KDB_MAX_NEST_ELEMS cap per level when they deep-copy the result. */
static KdbStatus kdb__field_to_value(const KdbField *f, KdbValue *out) {
    switch (f->type) {
        case KDB_TYPE_INT:
            return kdb_value_from_int(f->v.as_int, out);
        case KDB_TYPE_FLOAT:
            return kdb_value_from_float(f->v.as_float, out);
        case KDB_TYPE_BOOL:
            return kdb_value_from_bool((uint8_t)f->v.as_bool, out);
        case KDB_TYPE_STRING:
            return kdb_value_from_string(f->v.as_string, KDB_TYPE_STRING, out);
        case KDB_TYPE_NULL:
            return kdb_value_from_null(out);
        case KDB_TYPE_BLOB:
            return kdb_value_from_blob(f->v.as_blob.data, f->v.as_blob.len, out);
        case KDB_TYPE_ARRAY: {
            size_t count = f->v.as_array.count;
            if (count == 0) return kdb_value_from_array(NULL, 0, out);

            KdbValue *elems = (KdbValue *)calloc(count, sizeof(KdbValue));
            if (!elems) { kdb_err_oom("array field elements"); return KDB_ERR_OOM; }
            for (size_t i = 0; i < count; i++) {
                if (kdb__field_to_value(&f->v.as_array.items[i], &elems[i]) != KDB_OK) {
                    for (size_t j = 0; j < i; j++) kdb_value_free(&elems[j]);
                    free(elems);
                    return kdb_last_status();
                }
            }
            KdbStatus st = kdb_value_from_array(elems, count, out);
            for (size_t i = 0; i < count; i++) kdb_value_free(&elems[i]);
            free(elems);
            return st;
        }
        case KDB_TYPE_OBJECT: {
            uint32_t count = 0;
            if (f->v.as_object) while (f->v.as_object[count].name != NULL) count++;
            if (count == 0) return kdb_value_from_object(NULL, 0, out);

            KdbRecordField *sub = (KdbRecordField *)calloc(count, sizeof(KdbRecordField));
            if (!sub) { kdb_err_oom("object field members"); return KDB_ERR_OOM; }
            for (uint32_t i = 0; i < count; i++) {
                KDB_STRLCPY(sub[i].col_name, f->v.as_object[i].name, KDB_MAX_NAME_LEN);
                if (kdb__field_to_value(&f->v.as_object[i], &sub[i].value) != KDB_OK) {
                    for (uint32_t j = 0; j < i; j++) kdb_value_free(&sub[j].value);
                    free(sub);
                    return kdb_last_status();
                }
            }
            KdbStatus st = kdb_value_from_object(sub, count, out);
            for (uint32_t i = 0; i < count; i++) kdb_value_free(&sub[i].value);
            free(sub);
            return st;
        }
        default:
            return kdb_value_from_null(out);
    }
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

        if (kdb__field_to_value(f, &val) != KDB_OK) {
            kdb_record_free(r);
            return NULL;
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


/* Recursively frees a single KdbField's owned memory: its name, and
 * whatever its value owns (string/blob data, or array/object children).
 * Safe on a partially-built field (calloc'd-but-not-yet-filled array/object
 * slots are type NULL, which is a no-op to free) and safe to call on a
 * field whose .name is NULL (array elements never have one). */
static void kdb__free_field_value(KdbField *f) {
    if (!f) return;
    free((void *)f->name);
    if (f->type == KDB_TYPE_STRING) {
        free((void *)f->v.as_string);
    } else if (f->type == KDB_TYPE_BLOB) {
        free((void *)f->v.as_blob.data);
    } else if (f->type == KDB_TYPE_ARRAY) {
        for (size_t i = 0; i < f->v.as_array.count; i++)
            kdb__free_field_value((KdbField *)&f->v.as_array.items[i]);
        free((void *)f->v.as_array.items);
    } else if (f->type == KDB_TYPE_OBJECT) {
        if (f->v.as_object) {
            for (const KdbField *sub = f->v.as_object; sub->name != NULL; sub++)
                kdb__free_field_value((KdbField *)sub);
            free((void *)f->v.as_object);
        }
    }
}

/* Recursive KdbValue -> KdbField conversion (the read side), the mirror of
 * kdb__field_to_value(). dst->name is NOT set here -- callers own that
 * (top-level row fields get a real name; array elements pass NULL). */
static KdbStatus kdb__value_to_field(const KdbValue *src, KdbField *dst) {
    dst->type = src->type;
    switch (src->type) {
        case KDB_TYPE_INT:    dst->v.as_int   = src->v.as_int;   return KDB_OK;
        case KDB_TYPE_FLOAT:  dst->v.as_float = src->v.as_float; return KDB_OK;
        case KDB_TYPE_BOOL:   dst->v.as_bool  = src->v.as_bool;  return KDB_OK;
        case KDB_TYPE_STRING: {
            char *copy = NULL;
            if (src->v.as_string.data) {
                copy = (char *)malloc(src->v.as_string.len + 1);
                if (!copy) { kdb_err_oom("row field string"); return KDB_ERR_OOM; }
                memcpy(copy, src->v.as_string.data, src->v.as_string.len + 1);
            }
            dst->v.as_string = copy;
            return KDB_OK;
        }
        case KDB_TYPE_BLOB: {
            void *copy = NULL;
            if (src->v.as_blob.len > 0 && src->v.as_blob.data) {
                copy = malloc(src->v.as_blob.len);
                if (!copy) { kdb_err_oom("row field blob"); return KDB_ERR_OOM; }
                memcpy(copy, src->v.as_blob.data, src->v.as_blob.len);
            }
            dst->v.as_blob.data = copy;
            dst->v.as_blob.len  = src->v.as_blob.len;
            return KDB_OK;
        }
        case KDB_TYPE_ARRAY: {
            size_t count = src->v.as_array.count;
            dst->v.as_array.items = NULL;
            dst->v.as_array.count = 0;
            if (count == 0) return KDB_OK;

            KdbField *items = (KdbField *)calloc(count, sizeof(KdbField));
            if (!items) { kdb_err_oom("row field array"); return KDB_ERR_OOM; }
            dst->v.as_array.items = items;
            dst->v.as_array.count = count;
            for (size_t i = 0; i < count; i++) {
                items[i].name = NULL;
                if (kdb__value_to_field(&src->v.as_array.items[i], &items[i]) != KDB_OK)
                    return KDB_ERR_OOM;
            }
            return KDB_OK;
        }
        case KDB_TYPE_OBJECT: {
            uint32_t count = src->v.as_object.count;
            /* always a valid NULL-terminated array, even when empty, so
             * callers can iterate it unconditionally */
            KdbField *fields = (KdbField *)calloc((size_t)count + 1, sizeof(KdbField));
            if (!fields) { kdb_err_oom("row field object"); return KDB_ERR_OOM; }
            dst->v.as_object = fields;
            for (uint32_t i = 0; i < count; i++) {
                char *name_copy = (char *)malloc(KDB_MAX_NAME_LEN);
                if (!name_copy) { kdb_err_oom("row field object key"); return KDB_ERR_OOM; }
                KDB_STRLCPY(name_copy, src->v.as_object.fields[i].col_name, KDB_MAX_NAME_LEN);
                fields[i].name = name_copy;
                if (kdb__value_to_field(&src->v.as_object.fields[i].value, &fields[i]) != KDB_OK)
                    return KDB_ERR_OOM;
            }
            return KDB_OK;
        }
        case KDB_TYPE_NULL:
        default:
            return KDB_OK;
    }
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
                for (uint32_t j = 0; j < i; j++) kdb__free_field_value(&row->fields[j]);
                free(row->fields);
                free(row);
                kdb_err_oom("KdbRow field name");
                return NULL;
            }
            KDB_STRLCPY(name_copy, src->col_name, KDB_MAX_NAME_LEN);
            dst->name = name_copy;

            if (kdb__value_to_field(&src->value, dst) != KDB_OK) {
                for (uint32_t j = 0; j <= i; j++) kdb__free_field_value(&row->fields[j]);
                free(row->fields);
                free(row);
                return NULL;
            }
        }
    }
    return row;
}


#define KDB_TX_MARKER_NAME ".kdb_tx_commit"

/* GCC's -Wformat-truncation can't prove these are safe: out_size is a
 * runtime parameter, not a sizeof() visible at the snprintf call site, so
 * it conservatively assumes out_size could be too small. Every call site
 * in this file passes a 4104-byte buffer, comfortably enough -- this is
 * the well-known false-positive case for that warning on generic
 * bounded-buffer helper functions. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

static void kdb__tx_backup_path(KumDB *db, const char *table_name, char *out, size_t out_size) {
    char kdb_path[4104];
    kdb_storage_path(db->data_dir, table_name, kdb_path, sizeof(kdb_path));
    snprintf(out, out_size, "%s.txbak", kdb_path);
}

static void kdb__tx_marker_path(KumDB *db, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", db->data_dir, KDB_TX_MARKER_NAME);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static KdbStatus kdb__tx_backup_table(KumDB *db, const char *table_name) {
    char src_path[4104], dst_path[4104];
    kdb_storage_path(db->data_dir, table_name, src_path, sizeof(src_path));
    kdb__tx_backup_path(db, table_name, dst_path, sizeof(dst_path));

    FILE *src = fopen(src_path, "rb");
    if (!src) { kdb_err_io(src_path, "open for tx backup"); return KDB_ERR_IO; }
    FILE *dst = fopen(dst_path, "wb");
    if (!dst) { fclose(src); kdb_err_io(dst_path, "create tx backup"); return KDB_ERR_IO; }

    char buf[65536];
    size_t n;
    KdbStatus st = KDB_OK;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { st = KDB_ERR_IO; break; }
    }
    if (st == KDB_OK && ferror(src)) st = KDB_ERR_IO;
    fclose(src);

    if (st == KDB_OK && (fflush(dst) != 0 || kdb_fsync(fileno(dst)) != 0)) st = KDB_ERR_IO;
    fclose(dst);

    if (st != KDB_OK) {
        unlink(dst_path);
        kdb_err_io(dst_path, "write tx backup");
        return st;
    }
    return KDB_OK;
}

/* Rolls back one table by putting its backup back in place. Evicts any
 * cached handle first -- an already-open fd keeps referencing the old
 * inode through a rename, so without this the cache would silently keep
 * pointing at the pre-rollback (or pre-commit-cleanup) file. */
static KdbStatus kdb__tx_restore_table(KumDB *db, const char *table_name) {
    char real_path[4104], bak_path[4104];
    kdb_storage_path(db->data_dir, table_name, real_path, sizeof(real_path));
    kdb__tx_backup_path(db, table_name, bak_path, sizeof(bak_path));

    kdb__evict_table(db, table_name);

    if (rename(bak_path, real_path) != 0) {
        kdb_err_io(real_path, "restore tx backup");
        return KDB_ERR_IO;
    }
    return KDB_OK;
}

static void kdb__tx_delete_backup(KumDB *db, const char *table_name) {
    char bak_path[4104];
    kdb__tx_backup_path(db, table_name, bak_path, sizeof(bak_path));
    unlink(bak_path);
}

static KdbStatus kdb__tx_write_marker(KumDB *db, char tables[][KDB_MAX_NAME_LEN], uint32_t count) {
    char marker_path[4104];
    kdb__tx_marker_path(db, marker_path, sizeof(marker_path));

    char buf[KDB_TX_MAX_TABLES * KDB_MAX_NAME_LEN];
    size_t pos = 0;
    for (uint32_t i = 0; i < count && pos < sizeof(buf); i++) {
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s\n", tables[i]);
        if (n > 0) {
            size_t avail = sizeof(buf) - pos;
            pos += (size_t)n < avail ? (size_t)n : avail;
        }
    }

    return kdb_atomic_write(marker_path, (const uint8_t *)buf, pos);
}

static void kdb__tx_delete_marker(KumDB *db) {
    char marker_path[4104];
    kdb__tx_marker_path(db, marker_path, sizeof(marker_path));
    unlink(marker_path);
}

/* Called on every read-write kdb_open(). Cheap no-op in the common case
 * (no marker, no leftover .txbak files). Two cases to resolve:
 *
 *   1. Marker present -> a transaction had already reached the "safe to
 *      discard backups" point before the process died. Finish that
 *      cleanup; do NOT roll back, the data is correctly committed.
 *   2. No marker, but some table has a leftover .txbak -> a transaction
 *      was interrupted before it ever got that far. Roll it back.
 */
static void kdb__tx_recover(KumDB *db) {
    char marker_path[4104];
    kdb__tx_marker_path(db, marker_path, sizeof(marker_path));

    FILE *mf = fopen(marker_path, "rb");
    if (mf) {
        char line[KDB_MAX_NAME_LEN];
        while (fgets(line, sizeof(line), mf)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
            if (len > 0) kdb__tx_delete_backup(db, line);
        }
        fclose(mf);
        unlink(marker_path);
    }

    DIR *dir = opendir(db->data_dir);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t nlen = strlen(name);
        static const char suffix[] = ".kdb.txbak";
        size_t slen = sizeof(suffix) - 1;
        if (nlen <= slen || strcmp(name + nlen - slen, suffix) != 0) continue;

        char table_name[KDB_MAX_NAME_LEN];
        size_t base_len = nlen - slen;
        if (base_len >= sizeof(table_name)) continue;
        memcpy(table_name, name, base_len);
        table_name[base_len] = '\0';

        kdb__tx_restore_table(db, table_name);
    }
    closedir(dir);
}

/* Tracks 'table_name' in the transaction exactly once: backs it up (or,
 * if it doesn't exist yet, remembers to drop it on rollback instead). */
static KdbStatus kdb__tx_touch(KdbTx *tx, const char *table_name) {
    for (uint32_t i = 0; i < tx->table_count; i++) {
        if (strcmp(tx->tables[i], table_name) == 0) return KDB_OK;
    }
    if (tx->table_count >= KDB_TX_MAX_TABLES) {
        kdb_set_error(KDB_ERR_FULL, "Transaction already touches %d tables, that's the limit.", KDB_TX_MAX_TABLES);
        return KDB_ERR_FULL;
    }

    uint32_t idx = tx->table_count;
    KDB_STRLCPY(tx->tables[idx], table_name, sizeof(tx->tables[idx]));

    if (!kdb_table_exists(tx->db, table_name)) {
        tx->is_new_table[idx] = 1;
    } else {
        tx->is_new_table[idx] = 0;
        KdbStatus st = kdb__tx_backup_table(tx->db, table_name);
        if (st != KDB_OK) return st;
    }

    tx->table_count++;
    return KDB_OK;
}

KdbTx *kdb_tx_begin(KumDB *db) {
    if (!db) { kdb_err_null_arg("db", "kdb_tx_begin"); return NULL; }
    if (db->read_only) { kdb_err_table_read_only("(transaction)"); return NULL; }

    KdbTx *tx = KDB_ALLOC(KdbTx);
    if (!tx) { kdb_err_oom("KdbTx"); return NULL; }
    tx->db     = db;
    tx->active = 1;
    return tx;
}

KdbStatus kdb_tx_add(KdbTx *tx, const char *table_name, const KdbField *fields) {
    if (!tx || !tx->active) { kdb_err_bad_arg("tx", "not an active transaction"); return KDB_ERR_BAD_ARG; }
    if (tx->failed) {
        kdb_set_error(KDB_ERR_VALIDATION, "Transaction already has a failed operation -- roll it back.");
        return KDB_ERR_VALIDATION;
    }

    KdbStatus st = kdb__tx_touch(tx, table_name);
    if (st != KDB_OK) { tx->failed = 1; return st; }

    st = kdb_add(tx->db, table_name, fields);
    if (st != KDB_OK) tx->failed = 1;
    return st;
}

KdbStatus kdb_tx_update(KdbTx *tx, const char *table_name, const char **where_filters,
                        const KdbField *set_fields, size_t *updated_out) {
    if (!tx || !tx->active) { kdb_err_bad_arg("tx", "not an active transaction"); return KDB_ERR_BAD_ARG; }
    if (tx->failed) {
        kdb_set_error(KDB_ERR_VALIDATION, "Transaction already has a failed operation -- roll it back.");
        return KDB_ERR_VALIDATION;
    }

    KdbStatus st = kdb__tx_touch(tx, table_name);
    if (st != KDB_OK) { tx->failed = 1; return st; }

    st = kdb_update(tx->db, table_name, where_filters, set_fields, updated_out);
    if (st != KDB_OK) tx->failed = 1;
    return st;
}

KdbStatus kdb_tx_delete(KdbTx *tx, const char *table_name, const char **filters, size_t *deleted_out) {
    if (!tx || !tx->active) { kdb_err_bad_arg("tx", "not an active transaction"); return KDB_ERR_BAD_ARG; }
    if (tx->failed) {
        kdb_set_error(KDB_ERR_VALIDATION, "Transaction already has a failed operation -- roll it back.");
        return KDB_ERR_VALIDATION;
    }

    KdbStatus st = kdb__tx_touch(tx, table_name);
    if (st != KDB_OK) { tx->failed = 1; return st; }

    st = kdb_delete(tx->db, table_name, filters, deleted_out);
    if (st != KDB_OK) tx->failed = 1;
    return st;
}

KdbStatus kdb_tx_commit(KdbTx *tx) {
    if (!tx || !tx->active) { kdb_err_bad_arg("tx", "not an active transaction"); return KDB_ERR_BAD_ARG; }
    if (tx->failed) {
        kdb_set_error(KDB_ERR_VALIDATION, "Transaction had a failed operation -- can't commit, roll it back instead.");
        return KDB_ERR_VALIDATION;
    }

    char backed_up[KDB_TX_MAX_TABLES][KDB_MAX_NAME_LEN];
    uint32_t nbacked = 0;
    for (uint32_t i = 0; i < tx->table_count; i++) {
        if (!tx->is_new_table[i]) {
            KDB_STRLCPY(backed_up[nbacked], tx->tables[i], sizeof(backed_up[nbacked]));
            nbacked++;
        }
    }

    if (nbacked > 0) {
        KdbStatus st = kdb__tx_write_marker(tx->db, backed_up, nbacked);
        if (st != KDB_OK) return st; /* not committed yet -- tx is still valid, retry or roll back */

        for (uint32_t i = 0; i < nbacked; i++) kdb__tx_delete_backup(tx->db, backed_up[i]);
        kdb__tx_delete_marker(tx->db);
    }

    free(tx);
    return KDB_OK;
}

KdbStatus kdb_tx_rollback(KdbTx *tx) {
    if (!tx || !tx->active) { kdb_err_bad_arg("tx", "not an active transaction"); return KDB_ERR_BAD_ARG; }

    KdbStatus first_err = KDB_OK;
    for (uint32_t i = 0; i < tx->table_count; i++) {
        KdbStatus st = tx->is_new_table[i]
            ? kdb_drop_table(tx->db, tx->tables[i])
            : kdb__tx_restore_table(tx->db, tx->tables[i]);
        if (st != KDB_OK && first_err == KDB_OK) first_err = st;
    }

    free(tx);
    return first_err;
}

static KumDB *kdb__open_internal(const char *data_dir, uint8_t read_only) {
    if (!data_dir) {
        kdb_err_null_arg("data_dir", "kdb_open");
        return NULL;
    }

    
    struct stat st;
    if (stat(data_dir, &st) != 0) {
        if (!read_only) {
            kdb_mkdir(data_dir);
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

    if (!read_only) kdb__tx_recover(db);

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

KdbStatus kdb_add_column(KumDB *db, const char *table_name, const char *col_name,
                         KdbFieldType type, int nullable, int indexed) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_add_column");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_add_column(tbl, col_name, (KdbType)type, nullable ? 1 : 0, indexed ? 1 : 0);
}

KdbStatus kdb_drop_column(KumDB *db, const char *table_name, const char *col_name) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_drop_column");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_drop_column(tbl, col_name);
}

/* Closes and evicts a table's cached handle, if open. Needed anywhere a
 * table's file gets replaced/renamed out from under a live process --
 * without this, an already-open fd keeps referencing the old inode and the
 * cached handle silently goes stale relative to what's actually on disk. */
static void kdb__evict_table(KumDB *db, const char *table_name) {
    for (uint32_t i = 0; i < db->table_count; i++) {
        if (db->tables[i] && strcmp(db->tables[i]->name, table_name) == 0) {
            kdb_table_close(db->tables[i]);
            free(db->tables[i]);
            db->tables[i] = db->tables[--db->table_count];
            db->tables[db->table_count] = NULL;
            return;
        }
    }
}

KdbStatus kdb_drop_table(KumDB *db, const char *table_name) {
    if (!db || !table_name) {
        kdb_err_null_arg("db/table_name", "kdb_drop_table");
        return KDB_ERR_BAD_ARG;
    }

    kdb__evict_table(db, table_name);
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
    for (uint32_t i = 0; i < row->field_count; i++)
        kdb__free_field_value(&row->fields[i]);
    free(row->fields);
    row->fields      = NULL;
    row->field_count = 0;
}

static const KdbField *kdb__find_field(const KdbField *fields, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++) {
        if (fields[i].name && strcmp(fields[i].name, name) == 0) return &fields[i];
    }
    return NULL;
}

static const KdbField *kdb__find_field_in_object(const KdbField *obj, const char *name) {
    for (const KdbField *f = obj; f && f->name; f++) {
        if (strcmp(f->name, name) == 0) return f;
    }
    return NULL;
}

/* Same dot-path rule as kdb_record_get_field (record.c): try an exact
 * field-name match first (the fast common case, and the only sane
 * interpretation of a JOIN-qualified name like "u.name" -- a literal
 * field name at the SQL layer, never a nested lookup), and only fall back
 * to resolving "address.city" as field "address" (must be KDB_TYPE_OBJECT)
 * then "city" among its own fields if no exact match exists. No dot-path
 * into an ARRAY -- elements there are unnamed. */
const KdbField *kdb_row_get(const KdbRow *row, const char *col_name) {
    if (!row || !col_name) return NULL;

    const KdbField *exact = kdb__find_field(row->fields, row->field_count, col_name);
    if (exact) return exact;

    const char *dot = strchr(col_name, '.');
    if (!dot) return NULL;

    char head[KDB_MAX_NAME_LEN];
    size_t head_len = (size_t)(dot - col_name);
    if (head_len == 0 || head_len >= sizeof(head)) return NULL;
    memcpy(head, col_name, head_len);
    head[head_len] = '\0';

    const KdbField *f = kdb__find_field(row->fields, row->field_count, head);
    const char *rest = dot + 1;

    for (;;) {
        if (!f || f->type != KDB_TYPE_OBJECT) return NULL;

        const char *next_dot = strchr(rest, '.');
        if (!next_dot) return kdb__find_field_in_object(f->v.as_object, rest);

        head_len = (size_t)(next_dot - rest);
        if (head_len == 0 || head_len >= sizeof(head)) return NULL;
        memcpy(head, rest, head_len);
        head[head_len] = '\0';

        f = kdb__find_field_in_object(f->v.as_object, head);
        rest = next_dot + 1;
    }
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

KdbStatus kdb_row_get_array(const KdbRow *row, const char *col, const KdbField **items_out, size_t *count_out) {
    const KdbField *f = kdb_row_get(row, col);
    if (!f)                          { kdb_err_field_not_found(col, "row"); return KDB_ERR_NOT_FOUND; }
    if (f->type != KDB_TYPE_ARRAY)   { kdb_err_bad_type(col, KDB_TYPE_ARRAY, (KdbType)f->type); return KDB_ERR_BAD_TYPE; }
    *items_out = f->v.as_array.items;
    *count_out = f->v.as_array.count;
    return KDB_OK;
}

KdbStatus kdb_row_get_object(const KdbRow *row, const char *col, const KdbField **fields_out) {
    const KdbField *f = kdb_row_get(row, col);
    if (!f)                          { kdb_err_field_not_found(col, "row"); return KDB_ERR_NOT_FOUND; }
    if (f->type != KDB_TYPE_OBJECT)  { kdb_err_bad_type(col, KDB_TYPE_OBJECT, (KdbType)f->type); return KDB_ERR_BAD_TYPE; }
    *fields_out = f->v.as_object;
    return KDB_OK;
}


static void kdb__print_field_value(FILE *fp, const KdbField *f) {
    switch (f->type) {
        case KDB_TYPE_INT:    fprintf(fp, "%lld",   (long long)f->v.as_int);   break;
        case KDB_TYPE_FLOAT:  fprintf(fp, "%g",     f->v.as_float);            break;
        case KDB_TYPE_BOOL:   fprintf(fp, "%s",     f->v.as_bool ? "true" : "false"); break;
        case KDB_TYPE_STRING: fprintf(fp, "\"%s\"", f->v.as_string ? f->v.as_string : ""); break;
        case KDB_TYPE_NULL:   fprintf(fp, "null");                             break;
        case KDB_TYPE_BLOB:   fprintf(fp, "<blob:%zu bytes>", f->v.as_blob.len); break;
        case KDB_TYPE_ARRAY:
            fprintf(fp, "[");
            for (size_t i = 0; i < f->v.as_array.count; i++) {
                if (i > 0) fprintf(fp, ", ");
                kdb__print_field_value(fp, &f->v.as_array.items[i]);
            }
            fprintf(fp, "]");
            break;
        case KDB_TYPE_OBJECT:
            fprintf(fp, "{");
            if (f->v.as_object) {
                int first = 1;
                for (const KdbField *sub = f->v.as_object; sub->name != NULL; sub++) {
                    if (!first) fprintf(fp, ", ");
                    fprintf(fp, "%s: ", sub->name);
                    kdb__print_field_value(fp, sub);
                    first = 0;
                }
            }
            fprintf(fp, "}");
            break;
        default:
            fprintf(fp, "<unknown>");
            break;
    }
}

void kdb_row_print(const KdbRow *row, FILE *fp) {
    if (!row || !fp) return;
    fprintf(fp, "{ id=%llu", (unsigned long long)row->id);
    for (uint32_t i = 0; i < row->field_count; i++) {
        const KdbField *f = &row->fields[i];
        fprintf(fp, ", %s=", f->name ? f->name : "?");
        kdb__print_field_value(fp, f);
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