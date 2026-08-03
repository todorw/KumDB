#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
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

/* A savepoint's own backup for one table -- suffixed by the savepoint's
 * stack depth (unique within one transaction at any given time, since
 * depths are only reused after the savepoint that held them is popped
 * and its own backup already deleted -- see kdb_tx_rollback_to_savepoint/
 * kdb_tx_release_savepoint). Recognizable (and cleaned up as orphaned
 * garbage on crash recovery) by containing ".txbak.sp" -- see
 * kdb__tx_recover. */
static void kdb__tx_savepoint_backup_path(KumDB *db, const char *table_name, uint32_t depth, char *out, size_t out_size) {
    char kdb_path[4104];
    kdb_storage_path(db->data_dir, table_name, kdb_path, sizeof(kdb_path));
    snprintf(out, out_size, "%s.txbak.sp%u", kdb_path, depth);
}

static void kdb__tx_marker_path(KumDB *db, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", db->data_dir, KDB_TX_MARKER_NAME);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static KdbStatus kdb__tx_backup_table_to(KumDB *db, const char *table_name, const char *dst_path) {
    char src_path[4104];
    kdb_storage_path(db->data_dir, table_name, src_path, sizeof(src_path));

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

static KdbStatus kdb__tx_backup_table(KumDB *db, const char *table_name) {
    char dst_path[4104];
    kdb__tx_backup_path(db, table_name, dst_path, sizeof(dst_path));
    return kdb__tx_backup_table_to(db, table_name, dst_path);
}

/* Rolls back one table by putting its backup back in place. Evicts any
 * cached handle first -- an already-open fd keeps referencing the old
 * inode through a rename, so without this the cache would silently keep
 * pointing at the pre-rollback (or pre-commit-cleanup) file. */
static KdbStatus kdb__tx_restore_table_from(KumDB *db, const char *table_name, const char *bak_path) {
    char real_path[4104];
    kdb_storage_path(db->data_dir, table_name, real_path, sizeof(real_path));

    kdb__evict_table(db, table_name);

    if (rename(bak_path, real_path) != 0) {
        kdb_err_io(real_path, "restore tx backup");
        return KDB_ERR_IO;
    }
    return KDB_OK;
}

static KdbStatus kdb__tx_restore_table(KumDB *db, const char *table_name) {
    char bak_path[4104];
    kdb__tx_backup_path(db, table_name, bak_path, sizeof(bak_path));
    return kdb__tx_restore_table_from(db, table_name, bak_path);
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

        /* a leftover savepoint-level backup is always orphaned garbage by
         * the time we get here -- the transaction that made it either
         * committed already (savepoint backups are cleaned up as part of
         * that, see kdb_tx_commit) or needs the whole-transaction
         * rollback below, which makes every savepoint within it moot.
         * Just discard it, regardless of which table/depth. */
        if (strstr(name, ".kdb.txbak.sp") != NULL) {
            char path[4104];
            snprintf(path, sizeof(path), "%s/%s", db->data_dir, name);
            unlink(path);
            continue;
        }

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
 * if it doesn't exist yet, remembers to drop it on rollback instead).
 * Separately (and regardless of whether the transaction as a whole has
 * already seen this table before), also tracks it in every currently
 * active savepoint that hasn't seen it yet since ITS OWN creation --
 * each such savepoint gets its own backup of the table's current state,
 * so kdb_tx_rollback_to_savepoint can undo back to exactly that point
 * later without needing to touch any of the others. */
static KdbStatus kdb__tx_touch(KdbTx *tx, const char *table_name) {
    int already_tracked = 0;
    for (uint32_t i = 0; i < tx->table_count; i++) {
        if (strcmp(tx->tables[i], table_name) == 0) { already_tracked = 1; break; }
    }
    if (!already_tracked) {
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
    }

    for (uint32_t sp = 0; sp < tx->savepoint_count; sp++) {
        KdbSavepoint *s = &tx->savepoints[sp];
        int seen = 0;
        for (uint32_t i = 0; i < s->table_count; i++) {
            if (strcmp(s->tables[i], table_name) == 0) { seen = 1; break; }
        }
        if (seen) continue;
        if (s->table_count >= KDB_TX_MAX_TABLES) {
            kdb_set_error(KDB_ERR_FULL, "Savepoint '%s' already touches %d tables, that's the limit.", s->name, KDB_TX_MAX_TABLES);
            return KDB_ERR_FULL;
        }

        uint32_t idx = s->table_count;
        KDB_STRLCPY(s->tables[idx], table_name, sizeof(s->tables[idx]));

        if (!kdb_table_exists(tx->db, table_name)) {
            s->is_new_table[idx] = 1;
        } else {
            s->is_new_table[idx] = 0;
            char bak_path[4104];
            kdb__tx_savepoint_backup_path(tx->db, table_name, sp, bak_path, sizeof(bak_path));
            KdbStatus st = kdb__tx_backup_table_to(tx->db, table_name, bak_path);
            if (st != KDB_OK) return st;
        }

        s->table_count++;
    }

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

/* Deletes every active savepoint's own backup files -- called right
 * before a whole-transaction commit or rollback ends tx, since either way
 * makes every savepoint within it moot: commit means nothing needs
 * undoing, and the whole-transaction rollback below already restores
 * each table by itself, without going through any savepoint's own
 * backup. */
static void kdb__tx_cleanup_savepoints(KdbTx *tx) {
    for (uint32_t sp = 0; sp < tx->savepoint_count; sp++) {
        KdbSavepoint *s = &tx->savepoints[sp];
        for (uint32_t i = 0; i < s->table_count; i++) {
            if (!s->is_new_table[i]) {
                char bak_path[4104];
                kdb__tx_savepoint_backup_path(tx->db, s->tables[i], sp, bak_path, sizeof(bak_path));
                unlink(bak_path);
            }
        }
    }
    tx->savepoint_count = 0;
}

KdbStatus kdb_tx_commit(KdbTx *tx) {
    if (!tx || !tx->active) { kdb_err_bad_arg("tx", "not an active transaction"); return KDB_ERR_BAD_ARG; }
    if (tx->failed) {
        kdb_set_error(KDB_ERR_VALIDATION, "Transaction had a failed operation -- can't commit, roll it back instead.");
        return KDB_ERR_VALIDATION;
    }

    kdb__tx_cleanup_savepoints(tx);

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

    kdb__tx_cleanup_savepoints(tx);

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

/* Finds an active savepoint by name; -1 if none matches. */
static int32_t kdb__tx_find_savepoint(KdbTx *tx, const char *name) {
    for (uint32_t i = 0; i < tx->savepoint_count; i++) {
        if (strcmp(tx->savepoints[i].name, name) == 0) return (int32_t)i;
    }
    return -1;
}

KdbStatus kdb_tx_savepoint(KdbTx *tx, const char *name) {
    if (!tx || !tx->active || !name || !name[0]) {
        kdb_err_bad_arg("tx/name", "not an active transaction, or no savepoint name given");
        return KDB_ERR_BAD_ARG;
    }
    if (tx->failed) {
        kdb_set_error(KDB_ERR_VALIDATION, "Transaction already has a failed operation -- roll it back.");
        return KDB_ERR_VALIDATION;
    }
    if (kdb__tx_find_savepoint(tx, name) >= 0) {
        kdb_set_error(KDB_ERR_EXISTS, "Savepoint '%s' already exists in this transaction.", name);
        return KDB_ERR_EXISTS;
    }
    if (tx->savepoint_count >= KDB_TX_MAX_SAVEPOINTS) {
        kdb_set_error(KDB_ERR_FULL, "Transaction already has the max %d savepoints.", KDB_TX_MAX_SAVEPOINTS);
        return KDB_ERR_FULL;
    }

    KdbSavepoint *s = &tx->savepoints[tx->savepoint_count];
    memset(s, 0, sizeof(*s));
    KDB_STRLCPY(s->name, name, sizeof(s->name));
    tx->savepoint_count++;
    return KDB_OK;
}

KdbStatus kdb_tx_rollback_to_savepoint(KdbTx *tx, const char *name) {
    if (!tx || !tx->active || !name) {
        kdb_err_bad_arg("tx/name", "not an active transaction, or no savepoint name given");
        return KDB_ERR_BAD_ARG;
    }
    int32_t idx = kdb__tx_find_savepoint(tx, name);
    if (idx < 0) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "No savepoint named '%s' in this transaction.", name);
        return KDB_ERR_NOT_FOUND;
    }

    /* discard every savepoint nested inside this one first -- their own
     * tracked state no longer makes sense once we've rewound past their
     * creation point (their backups are never restored, just dropped). */
    for (uint32_t sp = tx->savepoint_count; sp > (uint32_t)idx + 1; ) {
        sp--;
        KdbSavepoint *s = &tx->savepoints[sp];
        for (uint32_t i = 0; i < s->table_count; i++) {
            if (!s->is_new_table[i]) {
                char bak_path[4104];
                kdb__tx_savepoint_backup_path(tx->db, s->tables[i], sp, bak_path, sizeof(bak_path));
                unlink(bak_path);
            }
        }
    }

    /* now restore this savepoint's own tracked tables from ITS backups */
    KdbSavepoint *target = &tx->savepoints[idx];
    KdbStatus first_err = KDB_OK;
    for (uint32_t i = 0; i < target->table_count; i++) {
        KdbStatus st;
        if (target->is_new_table[i]) {
            st = kdb_drop_table(tx->db, target->tables[i]);
        } else {
            char bak_path[4104];
            kdb__tx_savepoint_backup_path(tx->db, target->tables[i], (uint32_t)idx, bak_path, sizeof(bak_path));
            st = kdb__tx_restore_table_from(tx->db, target->tables[i], bak_path);
        }
        if (st != KDB_OK && first_err == KDB_OK) first_err = st;
    }

    /* this savepoint stays active (same name, same depth), just reset to
     * track nothing yet -- ready to back up fresh touches again from
     * here, same as right after it was first created */
    tx->savepoint_count = (uint32_t)idx + 1;
    memset(target->tables, 0, sizeof(target->tables));
    memset(target->is_new_table, 0, sizeof(target->is_new_table));
    target->table_count = 0;

    tx->failed = 0; /* same recovery-escape-hatch reasoning kdb_tx_rollback() has */
    return first_err;
}

KdbStatus kdb_tx_release_savepoint(KdbTx *tx, const char *name) {
    if (!tx || !tx->active || !name) {
        kdb_err_bad_arg("tx/name", "not an active transaction, or no savepoint name given");
        return KDB_ERR_BAD_ARG;
    }
    int32_t idx = kdb__tx_find_savepoint(tx, name);
    if (idx < 0) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "No savepoint named '%s' in this transaction.", name);
        return KDB_ERR_NOT_FOUND;
    }

    /* releasing a savepoint also releases everything nested inside it,
     * same as real SQL -- delete every one of their backup files (giving
     * up the ability to roll back to any of them specifically) without
     * restoring anything. */
    for (uint32_t sp = (uint32_t)idx; sp < tx->savepoint_count; sp++) {
        KdbSavepoint *s = &tx->savepoints[sp];
        for (uint32_t i = 0; i < s->table_count; i++) {
            if (!s->is_new_table[i]) {
                char bak_path[4104];
                kdb__tx_savepoint_backup_path(tx->db, s->tables[i], sp, bak_path, sizeof(bak_path));
                unlink(bak_path);
            }
        }
    }
    tx->savepoint_count = (uint32_t)idx;
    return KDB_OK;
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
    /* a SQL BEGIN left un-COMMIT-ed/un-ROLLBACK-ed when the handle closes
     * -- roll it back now rather than leaking the KdbTx and its backup
     * files; leaving them for kdb__tx_recover() on the next kdb_open()
     * would also be correct (that's exactly what a real crash mid-
     * transaction looks like to this engine) but doing it here is
     * immediate and frees tx right away. */
    if (db->sql_tx) { kdb_tx_rollback((KdbTx *)db->sql_tx); db->sql_tx = NULL; }
    for (uint32_t i = 0; i < db->table_count; i++) {
        if (db->tables[i]) {
            kdb_table_close(db->tables[i]);
            free(db->tables[i]);
            db->tables[i] = NULL;
        }
    }
    free(db);
}


