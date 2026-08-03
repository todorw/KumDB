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
                               uint8_t     indexed,
                               uint8_t     unique) {
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

    if (unique) indexed = 1; /* a unique column always gets a real index to check against */

    KdbColumn *col = &tbl->header.columns[tbl->header.column_count];
    memset(col, 0, sizeof(*col));
    KDB_STRLCPY(col->name, col_name, KDB_MAX_NAME_LEN);
    col->type     = type;
    col->nullable = nullable;
    col->indexed  = indexed;
    col->unique   = unique;
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


KdbStatus kdb_table_create_index(KdbTable *tbl, const char *col_name) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_create_index");
        return KDB_ERR_BAD_ARG;
    }
    if (!kdb_table_has_column(tbl, col_name)) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }

    KdbColumn *col = NULL;
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0) { col = &tbl->header.columns[i]; break; }
    }
    if (col->indexed) {
        kdb_err_column_exists(col_name, tbl->name);
        return KDB_ERR_EXISTS;
    }

    KdbIndex *idx = kdb_index_new(col_name);
    if (!idx) return KDB_ERR_OOM;
    KdbIndex **new_indices = realloc(tbl->indices, (tbl->index_count + 1) * sizeof(KdbIndex *));
    if (!new_indices) {
        kdb_index_free(idx);
        kdb_err_oom("index array grow");
        return KDB_ERR_OOM;
    }
    new_indices[tbl->index_count] = idx;
    tbl->indices = new_indices;
    tbl->index_count++;
    kdb_index_rebuild(idx, tbl);

    col->indexed = 1;
    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

KdbStatus kdb_table_drop_index(KdbTable *tbl, const char *col_name) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_drop_index");
        return KDB_ERR_BAD_ARG;
    }
    if (!kdb_table_has_column(tbl, col_name)) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }

    KdbColumn *col = NULL;
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0) { col = &tbl->header.columns[i]; break; }
    }
    if (!col->indexed) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "column '%s' on table '%s' isn't indexed", col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }

    for (uint32_t i = 0; i < tbl->index_count; i++) {
        if (tbl->indices[i] && strcmp(tbl->indices[i]->col_name, col_name) == 0) {
            kdb_index_free(tbl->indices[i]);
            memmove(&tbl->indices[i], &tbl->indices[i + 1], (tbl->index_count - i - 1) * sizeof(KdbIndex *));
            tbl->index_count--;
            break;
        }
    }

    col->indexed = 0;
    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}


typedef struct {
    const char *old_name;
    const char *new_name;
} KdbRenameColCtx;

static int kdb__rename_column_transform(KdbRecord *r, void *ud) {
    const KdbRenameColCtx *ctx = (const KdbRenameColCtx *)ud;
    if (!r || !ctx) return 1;
    for (uint32_t i = 0; i < r->field_count; i++) {
        if (strcmp(r->fields[i].col_name, ctx->old_name) == 0) {
            KDB_STRLCPY(r->fields[i].col_name, ctx->new_name, KDB_MAX_NAME_LEN);
            break;
        }
    }
    return !r->deleted;
}

KdbStatus kdb_table_rename_column(KdbTable *tbl, const char *old_name, const char *new_name) {
    if (!tbl || !old_name || !new_name) {
        kdb_err_null_arg("tbl/old_name/new_name", "kdb_table_rename_column");
        return KDB_ERR_BAD_ARG;
    }
    if (!kdb_table_has_column(tbl, old_name)) {
        kdb_err_field_not_found(old_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }
    if (kdb_table_has_column(tbl, new_name)) {
        kdb_err_column_exists(new_name, tbl->name);
        return KDB_ERR_EXISTS;
    }

    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, old_name) == 0) {
            KDB_STRLCPY(tbl->header.columns[i].name, new_name, KDB_MAX_NAME_LEN);
            break;
        }
    }
    for (uint32_t i = 0; i < tbl->index_count; i++) {
        if (tbl->indices[i] && strcmp(tbl->indices[i]->col_name, old_name) == 0) {
            KDB_STRLCPY(tbl->indices[i]->col_name, new_name, KDB_MAX_NAME_LEN);
            break;
        }
    }

    KdbRenameColCtx ctx = { old_name, new_name };
    KdbStatus st = kdb_storage_rewrite(tbl, kdb__rename_column_transform, &ctx);
    if (st != KDB_OK) return st;

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

