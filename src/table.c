#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/table.h"
#include "../include/storage.h"
#include "../include/index.h"
#include "../include/record.h"
#include "../include/types.h"
#include "../include/error.h"
#include "../include/lock.h"
#include "../include/query.h"


KdbStatus kdb_table_create(const char      *data_dir,
                           const char      *table_name,
                           const KdbColumn *columns,
                           uint32_t         column_count) {
    return kdb_storage_create(data_dir, table_name, columns, column_count);
}

KdbStatus kdb_table_open(KdbTable   *tbl,
                         const char *data_dir,
                         const char *table_name) {
    KdbStatus st = kdb_storage_open(tbl, data_dir, table_name);
    if (st != KDB_OK) return st;

    
    if (tbl->header.column_count > 0) {
        KdbIndex *idx_arr[KDB_MAX_COLUMNS];
        memset(idx_arr, 0, sizeof(idx_arr));
        uint32_t idx_count = 0;

        st = kdb_index_build_for_table(tbl->header.columns,
                                       tbl->header.column_count,
                                       idx_arr, &idx_count);
        if (st != KDB_OK) {
            kdb_storage_close(tbl);
            return st;
        }

        if (idx_count > 0) {
            tbl->indices = (KdbIndex **)calloc(idx_count, sizeof(KdbIndex *));
            if (!tbl->indices) {
                for (uint32_t i = 0; i < idx_count; i++) kdb_index_free(idx_arr[i]);
                kdb_storage_close(tbl);
                kdb_err_oom("index array");
                return KDB_ERR_OOM;
            }
            memcpy(tbl->indices, idx_arr, idx_count * sizeof(KdbIndex *));
            tbl->index_count = idx_count;

            
            for (uint32_t i = 0; i < idx_count; i++) {
                st = kdb_index_rebuild(tbl->indices[i], tbl);
                if (st != KDB_OK) {
                    kdb_index_free_array(tbl->indices, tbl->index_count);
                    tbl->indices     = NULL;
                    tbl->index_count = 0;
                    kdb_storage_close(tbl);
                    return st;
                }
            }
        }
    }

    return KDB_OK;
}

void kdb_table_close(KdbTable *tbl) {
    if (!tbl) return;
    if (tbl->indices) {
        kdb_index_free_array(tbl->indices, tbl->index_count);
        tbl->indices     = NULL;
        tbl->index_count = 0;
    }
    kdb_storage_close(tbl);
}

KdbStatus kdb_table_drop(KdbTable   *tbl,
                         const char *data_dir,
                         const char *table_name) {
    if (tbl) kdb_table_close(tbl);
    return kdb_storage_drop(data_dir, table_name);
}

int kdb_storage_table_exists(const char *data_dir, const char *table_name) {
    return kdb_storage_exists(data_dir, table_name);
}


const KdbColumn *kdb_table_get_column(const KdbTable *tbl, const char *col_name) {
    if (!tbl || !col_name) return NULL;
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0)
            return &tbl->header.columns[i];
    }
    return NULL;
}

int kdb_table_has_column(const KdbTable *tbl, const char *col_name) {
    return kdb_table_get_column(tbl, col_name) != NULL;
}

KdbStatus kdb_table_add_column(KdbTable   *tbl,
                               const char *col_name,
                               KdbType     type,
                               uint8_t     nullable,
                               uint8_t     indexed) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_add_column");
        return KDB_ERR_BAD_ARG;
    }
    if (kdb_table_has_column(tbl, col_name)) {
        kdb_err_column_exists(col_name, tbl->name);
        return KDB_ERR_EXISTS;
    }
    if (tbl->header.column_count >= KDB_MAX_COLUMNS) {
        kdb_err_table_full(tbl->name);
        return KDB_ERR_FULL;
    }

    KdbColumn *col = &tbl->header.columns[tbl->header.column_count];
    memset(col, 0, sizeof(*col));
    KDB_STRLCPY(col->name, col_name, KDB_MAX_NAME_LEN);
    col->type     = type;
    col->nullable = nullable;
    col->indexed  = indexed;
    tbl->header.column_count++;
    tbl->dirty = 1;

    if (indexed) {
        KdbIndex *idx = kdb_index_new(col_name);
        if (!idx) return KDB_ERR_OOM;

        KdbIndex **new_indices = realloc(tbl->indices,
                                         (tbl->index_count + 1) * sizeof(KdbIndex *));
        if (!new_indices) {
            kdb_index_free(idx);
            kdb_err_oom("index array grow");
            return KDB_ERR_OOM;
        }
        new_indices[tbl->index_count] = idx;
        tbl->indices     = new_indices;
        tbl->index_count++;

        
        kdb_index_rebuild(idx, tbl);
    }

    return kdb_storage_flush_header(tbl);
}