/* Child-side FK enforcement: for every column in tbl with a foreign key,
 * if the row's value for it is non-NULL, the referenced table must
 * already have a row whose ref_col equals it. Lives here (not table.c)
 * because it needs KumDB* to open the referenced table by name -- a bare
 * KdbTable* handle only knows its own schema. Used by kdb_add_validated
 * and kdb_update (and, transitively, kdb_tx_add/kdb_tx_update and every
 * SQL INSERT/UPDATE, all of which route through those two). */
static KdbStatus kdb__check_fk_child(KumDB *db, KdbTable *tbl, const KdbRecord *r) {
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        const KdbColumn *col = &tbl->header.columns[i];
        if (!col->has_fk) continue;

        const KdbRecordField *f = kdb_record_get_field(r, col->name);
        if (!f || f->value.type == KDB_TYPE_NULL) continue; /* a NULL FK value is never checked, same as UNIQUE */

        KdbTable *ref_tbl = kdb__get_table(db, col->fk_ref_table);
        if (!ref_tbl) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Foreign key on '%s.%s' references table '%s', which doesn't exist (or couldn't be opened).",
                tbl->name, col->name, col->fk_ref_table);
            return KDB_ERR_VALIDATION;
        }

        KdbQuery q;
        kdb_query_init(&q);
        KdbStatus st = kdb_query_add_filter_value(&q, col->fk_ref_col, KDB_OP_EQ, &f->value, NULL);
        if (st != KDB_OK) { kdb_query_free(&q); return st; }
        size_t count = 0;
        st = kdb_query_count(ref_tbl, &q, &count);
        kdb_query_free(&q);
        if (st != KDB_OK) return st;

        if (count == 0) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Foreign key violation: '%s.%s' has no matching row in '%s.%s'.",
                tbl->name, col->name, col->fk_ref_table, col->fk_ref_col);
            return KDB_ERR_VALIDATION;
        }
    }
    return KDB_OK;
}

/* Same idea for UPDATE's would-be result -- only columns the patch
 * actually touches (an untouched FK column can't become invalid). */
static KdbStatus kdb__check_fk_child_update(KumDB *db, KdbTable *tbl, const KdbRecord *patch) {
    for (uint32_t i = 0; i < patch->field_count; i++) {
        const KdbColumn *col = NULL;
        for (uint32_t c = 0; c < tbl->header.column_count; c++) {
            if (strcmp(tbl->header.columns[c].name, patch->fields[i].col_name) == 0) { col = &tbl->header.columns[c]; break; }
        }
        if (!col || !col->has_fk) continue;

        const KdbValue *val = &patch->fields[i].value;
        if (val->type == KDB_TYPE_NULL) continue;

        KdbTable *ref_tbl = kdb__get_table(db, col->fk_ref_table);
        if (!ref_tbl) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Foreign key on '%s.%s' references table '%s', which doesn't exist (or couldn't be opened).",
                tbl->name, col->name, col->fk_ref_table);
            return KDB_ERR_VALIDATION;
        }

        KdbQuery q;
        kdb_query_init(&q);
        KdbStatus st = kdb_query_add_filter_value(&q, col->fk_ref_col, KDB_OP_EQ, val, NULL);
        if (st != KDB_OK) { kdb_query_free(&q); return st; }
        size_t count = 0;
        st = kdb_query_count(ref_tbl, &q, &count);
        kdb_query_free(&q);
        if (st != KDB_OK) return st;

        if (count == 0) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Foreign key violation: '%s.%s' has no matching row in '%s.%s'.",
                tbl->name, col->name, col->fk_ref_table, col->fk_ref_col);
            return KDB_ERR_VALIDATION;
        }
    }
    return KDB_OK;
}

typedef struct {
    char    table[KDB_MAX_NAME_LEN];
    char    col[KDB_MAX_NAME_LEN];
    uint8_t on_delete;
    uint8_t on_update;
} KdbFkReferencer;

/* Every (table, column) anywhere in the data directory whose FK points at
 * (ref_table_name, ref_col_name) -- scans every table's own schema (not
 * just already-open ones, via kdb_storage_list_tables + kdb__get_table),
 * so this also catches a referencing table this KumDB handle hasn't
 * touched yet this session. Fine at the table counts this engine targets
 * -- not cached, re-scanned on every call. */
static KdbStatus kdb__find_fk_referencers(KumDB *db, const char *ref_table_name, const char *ref_col_name,
                                          KdbFkReferencer *out, uint32_t max_out, uint32_t *count_out) {
    *count_out = 0;
    char names[KDB_MAX_TABLES][KDB_MAX_NAME_LEN];
    uint32_t n = 0;
    KdbStatus st = kdb_storage_list_tables(db->data_dir, names, &n);
    if (st != KDB_OK) return st;

    for (uint32_t i = 0; i < n && *count_out < max_out; i++) {
        KdbTable *t = kdb__get_table(db, names[i]);
        if (!t) continue;
        for (uint32_t c = 0; c < t->header.column_count && *count_out < max_out; c++) {
            const KdbColumn *col = &t->header.columns[c];
            if (col->has_fk && strcmp(col->fk_ref_table, ref_table_name) == 0 && strcmp(col->fk_ref_col, ref_col_name) == 0) {
                snprintf(out[*count_out].table, sizeof(out[*count_out].table), "%.127s", names[i]);
                snprintf(out[*count_out].col, sizeof(out[*count_out].col), "%.127s", col->name);
                out[*count_out].on_delete = col->on_delete;
                out[*count_out].on_update = col->on_update;
                (*count_out)++;
            }
        }
    }
    return KDB_OK;
}

#define KDB_MAX_FK_REFERENCERS 32

/* Forward declaration -- kdb__cascade_set_one_field (just below) needs to
 * recurse into kdb__enforce_fk_referential_actions itself (a CASCADE/SET
 * NULL propagated onto a referencing table might itself be referenced by
 * yet another table), which is defined right after it. */
static KdbStatus kdb__enforce_fk_referential_actions(KumDB *db, KdbTable *tbl, const KdbResult *affected,
                                                      const KdbRecord *patch, int depth);

/* Same idea for the composite (multi-column) FK counterpart, defined
 * further below -- kdb__enforce_fk_referential_actions' DELETE CASCADE
 * branch also runs composite checks against the rows it's about to
 * cascade-delete (a table can have both single-column and composite FKs
 * pointing elsewhere), and kdb__cascade_set_composite_fields needs to
 * recurse into it the same way kdb__cascade_set_one_field recurses into
 * the single-column version. */
static KdbStatus kdb__enforce_composite_fk_referential_actions(KumDB *db, KdbTable *tbl, const KdbResult *affected,
                                                                const KdbRecord *patch, int depth);

/* Runs a single-field KdbRecord patch (col_name = val, or NULL for SET
 * NULL) through kdb_table_update directly -- used for CASCADE (propagate
 * the new referenced value) and SET NULL, neither of which needs (or
 * safely can use, see the CASCADE case below) kdb__check_fk_child_update
 * re-validation the way a genuinely new user-supplied value would. */
static KdbStatus kdb__cascade_set_one_field(KumDB *db, KdbTable *tbl, const KdbQuery *q,
                                            const char *col_name, const KdbValue *val, int depth) {
    if (depth > KDB_FK_MAX_CASCADE_DEPTH) {
        kdb_set_error(KDB_ERR_VALIDATION, "Foreign key cascade chain too deep (max %d) -- check for a cycle.", KDB_FK_MAX_CASCADE_DEPTH);
        return KDB_ERR_VALIDATION;
    }

    KdbRecordField pf;
    memset(&pf, 0, sizeof(pf));
    KDB_STRLCPY(pf.col_name, col_name, KDB_MAX_NAME_LEN);
    KdbStatus st = val ? kdb_value_copy(val, &pf.value) : kdb_value_from_null(&pf.value);
    if (st != KDB_OK) return st;

    KdbRecord patch;
    memset(&patch, 0, sizeof(patch));
    patch.fields = &pf;
    patch.field_count = 1;

    KdbResult affected;
    st = kdb_query_execute(tbl, q, &affected);
    if (st != KDB_OK) { kdb_value_free(&pf.value); return st; }
    st = kdb__enforce_fk_referential_actions(db, tbl, &affected, &patch, depth);
    kdb_result_free(&affected);
    if (st == KDB_OK) {
        size_t updated = 0;
        st = kdb_table_update(tbl, q, &patch, &updated);
    }
    kdb_value_free(&pf.value);
    return st;
}

/* On DELETE/UPDATE of a row in tbl, decides what happens to every other
 * table's FK still pointing at it -- independently per referencer, each
 * following its own on_delete (patch==NULL) or on_update (patch!=NULL,
 * only for a column the patch actually touches -- one it doesn't touch
 * can't orphan anyone) action:
 *   KDB_FK_RESTRICT  -- rejects the write if any referencing row remains
 *                        (the original, and until now only, behavior).
 *   KDB_FK_CASCADE    -- deletes the referencing rows too (on delete), or
 *                        propagates the new value to them (on update).
 *   KDB_FK_SET_NULL   -- sets the referencing rows' FK column to NULL.
 * CASCADE/SET_NULL recurse (via kdb__cascade_set_one_field/kdb__delete_
 * with_depth), so a chain across several tables works the same way, but
 * depth is capped (KDB_FK_MAX_CASCADE_DEPTH) to fail cleanly on a cycle
 * instead of recursing forever. affected is the exact row set this
 * DELETE/UPDATE matches, fetched by the caller before anything is
 * actually mutated. */