KdbStatus kdb_table_set_nullable(KdbTable *tbl, const char *col_name, int nullable) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_set_nullable");
        return KDB_ERR_BAD_ARG;
    }
    if (!kdb_table_has_column(tbl, col_name)) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0) {
            tbl->header.columns[i].nullable = nullable ? 1 : 0;
            break;
        }
    }
    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

KdbStatus kdb_table_set_unique(KdbTable *tbl, const char *col_name, int unique) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_set_unique");
        return KDB_ERR_BAD_ARG;
    }
    if (!kdb_table_has_column(tbl, col_name)) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }
    KdbColumn *col = NULL;
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0) { col = &tbl->header.columns[i]; break; }
    }
    col->unique = unique ? 1 : 0;
    if (unique && !col->indexed) return kdb_table_create_index(tbl, col_name); /* also flushes the header */

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


/* NOT NULL + UNIQUE checks for a record about to be inserted (not yet
 * written anywhere) -- a straightforward query per unique column, safe
 * because nothing is mid-scan of tbl->fp here (contrast with UPDATE's
 * version below). NULLs never conflict with each other for UNIQUE, same
 * convention real SQL uses. */
static KdbStatus kdb_table_check_insert_constraints(KdbTable *tbl, const KdbRecord *r) {
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        const KdbColumn *col = &tbl->header.columns[i];
        if (col->nullable && !col->unique) continue;

        const KdbValue *val = NULL;
        for (uint32_t j = 0; j < r->field_count; j++) {
            if (strcmp(r->fields[j].col_name, col->name) == 0) { val = &r->fields[j].value; break; }
        }
        int is_null = !val || val->type == KDB_TYPE_NULL;

        if (!col->nullable && is_null) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "Column '%s' on table '%s' is NOT NULL -- can't insert a NULL/missing value into it.",
                col->name, tbl->name);
            return KDB_ERR_VALIDATION;
        }

        if (col->unique && !is_null) {
            KdbQuery q;
            kdb_query_init(&q);
            KdbStatus st = kdb_query_add_filter_value(&q, col->name, KDB_OP_EQ, val, NULL);
            if (st != KDB_OK) { kdb_query_free(&q); return st; }
            size_t count = 0;
            st = kdb_query_count(tbl, &q, &count);
            kdb_query_free(&q);
            if (st != KDB_OK) return st;
            if (count > 0) {
                kdb_set_error(KDB_ERR_VALIDATION,
                    "Duplicate value for UNIQUE column '%s' on table '%s'.",
                    col->name, tbl->name);
                return KDB_ERR_VALIDATION;
            }
        }
    }
    return KDB_OK;
}

/* Same NOT NULL + UNIQUE checks, but for an UPDATE's would-be result --
 * this can't just query tbl per matching row like the insert version
 * does, because kdb_table_update's own rewrite (see kdb_storage_rewrite)
 * streams through tbl->fp sequentially via a transform callback; a nested
 * query against the same file handle mid-scan would corrupt that scan's
 * read position. Instead this takes its own single, separate, complete
 * snapshot of every row up front (safe -- not nested inside another scan),
 * computes what each row's value would be after the patch (only for rows
 * the query actually matches; everything else keeps its current value),
 * and compares every pair of *effective* values for a would-be conflict.
 * O(matched-rows * total-rows * enforced-columns), fine at the row counts
 * this engine targets (same precedent this file's dedupe/set-op helpers
 * already established). Catches a new row-vs-existing-row conflict AND a
 * conflict between two rows both being changed by this same UPDATE to the
 * same new value. Returns KDB_OK, or KDB_ERR_VALIDATION with a clear
 * error already set -- nothing is rewritten either way when this fails,
 * the caller checks before calling kdb_storage_rewrite at all. */