KdbStatus kdb_table_drop_column(KdbTable   *tbl,
                                const char *col_name) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_drop_column");
        return KDB_ERR_BAD_ARG;
    }
    if (!kdb_table_has_column(tbl, col_name)) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }

    
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0) {
            memmove(&tbl->header.columns[i],
                    &tbl->header.columns[i + 1],
                    (tbl->header.column_count - i - 1) * sizeof(KdbColumn));
            tbl->header.column_count--;
            break;
        }
    }

    
    for (uint32_t i = 0; i < tbl->index_count; i++) {
        if (tbl->indices[i] && strcmp(tbl->indices[i]->col_name, col_name) == 0) {
            kdb_index_free(tbl->indices[i]);
            memmove(&tbl->indices[i], &tbl->indices[i + 1],
                    (tbl->index_count - i - 1) * sizeof(KdbIndex *));
            tbl->index_count--;
            break;
        }
    }

    
    extern const char *kdb__drop_col_name;
    kdb__drop_col_name = col_name;

    extern int kdb__drop_column_transform(KdbRecord *r, void *ud);
    KdbStatus st = kdb_storage_rewrite(tbl, kdb__drop_column_transform, (void *)col_name);
    if (st != KDB_OK) return st;

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}


const char *kdb__drop_col_name = NULL;

int kdb__drop_column_transform(KdbRecord *r, void *ud) {
    const char *col_name = (const char *)ud;
    if (!r || !col_name) return 1;
    
    for (uint32_t i = 0; i < r->field_count; i++) {
        if (strcmp(r->fields[i].col_name, col_name) == 0) {
            kdb_value_free(&r->fields[i].value);
            memmove(&r->fields[i], &r->fields[i + 1],
                    (r->field_count - i - 1) * sizeof(KdbRecordField));
            r->field_count--;
            break;
        }
    }
    return !r->deleted;
}

KdbStatus kdb_table_infer_schema(KdbTable *tbl, const KdbRecord *r) {
    if (!tbl || !r) {
        kdb_err_null_arg("tbl/r", "kdb_table_infer_schema");
        return KDB_ERR_BAD_ARG;
    }

    for (uint32_t i = 0; i < r->field_count; i++) {
        if (kdb_table_has_column(tbl, r->fields[i].col_name)) continue;
        if (tbl->header.column_count >= KDB_MAX_COLUMNS) {
            kdb_err_table_full(tbl->name);
            return KDB_ERR_FULL;
        }
        KdbColumn *col = &tbl->header.columns[tbl->header.column_count];
        memset(col, 0, sizeof(*col));
        KDB_STRLCPY(col->name, r->fields[i].col_name, KDB_MAX_NAME_LEN);
        col->type     = r->fields[i].value.type;
        col->nullable = 1;
        col->indexed  = 0;
        tbl->header.column_count++;
    }

    tbl->dirty = 1;
    return KDB_OK;
}