static KdbStatus kdb__enforce_fk_referential_actions(KumDB *db, KdbTable *tbl, const KdbResult *affected,
                                                      const KdbRecord *patch, int depth) {
    if (depth > KDB_FK_MAX_CASCADE_DEPTH) {
        kdb_set_error(KDB_ERR_VALIDATION, "Foreign key cascade chain too deep (max %d) -- check for a cycle.", KDB_FK_MAX_CASCADE_DEPTH);
        return KDB_ERR_VALIDATION;
    }

    /* Real columns plus the three always-present pseudo-columns (id/
     * created_at/updated_at, tracked on KdbRecord itself, not in
     * tbl->header.columns[]) -- "id" is the single most common FOREIGN
     * KEY reference target, so this can't just loop over header.columns. */
    uint32_t ncols = tbl->header.column_count;
    const char *pseudo_names[3] = { "id", "created_at", "updated_at" };

    for (uint32_t ci = 0; ci < ncols + 3; ci++) {
        const char *col_name = (ci < ncols) ? tbl->header.columns[ci].name : pseudo_names[ci - ncols];

        if (patch) {
            int touched = 0;
            for (uint32_t k = 0; k < patch->field_count; k++) {
                if (strcmp(patch->fields[k].col_name, col_name) == 0) { touched = 1; break; }
            }
            if (!touched) continue;
        }

        KdbFkReferencer refs[KDB_MAX_FK_REFERENCERS];
        uint32_t nrefs = 0;
        KdbStatus st = kdb__find_fk_referencers(db, tbl->name, col_name, refs, KDB_MAX_FK_REFERENCERS, &nrefs);
        if (st != KDB_OK) return st;
        if (nrefs == 0) continue;

        /* The new value a CASCADE update should propagate -- only ever
         * read when patch is non-NULL (an UPDATE) and some referencer
         * actually turns out to be CASCADE, but resolved once per column
         * rather than per (row, referencer) pair. */
        const KdbValue *new_val = NULL;
        if (patch) {
            for (uint32_t k = 0; k < patch->field_count; k++) {
                if (strcmp(patch->fields[k].col_name, col_name) == 0) { new_val = &patch->fields[k].value; break; }
            }
        }

        for (size_t r = 0; r < affected->count; r++) {
            KdbValue pseudo_val;
            const KdbValue *fval = NULL;
            if (kdb__pseudo_column_value(&affected->rows[r], col_name, &pseudo_val)) {
                fval = &pseudo_val;
            } else {
                const KdbRecordField *f = kdb_record_get_field(&affected->rows[r], col_name);
                if (f) fval = &f->value;
            }
            if (!fval || fval->type == KDB_TYPE_NULL) continue;

            for (uint32_t ri = 0; ri < nrefs; ri++) {
                KdbTable *rt = kdb__get_table(db, refs[ri].table);
                if (!rt) continue;
                KdbFkAction action = (KdbFkAction)(patch ? refs[ri].on_update : refs[ri].on_delete);

                if (action == KDB_FK_RESTRICT) {
                    KdbQuery q;
                    kdb_query_init(&q);
                    KdbStatus qst = kdb_query_add_filter_value(&q, refs[ri].col, KDB_OP_EQ, fval, NULL);
                    if (qst != KDB_OK) { kdb_query_free(&q); return qst; }
                    size_t count = 0;
                    qst = kdb_query_count(rt, &q, &count);
                    kdb_query_free(&q);
                    if (qst != KDB_OK) return qst;

                    if (count > 0) {
                        kdb_set_error(KDB_ERR_VALIDATION,
                            "Foreign key violation: '%s.%s' row(s) referenced by '%s.%s' can't be %s.",
                            tbl->name, col_name, refs[ri].table, refs[ri].col, patch ? "changed" : "deleted");
                        return KDB_ERR_VALIDATION;
                    }
                    continue;
                }

                KdbQuery q;
                kdb_query_init(&q);
                KdbStatus qst = kdb_query_add_filter_value(&q, refs[ri].col, KDB_OP_EQ, fval, NULL);
                if (qst != KDB_OK) { kdb_query_free(&q); return qst; }

                if (action == KDB_FK_SET_NULL) {
                    qst = kdb__cascade_set_one_field(db, rt, &q, refs[ri].col, NULL, depth + 1);
                } else { /* KDB_FK_CASCADE */
                    if (patch) {
                        qst = kdb__cascade_set_one_field(db, rt, &q, refs[ri].col, new_val, depth + 1);
                    } else {
                        /* DELETE CASCADE: remove the referencing rows too,
                         * recursing through the same FK machinery (so a
                         * chain across several tables cascades all the
                         * way down) via the typed KdbQuery directly --
                         * no raw-filter-string round trip needed here. */
                        KdbResult casc_affected;
                        qst = kdb_query_execute(rt, &q, &casc_affected);
                        if (qst == KDB_OK) {
                            qst = kdb__enforce_fk_referential_actions(db, rt, &casc_affected, NULL, depth + 1);
                            if (qst == KDB_OK)
                                qst = kdb__enforce_composite_fk_referential_actions(db, rt, &casc_affected, NULL, depth + 1);
                            kdb_result_free(&casc_affected);
                        }
                        if (qst == KDB_OK) {
                            size_t deleted = 0;
                            qst = kdb_table_delete(rt, &q, &deleted);
                        }
                    }
                }
                kdb_query_free(&q);
                if (qst != KDB_OK) return qst;
            }
        }
    }
    return KDB_OK;
}

/* Child-side composite FK enforcement for INSERT: for every composite FK
 * definition on tbl, if r has a non-NULL value for every one of its
 * columns (MATCH SIMPLE -- any NULL/missing component skips the whole
 * check, same as a NULL single-column FK value is never checked), the
 * referenced table must already have a row matching all of them at once. */
static KdbStatus kdb__check_composite_fk_child(KumDB *db, KdbTable *tbl, const KdbRecord *r) {
    for (uint8_t d = 0; d < tbl->header.n_composite_fks; d++) {
        const KdbCompositeFkDef *def = &tbl->header.composite_fks[d];
        KdbValue vals[KDB_MAX_COMPOSITE_COLS];
        int ok = 1;
        for (uint8_t k = 0; k < def->n_cols; k++) {
            const char *col_name = tbl->header.columns[def->col_positions[k]].name;
            const KdbRecordField *f = kdb_record_get_field(r, col_name);
            if (!f || f->value.type == KDB_TYPE_NULL) { ok = 0; break; }
            vals[k] = f->value;
        }
        if (!ok) continue;

        KdbTable *ref_tbl = kdb__get_table(db, def->ref_table);
        if (!ref_tbl) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Composite foreign key on '%s' references table '%s', which doesn't exist (or couldn't be opened).",
                tbl->name, def->ref_table);
            return KDB_ERR_VALIDATION;
        }

        KdbQuery q;
        kdb_query_init(&q);
        KdbStatus st = KDB_OK;
        for (uint8_t k = 0; k < def->n_cols && st == KDB_OK; k++) {
            st = kdb_query_add_filter_value(&q, def->ref_cols[k], KDB_OP_EQ, &vals[k], NULL);
        }
        if (st != KDB_OK) { kdb_query_free(&q); return st; }
        size_t count = 0;
        st = kdb_query_count(ref_tbl, &q, &count);
        kdb_query_free(&q);
        if (st != KDB_OK) return st;

        if (count == 0) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Composite foreign key violation: '%s' has no matching row in '%s'.", tbl->name, def->ref_table);
            return KDB_ERR_VALIDATION;
        }
    }
    return KDB_OK;
}

/* Same idea for UPDATE's would-be result. Unlike the single-column
 * version, a composite key's other components might not be in patch at
 * all (only one column of the key is being changed) -- affected carries
 * each row's pre-patch values so the untouched components can be filled
 * in from there. Only definitions where at least one column is actually
 * in patch are checked (an untouched key can't become invalid). */
static KdbStatus kdb__check_composite_fk_child_update(KumDB *db, KdbTable *tbl, const KdbResult *affected,
                                                       const KdbRecord *patch) {
    for (uint8_t d = 0; d < tbl->header.n_composite_fks; d++) {
        const KdbCompositeFkDef *def = &tbl->header.composite_fks[d];

        int any_touched = 0;
        for (uint8_t k = 0; k < def->n_cols && !any_touched; k++) {
            const char *col_name = tbl->header.columns[def->col_positions[k]].name;
            for (uint32_t f = 0; f < patch->field_count; f++) {
                if (strcmp(patch->fields[f].col_name, col_name) == 0) { any_touched = 1; break; }
            }
        }
        if (!any_touched) continue;

        KdbTable *ref_tbl = kdb__get_table(db, def->ref_table);
        if (!ref_tbl) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Composite foreign key on '%s' references table '%s', which doesn't exist (or couldn't be opened).",
                tbl->name, def->ref_table);
            return KDB_ERR_VALIDATION;
        }

        for (size_t r = 0; r < affected->count; r++) {
            KdbValue vals[KDB_MAX_COMPOSITE_COLS];
            int ok = 1;
            for (uint8_t k = 0; k < def->n_cols; k++) {
                const char *col_name = tbl->header.columns[def->col_positions[k]].name;
                const KdbRecordField *pf = NULL;
                for (uint32_t f = 0; f < patch->field_count; f++) {
                    if (strcmp(patch->fields[f].col_name, col_name) == 0) { pf = &patch->fields[f]; break; }
                }
                const KdbValue *v = pf ? &pf->value : NULL;
                if (!v) {
                    const KdbRecordField *cf = kdb_record_get_field(&affected->rows[r], col_name);
                    if (cf) v = &cf->value;
                }
                if (!v || v->type == KDB_TYPE_NULL) { ok = 0; break; }
                vals[k] = *v;
            }
            if (!ok) continue;

            KdbQuery q;
            kdb_query_init(&q);
            KdbStatus st = KDB_OK;
            for (uint8_t k = 0; k < def->n_cols && st == KDB_OK; k++) {
                st = kdb_query_add_filter_value(&q, def->ref_cols[k], KDB_OP_EQ, &vals[k], NULL);
            }
            if (st != KDB_OK) { kdb_query_free(&q); return st; }
            size_t count = 0;
            st = kdb_query_count(ref_tbl, &q, &count);
            kdb_query_free(&q);
            if (st != KDB_OK) return st;

            if (count == 0) {
                kdb_set_error(KDB_ERR_VALIDATION,
                    "Composite foreign key violation: '%s' has no matching row in '%s'.", tbl->name, def->ref_table);
                return KDB_ERR_VALIDATION;
            }
        }
    }
    return KDB_OK;
}

typedef struct {
    char    table[KDB_MAX_NAME_LEN];
    uint8_t col_positions[KDB_MAX_COMPOSITE_COLS]; /* referencing table's own column positions */
    uint8_t n_cols;
    char    ref_cols[KDB_MAX_COMPOSITE_COLS][KDB_MAX_NAME_LEN]; /* names on tbl (the referenced side), same order */
    uint8_t on_delete;
    uint8_t on_update;
} KdbCompositeFkReferencer;

#define KDB_MAX_COMPOSITE_FK_REFERENCERS 16