static KdbStatus kdb_table_check_update_constraints(KdbTable *tbl, const KdbQuery *query, const KdbRecord *patch) {
    int any_enforced = 0;
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (!tbl->header.columns[i].nullable || tbl->header.columns[i].unique) { any_enforced = 1; break; }
    }
    if (!any_enforced) return KDB_OK;

    KdbQuery all_q;
    kdb_query_init(&all_q);
    KdbResult all;
    KdbStatus st = kdb_query_execute(tbl, &all_q, &all);
    kdb_query_free(&all_q);
    if (st != KDB_OK) return st;

    KdbStatus result = KDB_OK;
    for (uint32_t ci = 0; ci < tbl->header.column_count && result == KDB_OK; ci++) {
        const KdbColumn *col = &tbl->header.columns[ci];
        if (col->nullable && !col->unique) continue;

        for (size_t i = 0; i < all.count && result == KDB_OK; i++) {
            KdbRecord *ri = &all.rows[i];
            if (!kdb_query_matches(query, ri)) continue; /* only rows this UPDATE actually touches need checking */

            const KdbValue *vi = NULL;
            for (uint32_t k = 0; k < patch->field_count; k++) {
                if (strcmp(patch->fields[k].col_name, col->name) == 0) { vi = &patch->fields[k].value; break; }
            }
            if (!vi) {
                for (uint32_t k = 0; k < ri->field_count; k++) {
                    if (strcmp(ri->fields[k].col_name, col->name) == 0) { vi = &ri->fields[k].value; break; }
                }
            }
            int is_null_i = !vi || vi->type == KDB_TYPE_NULL;

            if (!col->nullable && is_null_i) {
                kdb_set_error(KDB_ERR_VALIDATION,
                    "Column '%s' on table '%s' is NOT NULL -- this UPDATE would leave it NULL/missing on row %llu.",
                    col->name, tbl->name, (unsigned long long)ri->id);
                result = KDB_ERR_VALIDATION;
                break;
            }
            if (!col->unique || is_null_i) continue;

            for (size_t j = 0; j < all.count; j++) {
                if (j == i) continue;
                KdbRecord *rj = &all.rows[j];
                const KdbValue *vj = NULL;
                if (kdb_query_matches(query, rj)) {
                    for (uint32_t k = 0; k < patch->field_count; k++) {
                        if (strcmp(patch->fields[k].col_name, col->name) == 0) { vj = &patch->fields[k].value; break; }
                    }
                }
                if (!vj) {
                    for (uint32_t k = 0; k < rj->field_count; k++) {
                        if (strcmp(rj->fields[k].col_name, col->name) == 0) { vj = &rj->fields[k].value; break; }
                    }
                }
                if (vj && vj->type != KDB_TYPE_NULL && kdb_value_compare(vi, vj) == 0) {
                    kdb_set_error(KDB_ERR_VALIDATION,
                        "This UPDATE would give UNIQUE column '%s' on table '%s' a duplicate value (rows %llu and %llu).",
                        col->name, tbl->name, (unsigned long long)ri->id, (unsigned long long)rj->id);
                    result = KDB_ERR_VALIDATION;
                    break;
                }
            }
        }
    }
    kdb_result_free(&all);
    return result;
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

    st = kdb_table_check_insert_constraints(tbl, r);
    if (st != KDB_OK) { kdb_lock_release(&lock); return st; }


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
        st = kdb_table_check_insert_constraints(tbl, &records[i]);
        if (st != KDB_OK) { kdb_lock_release(&lock); return st; }

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

    st = kdb_table_check_update_constraints(tbl, query, patch);
    if (st != KDB_OK) { kdb_lock_release(&lock); return st; }

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