KdbStatus kdb_table_insert(KdbTable *tbl, KdbRecord *r) {
    if (!tbl || !r) {
        kdb_err_null_arg("tbl/r", "kdb_table_insert");
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->read_only) {
        kdb_err_table_read_only(tbl->name);
        return KDB_ERR_READ_ONLY;
    }

    KdbLock lock = { .fd = -1 };
    KdbStatus st = kdb_lock_acquire(&lock, tbl->path, 1);
    if (st != KDB_OK) return st;

    
    if (tbl->header.column_count == 0) {
        st = kdb_table_infer_schema(tbl, r);
        if (st != KDB_OK) { kdb_lock_release(&lock); return st; }
    }

    
    if (fseek(tbl->fp, 0, SEEK_END) != 0) {
        kdb_lock_release(&lock);
        kdb_err_io(tbl->path, "fseek before insert");
        return KDB_ERR_IO;
    }
    uint64_t file_offset = (uint64_t)ftell(tbl->fp);

    st = kdb_storage_append(tbl, r);
    if (st != KDB_OK) { kdb_lock_release(&lock); return st; }

    
    for (uint32_t i = 0; i < tbl->index_count; i++) {
        kdb_index_insert(tbl->indices[i], r, file_offset);
    }

    kdb_storage_flush_header(tbl);
    kdb_lock_release(&lock);
    return KDB_OK;
}

KdbStatus kdb_table_insert_batch(KdbTable  *tbl,
                                 KdbRecord *records,
                                 size_t     count,
                                 size_t    *inserted_out) {
    if (!tbl || !records) {
        kdb_err_null_arg("tbl/records", "kdb_table_insert_batch");
        return KDB_ERR_BAD_ARG;
    }
    if (inserted_out) *inserted_out = 0;

    KdbLock lock = { .fd = -1 };
    KdbStatus st = kdb_lock_acquire(&lock, tbl->path, 1);
    if (st != KDB_OK) return st;

    if (tbl->header.column_count == 0 && count > 0) {
        st = kdb_table_infer_schema(tbl, &records[0]);
        if (st != KDB_OK) { kdb_lock_release(&lock); return st; }
    }

    for (size_t i = 0; i < count; i++) {
        if (fseek(tbl->fp, 0, SEEK_END) != 0) {
            kdb_lock_release(&lock);
            return KDB_ERR_IO;
        }
        uint64_t file_offset = (uint64_t)ftell(tbl->fp);

        st = kdb_storage_append(tbl, &records[i]);
        if (st != KDB_OK) { kdb_lock_release(&lock); return st; }

        for (uint32_t j = 0; j < tbl->index_count; j++)
            kdb_index_insert(tbl->indices[j], &records[i], file_offset);

        if (inserted_out) (*inserted_out)++;
    }

    kdb_storage_flush_header(tbl);
    kdb_lock_release(&lock);
    return KDB_OK;
}


typedef struct {
    const KdbQuery  *query;
    const KdbRecord *patch;
    size_t          *updated_out;
} KdbUpdateCtx;

static int kdb__update_transform(KdbRecord *r, void *ud) {
    KdbUpdateCtx *ctx = (KdbUpdateCtx *)ud;
    if (r->deleted) return 1;

    

    if (!kdb_query_matches(ctx->query, r)) return 1;

    
    for (uint32_t i = 0; i < ctx->patch->field_count; i++) {
        kdb_record_set_field(r,
                             ctx->patch->fields[i].col_name,
                             &ctx->patch->fields[i].value);
    }
    r->updated_at = (uint64_t)time(NULL);
    if (ctx->updated_out) (*ctx->updated_out)++;
    return 1;
}

KdbStatus kdb_table_update(KdbTable        *tbl,
                           const KdbQuery  *query,
                           const KdbRecord *patch,
                           size_t          *updated_out) {
    if (!tbl || !query || !patch) {
        kdb_err_null_arg("tbl/query/patch", "kdb_table_update");
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->read_only) {
        kdb_err_table_read_only(tbl->name);
        return KDB_ERR_READ_ONLY;
    }
    if (updated_out) *updated_out = 0;

    KdbLock lock = { .fd = -1 };
    KdbStatus st = kdb_lock_acquire(&lock, tbl->path, 1);
    if (st != KDB_OK) return st;

    KdbUpdateCtx ctx = { .query = query, .patch = patch, .updated_out = updated_out };
    st = kdb_storage_rewrite(tbl, kdb__update_transform, &ctx);

    if (st == KDB_OK && tbl->index_count > 0) {
        
        for (uint32_t i = 0; i < tbl->index_count; i++)
            kdb_index_rebuild(tbl->indices[i], tbl);
    }

    kdb_lock_release(&lock);
    return st;
}