/* Every (table, composite FK def) anywhere in the data directory whose
 * ref_table is ref_table_name -- same scanning approach as
 * kdb__find_fk_referencers (every table's own schema, not just already-
 * open ones). */
static KdbStatus kdb__find_composite_fk_referencers(KumDB *db, const char *ref_table_name,
                                                     KdbCompositeFkReferencer *out, uint32_t max_out, uint32_t *count_out) {
    *count_out = 0;
    char names[KDB_MAX_TABLES][KDB_MAX_NAME_LEN];
    uint32_t n = 0;
    KdbStatus st = kdb_storage_list_tables(db->data_dir, names, &n);
    if (st != KDB_OK) return st;

    for (uint32_t i = 0; i < n && *count_out < max_out; i++) {
        KdbTable *t = kdb__get_table(db, names[i]);
        if (!t) continue;
        for (uint8_t d = 0; d < t->header.n_composite_fks && *count_out < max_out; d++) {
            const KdbCompositeFkDef *def = &t->header.composite_fks[d];
            if (strcmp(def->ref_table, ref_table_name) != 0) continue;
            KdbCompositeFkReferencer *o = &out[*count_out];
            memset(o, 0, sizeof(*o));
            snprintf(o->table, sizeof(o->table), "%.127s", names[i]);
            o->n_cols = def->n_cols;
            for (uint8_t k = 0; k < def->n_cols; k++) {
                o->col_positions[k] = def->col_positions[k];
                snprintf(o->ref_cols[k], sizeof(o->ref_cols[k]), "%.127s", def->ref_cols[k]);
            }
            o->on_delete = def->on_delete;
            o->on_update = def->on_update;
            (*count_out)++;
        }
    }
    return KDB_OK;
}

/* Composite counterpart of kdb__cascade_set_one_field -- sets n_cols
 * fields at once (col_names[i] = vals[i], or NULL for SET NULL on every
 * one of them) via a single kdb_table_update patch, so a composite CASCADE
 * propagates all of a key's components together atomically rather than
 * field-by-field. */
static KdbStatus kdb__cascade_set_composite_fields(KumDB *db, KdbTable *tbl, const KdbQuery *q,
                                                   char col_names[][KDB_MAX_NAME_LEN], uint8_t n_cols,
                                                   const KdbValue *vals, int depth) {
    if (depth > KDB_FK_MAX_CASCADE_DEPTH) {
        kdb_set_error(KDB_ERR_VALIDATION, "Foreign key cascade chain too deep (max %d) -- check for a cycle.", KDB_FK_MAX_CASCADE_DEPTH);
        return KDB_ERR_VALIDATION;
    }

    KdbRecordField pf[KDB_MAX_COMPOSITE_COLS];
    memset(pf, 0, sizeof(pf));
    KdbStatus st = KDB_OK;
    uint8_t built = 0;
    for (; built < n_cols; built++) {
        KDB_STRLCPY(pf[built].col_name, col_names[built], KDB_MAX_NAME_LEN);
        st = vals ? kdb_value_copy(&vals[built], &pf[built].value) : kdb_value_from_null(&pf[built].value);
        if (st != KDB_OK) break;
    }
    if (st != KDB_OK) {
        for (uint8_t i = 0; i < built; i++) kdb_value_free(&pf[i].value);
        return st;
    }

    KdbRecord patch;
    memset(&patch, 0, sizeof(patch));
    patch.fields = pf;
    patch.field_count = n_cols;

    KdbResult affected;
    st = kdb_query_execute(tbl, q, &affected);
    if (st != KDB_OK) { for (uint8_t i = 0; i < n_cols; i++) kdb_value_free(&pf[i].value); return st; }
    st = kdb__enforce_fk_referential_actions(db, tbl, &affected, &patch, depth);
    if (st == KDB_OK) st = kdb__enforce_composite_fk_referential_actions(db, tbl, &affected, &patch, depth);
    kdb_result_free(&affected);
    if (st == KDB_OK) {
        size_t updated = 0;
        st = kdb_table_update(tbl, q, &patch, &updated);
    }
    for (uint8_t i = 0; i < n_cols; i++) kdb_value_free(&pf[i].value);
    return st;
}

/* Composite counterpart of kdb__enforce_fk_referential_actions -- same
 * RESTRICT/CASCADE/SET_NULL dispatch, but keyed off a whole composite key
 * at once rather than one column: a referencer is only relevant to an
 * UPDATE if at least one of its ref_cols is actually in patch, and every
 * row's full set of ref_cols values (post-patch, untouched components
 * filled in from affected) must be non-NULL for the check to apply at all
 * (MATCH SIMPLE). Never touches pseudo-columns -- composite FK can't
 * reference id/created_at/updated_at (see KdbCompositeFkDef). */
static KdbStatus kdb__enforce_composite_fk_referential_actions(KumDB *db, KdbTable *tbl, const KdbResult *affected,
                                                                const KdbRecord *patch, int depth) {
    if (depth > KDB_FK_MAX_CASCADE_DEPTH) {
        kdb_set_error(KDB_ERR_VALIDATION, "Foreign key cascade chain too deep (max %d) -- check for a cycle.", KDB_FK_MAX_CASCADE_DEPTH);
        return KDB_ERR_VALIDATION;
    }

    KdbCompositeFkReferencer refs[KDB_MAX_COMPOSITE_FK_REFERENCERS];
    uint32_t nrefs = 0;
    KdbStatus st = kdb__find_composite_fk_referencers(db, tbl->name, refs, KDB_MAX_COMPOSITE_FK_REFERENCERS, &nrefs);
    if (st != KDB_OK) return st;
    if (nrefs == 0) return KDB_OK;

    for (uint32_t ri = 0; ri < nrefs; ri++) {
        if (patch) {
            int any_touched = 0;
            for (uint8_t k = 0; k < refs[ri].n_cols && !any_touched; k++) {
                for (uint32_t f = 0; f < patch->field_count; f++) {
                    if (strcmp(patch->fields[f].col_name, refs[ri].ref_cols[k]) == 0) { any_touched = 1; break; }
                }
            }
            if (!any_touched) continue;
        }

        KdbTable *rt = kdb__get_table(db, refs[ri].table);
        if (!rt) continue;

        char rt_col_names[KDB_MAX_COMPOSITE_COLS][KDB_MAX_NAME_LEN];
        int positions_ok = 1;
        for (uint8_t k = 0; k < refs[ri].n_cols; k++) {
            if (refs[ri].col_positions[k] >= rt->header.column_count) { positions_ok = 0; break; }
            KDB_STRLCPY(rt_col_names[k], rt->header.columns[refs[ri].col_positions[k]].name, KDB_MAX_NAME_LEN);
        }
        if (!positions_ok) continue; /* shouldn't happen -- defensive, schema drift protection */

        KdbFkAction action = (KdbFkAction)(patch ? refs[ri].on_update : refs[ri].on_delete);

        for (size_t r = 0; r < affected->count; r++) {
            KdbValue vals[KDB_MAX_COMPOSITE_COLS];
            int ok = 1;
            for (uint8_t k = 0; k < refs[ri].n_cols; k++) {
                const KdbRecordField *f = kdb_record_get_field(&affected->rows[r], refs[ri].ref_cols[k]);
                if (!f || f->value.type == KDB_TYPE_NULL) { ok = 0; break; }
                vals[k] = f->value;
            }
            if (!ok) continue;

            KdbQuery q;
            kdb_query_init(&q);
            KdbStatus qst = KDB_OK;
            for (uint8_t k = 0; k < refs[ri].n_cols && qst == KDB_OK; k++) {
                qst = kdb_query_add_filter_value(&q, rt_col_names[k], KDB_OP_EQ, &vals[k], NULL);
            }
            if (qst != KDB_OK) { kdb_query_free(&q); return qst; }

            if (action == KDB_FK_RESTRICT) {
                size_t count = 0;
                qst = kdb_query_count(rt, &q, &count);
                kdb_query_free(&q);
                if (qst != KDB_OK) return qst;
                if (count > 0) {
                    kdb_set_error(KDB_ERR_VALIDATION,
                        "Foreign key violation: '%s' row(s) referenced by composite foreign key on '%s' can't be %s.",
                        tbl->name, refs[ri].table, patch ? "changed" : "deleted");
                    return KDB_ERR_VALIDATION;
                }
                continue;
            }

            if (action == KDB_FK_SET_NULL) {
                qst = kdb__cascade_set_composite_fields(db, rt, &q, rt_col_names, refs[ri].n_cols, NULL, depth + 1);
            } else { /* KDB_FK_CASCADE */
                if (patch) {
                    KdbValue new_vals[KDB_MAX_COMPOSITE_COLS];
                    for (uint8_t k = 0; k < refs[ri].n_cols; k++) {
                        const KdbRecordField *pf = NULL;
                        for (uint32_t f = 0; f < patch->field_count; f++) {
                            if (strcmp(patch->fields[f].col_name, refs[ri].ref_cols[k]) == 0) { pf = &patch->fields[f]; break; }
                        }
                        new_vals[k] = pf ? pf->value : vals[k]; /* untouched component keeps its current value */
                    }
                    qst = kdb__cascade_set_composite_fields(db, rt, &q, rt_col_names, refs[ri].n_cols, new_vals, depth + 1);
                } else {
                    KdbResult casc_affected;
                    qst = kdb_query_execute(rt, &q, &casc_affected);
                    if (qst == KDB_OK) {
                        qst = kdb__enforce_fk_referential_actions(db, rt, &casc_affected, NULL, depth + 1);
                        if (qst == KDB_OK)
                            qst = kdb__enforce_composite_fk_referential_actions(db, rt, &casc_affected, NULL, depth + 1);
                        kdb_result_free(&casc_affected);
                    }
                    if (qst == KDB_OK) {
                        size_t deleted = 0;
                        qst = kdb_table_delete(rt, &q, &deleted);
                    }
                }
            }
            kdb_query_free(&q);
            if (qst != KDB_OK) return qst;
        }
    }
    return KDB_OK;
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

    KdbStatus fkst = kdb__check_fk_child(db, tbl, r);
    if (fkst == KDB_OK) fkst = kdb__check_composite_fk_child(db, tbl, r);
    if (fkst != KDB_OK) { kdb_record_free(r); return fkst; }

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

static int kdb__agg_value_to_double(const KdbValue *v, double *out) {
    switch (v->type) {
        case KDB_TYPE_INT:   *out = (double)v->v.as_int; return 1;
        case KDB_TYPE_FLOAT: *out = v->v.as_float;        return 1;
        case KDB_TYPE_BOOL:  *out = (double)v->v.as_bool; return 1;
        default: return 0;
    }
}

/* $match: keep only rows kdb_query_matches accepts, same raw filter
 * strings kdb_find takes. Operates over an already-in-memory KdbResult
 * (not the table itself), so this is a linear scan + kdb_result_append
 * copy per match, not an indexed lookup -- fine as one stage in a
 * pipeline that already fetched the whole table up front. */
static KdbStatus kdb__agg_match(const KdbResult *cur, const char **filters, KdbResult *out) {
    KdbQuery q;
    KdbStatus st = kdb__build_query(filters, &q);
    if (st != KDB_OK) return st;

    st = kdb_result_init(out, cur->count > 0 ? cur->count : 1);
    if (st != KDB_OK) { kdb_query_free(&q); return st; }

    for (size_t i = 0; i < cur->count && st == KDB_OK; i++) {
        if (kdb_query_matches(&q, &cur->rows[i])) st = kdb_result_append(out, &cur->rows[i]);
    }
    kdb_query_free(&q);
    if (st != KDB_OK) kdb_result_free(out);
    return st;
}

typedef struct { const char *field; int ascending; } KdbAggSortKey;
static const KdbAggSortKey *kdb__agg_sort_keys  = NULL;
static size_t               kdb__agg_sort_nkeys = 0;

/* qsort context globals, same single-threaded precedent sql.c's own
 * sort/window comparators already establish (this engine is single-
 * writer, single-process). */
static int kdb__agg_row_cmp(const void *a, const void *b) {
    const KdbRecord *ra = (const KdbRecord *)a;
    const KdbRecord *rb = (const KdbRecord *)b;
    for (size_t i = 0; i < kdb__agg_sort_nkeys; i++) {
        const char *field = kdb__agg_sort_keys[i].field;
        KdbValue pa, pb;
        const KdbValue *fa, *fb;
        if (kdb__pseudo_column_value(ra, field, &pa)) fa = &pa;
        else { const KdbRecordField *f = kdb_record_get_field(ra, field); fa = f ? &f->value : NULL; }
        if (kdb__pseudo_column_value(rb, field, &pb)) fb = &pb;
        else { const KdbRecordField *f = kdb_record_get_field(rb, field); fb = f ? &f->value : NULL; }

        int cmp;
        if (!fa && !fb)   cmp = 0;
        else if (!fa)     cmp = kdb__agg_sort_keys[i].ascending ? -1 : 1;
        else if (!fb)     cmp = kdb__agg_sort_keys[i].ascending ? 1 : -1;
        else { cmp = kdb_value_compare(fa, fb); if (!kdb__agg_sort_keys[i].ascending) cmp = -cmp; }
        if (cmp != 0) return cmp;
    }
    return 0;
}

/* $sort: multi-key, in place (unlike $match/$group/$project, this
 * doesn't need a fresh KdbResult -- qsort reorders cur->rows directly). */
static void kdb__agg_sort(KdbResult *cur, const char *const *fields, const int *ascending, size_t n_fields) {
    if (n_fields == 0 || cur->count == 0) return;
    if (n_fields > KDB_AGG_MAX_SORT_KEYS) n_fields = KDB_AGG_MAX_SORT_KEYS;

    KdbAggSortKey keys[KDB_AGG_MAX_SORT_KEYS];
    for (size_t i = 0; i < n_fields; i++) { keys[i].field = fields[i]; keys[i].ascending = ascending[i]; }
    kdb__agg_sort_keys  = keys;
    kdb__agg_sort_nkeys = n_fields;
    qsort(cur->rows, cur->count, sizeof(KdbRecord), kdb__agg_row_cmp);
    kdb__agg_sort_keys  = NULL;
    kdb__agg_sort_nkeys = 0;
}

#define KDB_AGG_MAX_GROUPS 512

/* key_refs/min_ref/max_ref point straight into cur's own row data -- cur
 * stays alive for this whole stage (same "all must stay alive for the
 * whole call" precedent sql.c's SqlGroupAcc already follows), so no
 * separate copy/free bookkeeping is needed until the final output rows
 * are built. Deliberately real-columns-only (see KdbStage's own doc
 * comment) -- a pseudo-column value has no stable backing storage to
 * point into across the whole pass, unlike a real field already sitting
 * in cur->rows[i].fields[]. */
typedef struct {
    const KdbValue *key_refs[KDB_AGG_MAX_GROUP_KEYS];
    double          sum[KDB_AGG_MAX_ACCUMULATORS];
    int64_t         count_nonnull[KDB_AGG_MAX_ACCUMULATORS];
    const KdbValue *min_ref[KDB_AGG_MAX_ACCUMULATORS];
    const KdbValue *max_ref[KDB_AGG_MAX_ACCUMULATORS];
} KdbAggGroupAcc;

static KdbStatus kdb__agg_group(const KdbResult *cur, const char **group_by,
                                const KdbAccumulator *accs, size_t n_accs, KdbResult *out) {
    size_t n_group_by = 0;
    if (group_by) while (group_by[n_group_by]) n_group_by++;
    if (n_group_by > KDB_AGG_MAX_GROUP_KEYS) {
        kdb_set_error(KDB_ERR_BAD_ARG, "$group supports at most %d group-by fields", KDB_AGG_MAX_GROUP_KEYS);
        return KDB_ERR_BAD_ARG;
    }
    if (n_accs > KDB_AGG_MAX_ACCUMULATORS) {
        kdb_set_error(KDB_ERR_BAD_ARG, "$group supports at most %d accumulators", KDB_AGG_MAX_ACCUMULATORS);
        return KDB_ERR_BAD_ARG;
    }

    KdbAggGroupAcc *groups = (KdbAggGroupAcc *)calloc(KDB_AGG_MAX_GROUPS, sizeof(KdbAggGroupAcc));
    if (!groups) { kdb_err_oom("aggregation groups"); return KDB_ERR_OOM; }
    size_t ngroups = 0;

    for (size_t r = 0; r < cur->count; r++) {
        const KdbRecord *row = &cur->rows[r];
        const KdbValue *key_refs[KDB_AGG_MAX_GROUP_KEYS];
        for (size_t k = 0; k < n_group_by; k++) {
            const KdbRecordField *f = kdb_record_get_field(row, group_by[k]);
            key_refs[k] = f ? &f->value : NULL;
        }

        size_t gi;
        if (n_group_by == 0) {
            gi = 0;
            if (ngroups == 0) ngroups = 1;
        } else {
            int found = 0;
            for (gi = 0; gi < ngroups; gi++) {
                int all_match = 1;
                for (size_t k = 0; k < n_group_by; k++) {
                    const KdbValue *a = groups[gi].key_refs[k], *b = key_refs[k];
                    int eq = (!a && !b) || (a && b && kdb_value_compare(a, b) == 0);
                    if (!eq) { all_match = 0; break; }
                }
                if (all_match) { found = 1; break; }
            }
            if (!found) {
                if (ngroups >= KDB_AGG_MAX_GROUPS) {
                    free(groups);
                    kdb_set_error(KDB_ERR_FULL, "$group: too many distinct groups (max %d)", KDB_AGG_MAX_GROUPS);
                    return KDB_ERR_FULL;
                }
                gi = ngroups++;
                for (size_t k = 0; k < n_group_by; k++) groups[gi].key_refs[k] = key_refs[k];
            }
        }

        for (size_t a = 0; a < n_accs; a++) {
            const KdbAccumulator *acc = &accs[a];
            if (acc->type == KDB_ACC_COUNT) {
                if (!acc->source_field) { groups[gi].count_nonnull[a]++; continue; }
                const KdbRecordField *f = kdb_record_get_field(row, acc->source_field);
                if (f && f->value.type != KDB_TYPE_NULL) groups[gi].count_nonnull[a]++;
                continue;
            }
            const KdbRecordField *f = kdb_record_get_field(row, acc->source_field);
            if (!f || f->value.type == KDB_TYPE_NULL) continue;
            switch (acc->type) {
                case KDB_ACC_SUM:
                case KDB_ACC_AVG: {
                    double v;
                    if (kdb__agg_value_to_double(&f->value, &v)) { groups[gi].sum[a] += v; groups[gi].count_nonnull[a]++; }
                    break;
                }
                case KDB_ACC_MIN:
                    if (!groups[gi].min_ref[a] || kdb_value_compare(&f->value, groups[gi].min_ref[a]) < 0) groups[gi].min_ref[a] = &f->value;
                    break;
                case KDB_ACC_MAX:
                    if (!groups[gi].max_ref[a] || kdb_value_compare(&f->value, groups[gi].max_ref[a]) > 0) groups[gi].max_ref[a] = &f->value;
                    break;
                default: break;
            }
        }
    }
    if (n_group_by == 0 && ngroups == 0) ngroups = 1; /* no rows: still emit one summary group, same as SQL's GROUP BY-less aggregate */

    KdbStatus st = kdb_result_init(out, ngroups);
    if (st != KDB_OK) { free(groups); return st; }

    for (size_t gi = 0; gi < ngroups && st == KDB_OK; gi++) {
        KdbRecord orow;
        memset(&orow, 0, sizeof(orow));
        uint32_t nfields = (uint32_t)(n_group_by + n_accs);
        orow.fields = (KdbRecordField *)calloc(nfields ? nfields : 1, sizeof(KdbRecordField));
        if (!orow.fields) { st = KDB_ERR_OOM; kdb_err_oom("group output fields"); break; }

        uint32_t fi = 0;
        int ok = 1;
        for (size_t k = 0; k < n_group_by && ok; k++) {
            KDB_STRLCPY(orow.fields[fi].col_name, group_by[k], KDB_MAX_NAME_LEN);
            if (groups[gi].key_refs[k]) { if (kdb_value_copy(groups[gi].key_refs[k], &orow.fields[fi].value) != KDB_OK) ok = 0; }
            else orow.fields[fi].value.type = KDB_TYPE_NULL;
            fi++;
        }
        for (size_t a = 0; a < n_accs && ok; a++) {
            KDB_STRLCPY(orow.fields[fi].col_name, accs[a].output_name, KDB_MAX_NAME_LEN);
            KdbValue *ov = &orow.fields[fi].value;
            switch (accs[a].type) {
                case KDB_ACC_COUNT: ov->type = KDB_TYPE_INT;   ov->v.as_int   = groups[gi].count_nonnull[a]; break;
                case KDB_ACC_SUM:   ov->type = KDB_TYPE_FLOAT; ov->v.as_float = groups[gi].sum[a];           break;
                case KDB_ACC_AVG:
                    ov->type = KDB_TYPE_FLOAT;
                    ov->v.as_float = groups[gi].count_nonnull[a] > 0 ? groups[gi].sum[a] / (double)groups[gi].count_nonnull[a] : 0.0;
                    break;
                case KDB_ACC_MIN:
                    if (groups[gi].min_ref[a]) { if (kdb_value_copy(groups[gi].min_ref[a], ov) != KDB_OK) ok = 0; }
                    else ov->type = KDB_TYPE_NULL;
                    break;
                case KDB_ACC_MAX:
                    if (groups[gi].max_ref[a]) { if (kdb_value_copy(groups[gi].max_ref[a], ov) != KDB_OK) ok = 0; }
                    else ov->type = KDB_TYPE_NULL;
                    break;
            }
            fi++;
        }
        orow.field_count = fi;
        st = ok ? kdb_result_append(out, &orow) : KDB_ERR_OOM;
        for (uint32_t k = 0; k < orow.field_count; k++) kdb_value_free(&orow.fields[k].value);
        free(orow.fields);
    }

    free(groups);
    if (st != KDB_OK) kdb_result_free(out);
    return st;
}

/* $project: keep only the named fields, in that order -- id/created_at/
 * updated_at work here (unlike $group's keys/$sort's fields) since the
 * pseudo-value is copied straight into the output row instead of being
 * held onto across a whole stage. A missing field is silently skipped,
 * same "soft" convention SELECT-item projection elsewhere in this engine
 * already follows. */
static KdbStatus kdb__agg_project(const KdbResult *cur, const char **fields, KdbResult *out) {
    size_t n_fields = 0;
    if (fields) while (fields[n_fields]) n_fields++;

    KdbStatus st = kdb_result_init(out, cur->count > 0 ? cur->count : 1);
    if (st != KDB_OK) return st;

    for (size_t r = 0; r < cur->count && st == KDB_OK; r++) {
        KdbRecord orow;
        memset(&orow, 0, sizeof(orow));
        orow.id         = cur->rows[r].id;
        orow.created_at = cur->rows[r].created_at;
        orow.updated_at = cur->rows[r].updated_at;
        orow.fields = (KdbRecordField *)calloc(n_fields ? n_fields : 1, sizeof(KdbRecordField));
        if (!orow.fields) { st = KDB_ERR_OOM; kdb_err_oom("project output fields"); break; }

        uint32_t fi = 0;
        int ok = 1;
        for (size_t k = 0; k < n_fields && ok; k++) {
            KdbValue pseudo;
            const KdbValue *val = NULL;
            if (kdb__pseudo_column_value(&cur->rows[r], fields[k], &pseudo)) val = &pseudo;
            else { const KdbRecordField *f = kdb_record_get_field(&cur->rows[r], fields[k]); if (f) val = &f->value; }
            if (!val) continue;

            KDB_STRLCPY(orow.fields[fi].col_name, fields[k], KDB_MAX_NAME_LEN);
            if (kdb_value_copy(val, &orow.fields[fi].value) != KDB_OK) { ok = 0; break; }
            fi++;
        }
        orow.field_count = fi;
        st = ok ? kdb_result_append(out, &orow) : KDB_ERR_OOM;
        for (uint32_t k = 0; k < orow.field_count; k++) kdb_value_free(&orow.fields[k].value);
        free(orow.fields);
    }

    if (st != KDB_OK) kdb_result_free(out);
    return st;
}

KdbStatus kdb_aggregate(KumDB *db, const char *table_name,
                        const KdbStage *stages, size_t n_stages, KdbRows **rows_out) {
    if (!db || !table_name || !rows_out || (n_stages > 0 && !stages)) {
        kdb_err_null_arg("db/table_name/stages/rows_out", "kdb_aggregate");
        return KDB_ERR_BAD_ARG;
    }
    *rows_out = NULL;

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbQuery empty_q;
    kdb_query_init(&empty_q);
    KdbResult cur;
    KdbStatus st = kdb_query_execute(tbl, &empty_q, &cur);
    kdb_query_free(&empty_q);
    if (st != KDB_OK) return st;

    for (size_t i = 0; i < n_stages && st == KDB_OK; i++) {
        const KdbStage *stage = &stages[i];
        switch (stage->type) {
            case KDB_STAGE_MATCH: {
                KdbResult next;
                st = kdb__agg_match(&cur, stage->as.match_filters, &next);
                if (st == KDB_OK) { kdb_result_free(&cur); cur = next; }
                break;
            }
            case KDB_STAGE_GROUP: {
                KdbResult next;
                st = kdb__agg_group(&cur, stage->as.group.group_by,
                                    stage->as.group.accumulators, stage->as.group.n_accumulators, &next);
                if (st == KDB_OK) { kdb_result_free(&cur); cur = next; }
                break;
            }
            case KDB_STAGE_SORT:
                kdb__agg_sort(&cur, stage->as.sort.fields, stage->as.sort.ascending, stage->as.sort.n_fields);
                break;
            case KDB_STAGE_LIMIT:
                kdb_result_limit(&cur, stage->as.limit);
                break;
            case KDB_STAGE_SKIP:
                kdb_result_offset(&cur, stage->as.skip);
                break;
            case KDB_STAGE_PROJECT: {
                KdbResult next;
                st = kdb__agg_project(&cur, stage->as.project_fields, &next);
                if (st == KDB_OK) { kdb_result_free(&cur); cur = next; }
                break;
            }
        }
    }
    if (st != KDB_OK) { kdb_result_free(&cur); return st; }

    *rows_out = kdb__result_to_rows(&cur);
    return *rows_out ? KDB_OK : kdb_last_status();
}

#define KDB_TEXT_MAX_TERMS    16
#define KDB_TEXT_MAX_TERM_LEN 64

/* Splits text into lowercased alphanumeric words, writing up to
 * max_terms of them into out (each NUL-terminated, truncated to
 * KDB_TEXT_MAX_TERM_LEN-1 if longer) -- shared by query tokenizing and
 * (via kdb__text_count_field) row-field tokenizing, so both sides
 * normalize identically. */
static size_t kdb__text_tokenize(const char *text, char out[][KDB_TEXT_MAX_TERM_LEN], size_t max_terms) {
    size_t n = 0;
    size_t i = 0;
    size_t len = strlen(text);
    while (i < len && n < max_terms) {
        while (i < len && !isalnum((unsigned char)text[i])) i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && isalnum((unsigned char)text[i])) i++;
        size_t wlen = i - start;
        if (wlen >= KDB_TEXT_MAX_TERM_LEN) wlen = KDB_TEXT_MAX_TERM_LEN - 1;
        for (size_t k = 0; k < wlen; k++) out[n][k] = (char)tolower((unsigned char)text[start + k]);
        out[n][wlen] = '\0';
        n++;
    }
    return n;
}