typedef struct {
    const KdbQuery *query;
    size_t         *deleted_out;
} KdbDeleteCtx;

static int kdb__delete_transform(KdbRecord *r, void *ud) {
    KdbDeleteCtx *ctx = (KdbDeleteCtx *)ud;
    if (r->deleted) return 1;
    if (kdb_query_matches(ctx->query, r)) {
        r->deleted = 1;
        if (ctx->deleted_out) (*ctx->deleted_out)++;
    }
    return 1;
}

KdbStatus kdb_table_delete(KdbTable       *tbl,
                           const KdbQuery *query,
                           size_t         *deleted_out) {
    if (!tbl || !query) {
        kdb_err_null_arg("tbl/query", "kdb_table_delete");
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->read_only) {
        kdb_err_table_read_only(tbl->name);
        return KDB_ERR_READ_ONLY;
    }
    if (deleted_out) *deleted_out = 0;

    KdbLock lock = { .fd = -1 };
    KdbStatus st = kdb_lock_acquire(&lock, tbl->path, 1);
    if (st != KDB_OK) return st;

    KdbDeleteCtx ctx = { .query = query, .deleted_out = deleted_out };
    st = kdb_storage_rewrite(tbl, kdb__delete_transform, &ctx);

    if (st == KDB_OK && tbl->index_count > 0) {
        for (uint32_t i = 0; i < tbl->index_count; i++)
            kdb_index_rebuild(tbl->indices[i], tbl);
    }

    kdb_lock_release(&lock);
    return st;
}


KdbStatus kdb_table_compact(KdbTable *tbl) {
    if (!tbl) { kdb_err_null_arg("tbl", "kdb_table_compact"); return KDB_ERR_BAD_ARG; }

    KdbLock lock = { .fd = -1 };
    KdbStatus st = kdb_lock_acquire(&lock, tbl->path, 1);
    if (st != KDB_OK) return st;

    st = kdb_storage_compact(tbl);

    if (st == KDB_OK && tbl->index_count > 0) {
        for (uint32_t i = 0; i < tbl->index_count; i++)
            kdb_index_rebuild(tbl->indices[i], tbl);
    }

    kdb_lock_release(&lock);
    return st;
}

void kdb_table_print_schema(const KdbTable *tbl, FILE *fp) {
    if (!tbl || !fp) return;
    fprintf(fp, "Table: %s (%u columns)\n", tbl->name, tbl->header.column_count);
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        const KdbColumn *c = &tbl->header.columns[i];
        fprintf(fp, "  %-24s %s%s%s\n",
                c->name,
                kdb_type_name(c->type),
                c->nullable ? "" : " NOT NULL",
                c->indexed  ? " INDEXED" : "");
    }
}

void kdb_table_print_stats(KdbTable *tbl, FILE *fp) {
    if (!tbl || !fp) return;
    KdbStorageStats stats;
    if (kdb_storage_stats(tbl, &stats) == KDB_OK) {
        fprintf(fp, "Table: %s\n", tbl->name);
        fprintf(fp, "  File size:     %llu bytes\n", (unsigned long long)stats.file_size_bytes);
        fprintf(fp, "  Live records:  %llu\n",       (unsigned long long)stats.live_count);
        fprintf(fp, "  Deleted:       %llu\n",       (unsigned long long)stats.deleted_count);
        fprintf(fp, "  Fragmentation: %.1f%%\n",     stats.fragmentation_ratio * 100.0);
    }
}

uint64_t kdb_table_count(KdbTable *tbl) {
    if (!tbl) return 0;
    return tbl->header.record_count;
}