/* Tokenizes text the same way kdb__text_tokenize does, but instead of
 * collecting words just tallies, for each of terms[0..n_terms), how many
 * times it occurs as a whole word in text -- term-frequency scoring
 * against one field's value, added into hits[] (already tallying other
 * fields/rows, not reset here). */
static void kdb__text_count_field(const char *text, char terms[][KDB_TEXT_MAX_TERM_LEN], size_t n_terms, int *hits) {
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        while (i < len && !isalnum((unsigned char)text[i])) i++;
        if (i >= len) break;
        size_t start = i;
        while (i < len && isalnum((unsigned char)text[i])) i++;
        size_t wlen = i - start;
        char word[KDB_TEXT_MAX_TERM_LEN];
        if (wlen >= KDB_TEXT_MAX_TERM_LEN) wlen = KDB_TEXT_MAX_TERM_LEN - 1;
        for (size_t k = 0; k < wlen; k++) word[k] = (char)tolower((unsigned char)text[start + k]);
        word[wlen] = '\0';
        for (size_t t = 0; t < n_terms; t++) if (strcmp(word, terms[t]) == 0) hits[t]++;
    }
}

/* Deep-copies src into filtered with one extra FLOAT field appended
 * (col_name=extra_name, value=extra_value) -- shared by kdb_text_search's
 * "_score" and kdb_geo_near's "_distance_km", the two places a computed
 * per-row ranking value needs to ride along with the row's own fields. */
static KdbStatus kdb__append_row_with_extra_float(KdbResult *filtered, const KdbRecord *src,
                                                  const char *extra_name, double extra_value) {
    uint32_t nf_count = src->field_count + 1;
    KdbRecordField *nf = (KdbRecordField *)calloc(nf_count, sizeof(KdbRecordField));
    if (!nf) { kdb_err_oom("row-with-extra-field output"); return KDB_ERR_OOM; }

    uint32_t copied = 0;
    int ok = 1;
    for (uint32_t k = 0; k < src->field_count && ok; k++) {
        KDB_STRLCPY(nf[k].col_name, src->fields[k].col_name, KDB_MAX_NAME_LEN);
        if (kdb_value_copy(&src->fields[k].value, &nf[k].value) != KDB_OK) { ok = 0; break; }
        copied++;
    }

    KdbStatus st;
    if (ok) {
        KDB_STRLCPY(nf[src->field_count].col_name, extra_name, KDB_MAX_NAME_LEN);
        kdb_value_from_float(extra_value, &nf[src->field_count].value);
        copied++;

        KdbRecord orow;
        memset(&orow, 0, sizeof(orow));
        orow.id          = src->id;
        orow.created_at  = src->created_at;
        orow.updated_at  = src->updated_at;
        orow.fields      = nf;
        orow.field_count = nf_count;
        st = kdb_result_append(filtered, &orow);
    } else {
        st = KDB_ERR_OOM;
    }
    for (uint32_t k = 0; k < copied; k++) kdb_value_free(&nf[k].value);
    free(nf);
    return st;
}

typedef struct { size_t idx; double score; } KdbTextHit;

/* qsort context, same single-threaded precedent every other sort
 * comparator in this codebase already follows. Sorted by score
 * descending, ties broken by id ascending for a deterministic order
 * (qsort itself isn't guaranteed stable). */
static const KdbResult *kdb__text_ctx_all = NULL;
static int kdb__text_hit_cmp(const void *a, const void *b) {
    const KdbTextHit *ha = (const KdbTextHit *)a;
    const KdbTextHit *hb = (const KdbTextHit *)b;
    if (ha->score != hb->score) return ha->score > hb->score ? -1 : 1;
    uint64_t ida = kdb__text_ctx_all->rows[ha->idx].id;
    uint64_t idb = kdb__text_ctx_all->rows[hb->idx].id;
    if (ida != idb) return ida < idb ? -1 : 1;
    return 0;
}

KdbStatus kdb_text_search(KumDB *db, const char *table_name, const char *query,
                          const KdbTextSearchOpts *opts, KdbRows **rows_out) {
    if (!db || !table_name || !query || !rows_out) {
        kdb_err_null_arg("db/table_name/query/rows_out", "kdb_text_search");
        return KDB_ERR_BAD_ARG;
    }
    *rows_out = NULL;

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbTextMatchMode mode = opts ? opts->mode : KDB_TEXT_MATCH_ALL;
    size_t limit = opts ? opts->limit : 0;

    char search_fields[KDB_MAX_COLUMNS][KDB_MAX_NAME_LEN];
    size_t n_search_fields = 0;
    if (opts && opts->fields) {
        for (size_t i = 0; opts->fields[i] && n_search_fields < KDB_MAX_COLUMNS; i++)
            snprintf(search_fields[n_search_fields++], KDB_MAX_NAME_LEN, "%.127s", opts->fields[i]);
    } else {
        for (uint32_t i = 0; i < tbl->header.column_count && n_search_fields < KDB_MAX_COLUMNS; i++) {
            if (tbl->header.columns[i].type == KDB_TYPE_STRING)
                snprintf(search_fields[n_search_fields++], KDB_MAX_NAME_LEN, "%.127s", tbl->header.columns[i].name);
        }
    }
    if (n_search_fields == 0) {
        kdb_set_error(KDB_ERR_BAD_ARG,
            "kdb_text_search: table '%s' has no STRING field to search -- name them explicitly via opts->fields if it does",
            table_name);
        return KDB_ERR_BAD_ARG;
    }

    char terms[KDB_TEXT_MAX_TERMS][KDB_TEXT_MAX_TERM_LEN];
    size_t n_terms = kdb__text_tokenize(query, terms, KDB_TEXT_MAX_TERMS);
    if (n_terms == 0) {
        /* an empty/punctuation-only query matches nothing */
        *rows_out = (KdbRows *)calloc(1, sizeof(KdbRows));
        if (!*rows_out) { kdb_err_oom("KdbRows"); return KDB_ERR_OOM; }
        return KDB_OK;
    }

    KdbQuery empty_q;
    kdb_query_init(&empty_q);
    KdbResult all;
    KdbStatus st = kdb_query_execute(tbl, &empty_q, &all);
    kdb_query_free(&empty_q);
    if (st != KDB_OK) return st;

    KdbTextHit *hits = (KdbTextHit *)calloc(all.count > 0 ? all.count : 1, sizeof(KdbTextHit));
    if (!hits) { kdb_result_free(&all); kdb_err_oom("text search hits"); return KDB_ERR_OOM; }
    size_t n_hits = 0;

    for (size_t r = 0; r < all.count; r++) {
        int term_hits[KDB_TEXT_MAX_TERMS];
        memset(term_hits, 0, sizeof(term_hits));
        for (size_t f = 0; f < n_search_fields; f++) {
            const KdbRecordField *field = kdb_record_get_field(&all.rows[r], search_fields[f]);
            if (!field || field->value.type != KDB_TYPE_STRING || !field->value.v.as_string.data) continue;
            kdb__text_count_field(field->value.v.as_string.data, terms, n_terms, term_hits);
        }
        int n_matched_terms = 0;
        double score = 0.0;
        for (size_t t = 0; t < n_terms; t++) {
            if (term_hits[t] > 0) { n_matched_terms++; score += term_hits[t]; }
        }
        int included = (mode == KDB_TEXT_MATCH_ALL) ? (n_matched_terms == (int)n_terms) : (n_matched_terms > 0);
        if (included) { hits[n_hits].idx = r; hits[n_hits].score = score; n_hits++; }
    }

    kdb__text_ctx_all = &all;
    qsort(hits, n_hits, sizeof(KdbTextHit), kdb__text_hit_cmp);
    kdb__text_ctx_all = NULL;

    if (limit > 0 && n_hits > limit) n_hits = limit;

    KdbResult filtered;
    st = kdb_result_init(&filtered, n_hits > 0 ? n_hits : 1);
    if (st != KDB_OK) { free(hits); kdb_result_free(&all); return st; }

    for (size_t i = 0; i < n_hits && st == KDB_OK; i++) {
        st = kdb__append_row_with_extra_float(&filtered, &all.rows[hits[i].idx], "_score", hits[i].score);
    }

    free(hits);
    kdb_result_free(&all);
    if (st != KDB_OK) { kdb_result_free(&filtered); return st; }

    *rows_out = kdb__result_to_rows(&filtered);
    return *rows_out ? KDB_OK : kdb_last_status();
}

#define KDB_GEO_EARTH_RADIUS_KM 6371.0088
#define KDB_GEO_DEG_TO_RAD (3.14159265358979323846 / 180.0)

/* Great-circle (Haversine) distance in kilometers -- the standard
 * spherical-earth formula for real-world GPS coordinates; a flat-plane
 * (Pythagorean) approximation on raw lat/lon degrees breaks down badly
 * once two points are more than a few km apart, since a degree of
 * longitude covers less real distance the further from the equator you
 * get. */
double kdb_geo_distance_km(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * KDB_GEO_DEG_TO_RAD;
    double dlon = (lon2 - lon1) * KDB_GEO_DEG_TO_RAD;
    double a = sin(dlat / 2.0) * sin(dlat / 2.0) +
               cos(lat1 * KDB_GEO_DEG_TO_RAD) * cos(lat2 * KDB_GEO_DEG_TO_RAD) * sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return KDB_GEO_EARTH_RADIUS_KM * c;
}

typedef struct { size_t idx; double distance_km; } KdbGeoHit;

static const KdbResult *kdb__geo_ctx_all = NULL;
static int kdb__geo_hit_cmp(const void *a, const void *b) {
    const KdbGeoHit *ha = (const KdbGeoHit *)a;
    const KdbGeoHit *hb = (const KdbGeoHit *)b;
    if (ha->distance_km != hb->distance_km) return ha->distance_km < hb->distance_km ? -1 : 1;
    uint64_t ida = kdb__geo_ctx_all->rows[ha->idx].id;
    uint64_t idb = kdb__geo_ctx_all->rows[hb->idx].id;
    if (ida != idb) return ida < idb ? -1 : 1;
    return 0;
}

KdbStatus kdb_geo_near(KumDB *db, const char *table_name, const KdbGeoNearOpts *opts, KdbRows **rows_out) {
    if (!db || !table_name || !opts || !opts->lat_field || !opts->lon_field || !rows_out) {
        kdb_err_null_arg("db/table_name/opts/rows_out", "kdb_geo_near");
        return KDB_ERR_BAD_ARG;
    }
    *rows_out = NULL;
    if (opts->center_lat < -90.0 || opts->center_lat > 90.0 || opts->center_lon < -180.0 || opts->center_lon > 180.0) {
        kdb_set_error(KDB_ERR_BAD_ARG, "kdb_geo_near: center_lat must be -90..90 and center_lon -180..180");
        return KDB_ERR_BAD_ARG;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbQuery empty_q;
    kdb_query_init(&empty_q);
    KdbResult all;
    KdbStatus st = kdb_query_execute(tbl, &empty_q, &all);
    kdb_query_free(&empty_q);
    if (st != KDB_OK) return st;

    KdbGeoHit *hits = (KdbGeoHit *)calloc(all.count > 0 ? all.count : 1, sizeof(KdbGeoHit));
    if (!hits) { kdb_result_free(&all); kdb_err_oom("geo near hits"); return KDB_ERR_OOM; }
    size_t n_hits = 0;

    for (size_t r = 0; r < all.count; r++) {
        const KdbRecordField *latf = kdb_record_get_field(&all.rows[r], opts->lat_field);
        const KdbRecordField *lonf = kdb_record_get_field(&all.rows[r], opts->lon_field);
        double lat, lon;
        if (!latf || !lonf) continue;
        if (!kdb__agg_value_to_double(&latf->value, &lat)) continue;
        if (!kdb__agg_value_to_double(&lonf->value, &lon)) continue;

        double d = kdb_geo_distance_km(opts->center_lat, opts->center_lon, lat, lon);
        if (opts->max_distance_km > 0.0 && d > opts->max_distance_km) continue;
        hits[n_hits].idx = r;
        hits[n_hits].distance_km = d;
        n_hits++;
    }

    kdb__geo_ctx_all = &all;
    qsort(hits, n_hits, sizeof(KdbGeoHit), kdb__geo_hit_cmp);
    kdb__geo_ctx_all = NULL;

    if (opts->limit > 0 && n_hits > opts->limit) n_hits = opts->limit;

    KdbResult filtered;
    st = kdb_result_init(&filtered, n_hits > 0 ? n_hits : 1);
    if (st != KDB_OK) { free(hits); kdb_result_free(&all); return st; }

    for (size_t i = 0; i < n_hits && st == KDB_OK; i++) {
        st = kdb__append_row_with_extra_float(&filtered, &all.rows[hits[i].idx], "_distance_km", hits[i].distance_km);
    }

    free(hits);
    kdb_result_free(&all);
    if (st != KDB_OK) { kdb_result_free(&filtered); return st; }

    *rows_out = kdb__result_to_rows(&filtered);
    return *rows_out ? KDB_OK : kdb_last_status();
}

KdbStatus kdb_geo_within_box(KumDB *db, const char *table_name, const KdbGeoBoxOpts *opts, KdbRows **rows_out) {
    if (!db || !table_name || !opts || !opts->lat_field || !opts->lon_field || !rows_out) {
        kdb_err_null_arg("db/table_name/opts/rows_out", "kdb_geo_within_box");
        return KDB_ERR_BAD_ARG;
    }
    *rows_out = NULL;
    if (opts->min_lat > opts->max_lat || opts->min_lon > opts->max_lon ||
        opts->min_lat < -90.0 || opts->max_lat > 90.0 || opts->min_lon < -180.0 || opts->max_lon > 180.0) {
        kdb_set_error(KDB_ERR_BAD_ARG,
            "kdb_geo_within_box: min_lat/max_lat must be -90..90 (min<=max) and min_lon/max_lon -180..180 (min<=max)");
        return KDB_ERR_BAD_ARG;
    }

    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbQuery empty_q;
    kdb_query_init(&empty_q);
    KdbResult all;
    KdbStatus st = kdb_query_execute(tbl, &empty_q, &all);
    kdb_query_free(&empty_q);
    if (st != KDB_OK) return st;

    KdbResult filtered;
    st = kdb_result_init(&filtered, all.count > 0 ? all.count : 1);
    if (st != KDB_OK) { kdb_result_free(&all); return st; }

    for (size_t r = 0; r < all.count && st == KDB_OK; r++) {
        const KdbRecordField *latf = kdb_record_get_field(&all.rows[r], opts->lat_field);
        const KdbRecordField *lonf = kdb_record_get_field(&all.rows[r], opts->lon_field);
        double lat, lon;
        if (!latf || !lonf) continue;
        if (!kdb__agg_value_to_double(&latf->value, &lat)) continue;
        if (!kdb__agg_value_to_double(&lonf->value, &lon)) continue;
        if (lat < opts->min_lat || lat > opts->max_lat || lon < opts->min_lon || lon > opts->max_lon) continue;
        st = kdb_result_append(&filtered, &all.rows[r]);
    }

    kdb_result_free(&all);
    if (st != KDB_OK) { kdb_result_free(&filtered); return st; }

    *rows_out = kdb__result_to_rows(&filtered);
    return *rows_out ? KDB_OK : kdb_last_status();
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

    st = kdb__check_fk_child_update(db, tbl, patch);
    if (st != KDB_OK) { kdb_record_free(patch); kdb_query_free(&q); return st; }

    /* Referential actions (RESTRICT/CASCADE/SET NULL), plus composite FK
     * child-side validation: fetch exactly the rows this UPDATE matches
     * before anything is mutated, so both can see each row's current
     * (pre-patch) values -- kdb__enforce_fk_referential_actions for any
     * column another table's FK depends on, kdb__check_composite_fk_child_
     * update for a composite key's components patch doesn't touch. */
    KdbResult affected;
    st = kdb_query_execute(tbl, &q, &affected);
    if (st != KDB_OK) { kdb_record_free(patch); kdb_query_free(&q); return st; }
    st = kdb__check_composite_fk_child_update(db, tbl, &affected, patch);
    if (st == KDB_OK) st = kdb__enforce_fk_referential_actions(db, tbl, &affected, patch, 0);
    if (st == KDB_OK) st = kdb__enforce_composite_fk_referential_actions(db, tbl, &affected, patch, 0);
    kdb_result_free(&affected);
    if (st != KDB_OK) { kdb_record_free(patch); kdb_query_free(&q); return st; }

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

    /* Referential actions: same idea as kdb_update -- see which rows this
     * DELETE would actually remove before removing them. */
    KdbResult affected;
    st = kdb_query_execute(tbl, &q, &affected);
    if (st != KDB_OK) { kdb_query_free(&q); return st; }
    st = kdb__enforce_fk_referential_actions(db, tbl, &affected, NULL, 0);
    if (st == KDB_OK) st = kdb__enforce_composite_fk_referential_actions(db, tbl, &affected, NULL, 0);
    kdb_result_free(&affected);
    if (st != KDB_OK) { kdb_query_free(&q); return st; }

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
        cols[i].unique   = columns[i].unique   ? 1 : 0;
        if (cols[i].unique) cols[i].indexed = 1; /* a unique column always gets a real index to check against */
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
        out->unique   = c->unique   ? 1 : 0;
        (*count_out)++;
    }
    return KDB_OK;
}

KdbStatus kdb_add_column(KumDB *db, const char *table_name, const char *col_name,
                         KdbFieldType type, int nullable, int indexed, int unique) {
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
    return kdb_table_add_column(tbl, col_name, (KdbType)type, nullable ? 1 : 0, indexed ? 1 : 0, unique ? 1 : 0);
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

KdbStatus kdb_create_index(KumDB *db, const char *table_name, const char *col_name) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_create_index");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_create_index(tbl, col_name);
}

KdbStatus kdb_drop_index(KumDB *db, const char *table_name, const char *col_name) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_drop_index");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_drop_index(tbl, col_name);
}

KdbStatus kdb_create_composite_index(KumDB *db, const char *table_name, const char **col_names, uint32_t n_cols) {
    if (!db || !table_name || !col_names) {
        kdb_err_null_arg("db/table_name/col_names", "kdb_create_composite_index");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_create_composite_index(tbl, col_names, n_cols);
}

KdbStatus kdb_drop_composite_index(KumDB *db, const char *table_name, const char **col_names, uint32_t n_cols) {
    if (!db || !table_name || !col_names) {
        kdb_err_null_arg("db/table_name/col_names", "kdb_drop_composite_index");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_drop_composite_index(tbl, col_names, n_cols);
}

KdbStatus kdb_rename_column(KumDB *db, const char *table_name, const char *old_col, const char *new_col) {
    if (!db || !table_name || !old_col || !new_col) {
        kdb_err_null_arg("db/table_name/old_col/new_col", "kdb_rename_column");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_rename_column(tbl, old_col, new_col);
}

KdbStatus kdb_alter_column_nullable(KumDB *db, const char *table_name, const char *col_name, int nullable) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_alter_column_nullable");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_set_nullable(tbl, col_name, nullable);
}

KdbStatus kdb_alter_column_unique(KumDB *db, const char *table_name, const char *col_name, int unique) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_alter_column_unique");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_set_unique(tbl, col_name, unique);
}

KdbStatus kdb_alter_column_type(KumDB *db, const char *table_name, const char *col_name, KdbFieldType new_type) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_alter_column_type");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_alter_column_type(tbl, col_name, new_type);
}

KdbStatus kdb_add_foreign_key(KumDB *db, const char *table_name, const char *col_name,
                              const char *ref_table, const char *ref_col,
                              KdbFkAction on_delete, KdbFkAction on_update) {
    if (!db || !table_name || !col_name || !ref_table || !ref_col) {
        kdb_err_null_arg("db/table_name/col_name/ref_table/ref_col", "kdb_add_foreign_key");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    /* Validated here, not in kdb_table_add_foreign_key -- a bare KdbTable*
     * handle has no way to look up another table by name, only KumDB does. */
    KdbTable *ref_tbl = kdb__get_table(db, ref_table);
    if (!ref_tbl) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "Foreign key on '%s.%s' references table '%s', which doesn't exist.",
                      table_name, col_name, ref_table);
        return KDB_ERR_NOT_FOUND;
    }
    /* id/created_at/updated_at are always-valid reference targets even
     * though they're not "real" columns (kdb_table_has_column is false
     * for them) -- the most natural thing to REFERENCES is a table's own
     * auto id. */
    int ref_col_ok = kdb_table_has_column(ref_tbl, ref_col) ||
                     strcmp(ref_col, "id") == 0 || strcmp(ref_col, "created_at") == 0 ||
                     strcmp(ref_col, "updated_at") == 0;
    if (!ref_col_ok) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "Foreign key on '%s.%s' references column '%s.%s', which doesn't exist.",
                      table_name, col_name, ref_table, ref_col);
        return KDB_ERR_NOT_FOUND;
    }
    return kdb_table_add_foreign_key(tbl, col_name, ref_table, ref_col, on_delete, on_update);
}

KdbStatus kdb_drop_foreign_key(KumDB *db, const char *table_name, const char *col_name) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_drop_foreign_key");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_drop_foreign_key(tbl, col_name);
}

KdbStatus kdb_add_composite_foreign_key(KumDB *db, const char *table_name,
                                        const char **col_names, uint32_t n_cols,
                                        const char *ref_table, const char **ref_cols, uint32_t n_ref_cols,
                                        KdbFkAction on_delete, KdbFkAction on_update) {
    if (!db || !table_name || !col_names || !ref_table || !ref_cols) {
        kdb_err_null_arg("db/table_name/col_names/ref_table/ref_cols", "kdb_add_composite_foreign_key");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    /* Validated here, not in kdb_table_add_composite_foreign_key -- a bare
     * KdbTable* handle has no way to look up another table by name. Unlike
     * single-column FK, ref_cols must all be real columns on ref_table --
     * a composite key referencing id/created_at/updated_at as one of its
     * components doesn't make sense (see KdbCompositeFkDef). */
    KdbTable *ref_tbl = kdb__get_table(db, ref_table);
    if (!ref_tbl) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "Composite foreign key on '%s' references table '%s', which doesn't exist.",
                      table_name, ref_table);
        return KDB_ERR_NOT_FOUND;
    }
    for (uint32_t i = 0; i < n_ref_cols; i++) {
        if (!kdb_table_has_column(ref_tbl, ref_cols[i])) {
            kdb_set_error(KDB_ERR_NOT_FOUND, "Composite foreign key on '%s' references column '%s.%s', which doesn't exist.",
                          table_name, ref_table, ref_cols[i]);
            return KDB_ERR_NOT_FOUND;
        }
    }
    return kdb_table_add_composite_foreign_key(tbl, col_names, n_cols, ref_table, ref_cols, n_ref_cols, on_delete, on_update);
}

KdbStatus kdb_drop_composite_foreign_key(KumDB *db, const char *table_name, const char **col_names, uint32_t n_cols) {
    if (!db || !table_name || !col_names) {
        kdb_err_null_arg("db/table_name/col_names", "kdb_drop_composite_foreign_key");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_drop_composite_foreign_key(tbl, col_names, n_cols);
}

KdbStatus kdb_add_check_constraint(KumDB *db, const char *table_name, const char *col_name,
                                   KdbOperator op, const KdbField *literal) {
    if (!db || !table_name || !col_name || !literal) {
        kdb_err_null_arg("db/table_name/col_name/literal", "kdb_add_check_constraint");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbValue lit;
    if (kdb__field_to_value(literal, &lit) != KDB_OK) return kdb_last_status();
    KdbStatus st = kdb_table_add_check(tbl, col_name, op, &lit);
    kdb_value_free(&lit);
    return st;
}

KdbStatus kdb_set_column_default(KumDB *db, const char *table_name, const char *col_name, const KdbField *default_val) {
    if (!db || !table_name || !col_name || !default_val) {
        kdb_err_null_arg("db/table_name/col_name/default_val", "kdb_set_column_default");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();

    KdbValue val;
    if (kdb__field_to_value(default_val, &val) != KDB_OK) return kdb_last_status();
    KdbStatus st = kdb_table_set_default(tbl, col_name, &val);
    kdb_value_free(&val);
    return st;
}

KdbStatus kdb_drop_column_default(KumDB *db, const char *table_name, const char *col_name) {
    if (!db || !table_name || !col_name) {
        kdb_err_null_arg("db/table_name/col_name", "kdb_drop_column_default");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(table_name);
        return KDB_ERR_READ_ONLY;
    }
    KdbTable *tbl = kdb__get_table(db, table_name);
    if (!tbl) return kdb_last_status();
    return kdb_table_drop_default(tbl, col_name);
}

/* Renames the table itself: evicts any cached handle (closing its fd --
 * an already-open fd would otherwise keep referencing the old inode,
 * same reasoning kdb__evict_table's own doc comment gives for every
 * other file-replacing operation here), renames the .kdb file (and its
 * .lock file, best-effort -- it may not exist) on disk, then reopens
 * under the new name and updates the header's own stored table_name to
 * match (kdb_storage_open doesn't actually validate that field against
 * the filename it's given, so a stale value there wouldn't break
 * anything functionally, but leaving it wrong would be a real
 * consistency gap for anything that reads it later, e.g. dump tooling). */
KdbStatus kdb_rename_table(KumDB *db, const char *old_name, const char *new_name) {
    if (!db || !old_name || !new_name) {
        kdb_err_null_arg("db/old_name/new_name", "kdb_rename_table");
        return KDB_ERR_BAD_ARG;
    }
    if (db->read_only) {
        kdb_err_table_read_only(old_name);
        return KDB_ERR_READ_ONLY;
    }
    if (!kdb_storage_exists(db->data_dir, old_name)) {
        kdb_err_table_not_found(old_name);
        return KDB_ERR_NOT_FOUND;
    }
    if (kdb_storage_exists(db->data_dir, new_name)) {
        kdb_err_table_exists(new_name);
        return KDB_ERR_EXISTS;
    }

    kdb__evict_table(db, old_name);

    char old_path[4096], new_path[4096];
    char old_lock[4096 + 8], new_lock[4096 + 8];
    kdb_storage_path(db->data_dir, old_name, old_path, sizeof(old_path));
    kdb_storage_path(db->data_dir, new_name, new_path, sizeof(new_path));
    snprintf(old_lock, sizeof(old_lock), "%s.lock", old_path);
    snprintf(new_lock, sizeof(new_lock), "%s.lock", new_path);

    if (rename(old_path, new_path) != 0) {
        kdb_err_io(old_path, "rename table");
        return KDB_ERR_IO;
    }
    rename(old_lock, new_lock); /* best-effort -- no lock file is the common case */

    KdbTable *tbl = kdb__get_table(db, new_name);
    if (!tbl) return kdb_last_status();

    KDB_STRLCPY(tbl->header.table_name, new_name, KDB_MAX_NAME_LEN);
    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
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