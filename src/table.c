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
        KdbIndex *idx_arr[KDB_MAX_COLUMNS + KDB_MAX_COMPOSITE_INDEXES];
        memset(idx_arr, 0, sizeof(idx_arr));
        uint32_t idx_count = 0;

        st = kdb_index_build_for_table(tbl->header.columns,
                                       tbl->header.column_count,
                                       idx_arr, &idx_count);
        if (st != KDB_OK) {
            kdb_storage_close(tbl);
            return st;
        }

        st = kdb_index_build_composite_for_table(tbl->header.columns,
                                                 tbl->header.composite_indexes,
                                                 tbl->header.n_composite_indexes,
                                                 idx_arr, &idx_count);
        if (st != KDB_OK) {
            for (uint32_t i = 0; i < idx_count; i++) kdb_index_free(idx_arr[i]);
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

    /* Any FK on col_name goes away automatically -- it lives on the
     * KdbColumn entry itself, just shifted out above. CHECK constraints
     * are stored separately (by name, not position), so a dropped
     * column's checks need their own cleanup -- otherwise a dangling
     * check referencing a nonexistent column would linger. */
    for (uint32_t i = 0; i < tbl->header.n_checks; ) {
        if (strcmp(tbl->header.checks[i].col_name, col_name) == 0) {
            memmove(&tbl->header.checks[i], &tbl->header.checks[i + 1],
                    (tbl->header.n_checks - i - 1) * sizeof(KdbCheckDef));
            tbl->header.n_checks--;
        } else {
            i++;
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

/* A real composite (multi-column) index -- one KdbIndex hashing all
 * n_cols columns' values together, not n_cols independent single-column
 * indexes. col_names order matters for how the index itself hashes, but
 * not for finding/dropping it again later (kdb_index_find_composite/this
 * function's own drop counterpart match by column SET, not order). */
KdbStatus kdb_table_create_composite_index(KdbTable *tbl, const char **col_names, uint32_t n_cols) {
    if (!tbl || !col_names || n_cols < 2 || n_cols > KDB_MAX_COMPOSITE_COLS) {
        kdb_err_bad_arg("col_names/n_cols", "kdb_table_create_composite_index needs 2..KDB_MAX_COMPOSITE_COLS columns");
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->header.n_composite_indexes >= KDB_MAX_COMPOSITE_INDEXES) {
        kdb_set_error(KDB_ERR_FULL, "table '%s' already has the max %d composite indexes", tbl->name, KDB_MAX_COMPOSITE_INDEXES);
        return KDB_ERR_FULL;
    }

    uint8_t positions[KDB_MAX_COMPOSITE_COLS];
    for (uint32_t i = 0; i < n_cols; i++) {
        const KdbColumn *c = kdb_table_get_column(tbl, col_names[i]);
        if (!c) {
            kdb_err_field_not_found(col_names[i], tbl->name);
            return KDB_ERR_NOT_FOUND;
        }
        positions[i] = (uint8_t)(c - tbl->header.columns);
    }

    if (kdb_index_find_composite(tbl->indices, tbl->index_count, col_names, n_cols)) {
        kdb_set_error(KDB_ERR_EXISTS, "a composite index on exactly these columns already exists on table '%s'", tbl->name);
        return KDB_ERR_EXISTS;
    }

    KdbIndex *idx = kdb_index_new_composite(col_names, n_cols);
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

    KdbCompositeIndexDef *def = &tbl->header.composite_indexes[tbl->header.n_composite_indexes];
    memset(def, 0, sizeof(*def));
    for (uint32_t i = 0; i < n_cols; i++) def->col_positions[i] = positions[i];
    def->n_cols = (uint8_t)n_cols;
    tbl->header.n_composite_indexes++;

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

KdbStatus kdb_table_drop_composite_index(KdbTable *tbl, const char **col_names, uint32_t n_cols) {
    if (!tbl || !col_names || n_cols == 0) {
        kdb_err_null_arg("tbl/col_names", "kdb_table_drop_composite_index");
        return KDB_ERR_BAD_ARG;
    }

    KdbIndex *idx = kdb_index_find_composite(tbl->indices, tbl->index_count, col_names, n_cols);
    if (!idx) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "no composite index on exactly these columns exists on table '%s'", tbl->name);
        return KDB_ERR_NOT_FOUND;
    }

    uint8_t positions[KDB_MAX_COMPOSITE_COLS];
    for (uint32_t i = 0; i < n_cols; i++) {
        const KdbColumn *c = kdb_table_get_column(tbl, col_names[i]);
        positions[i] = c ? (uint8_t)(c - tbl->header.columns) : 0xFF;
    }
    for (uint8_t d = 0; d < tbl->header.n_composite_indexes; d++) {
        KdbCompositeIndexDef *def = &tbl->header.composite_indexes[d];
        if (def->n_cols != n_cols) continue;
        int all_match = 1;
        for (uint32_t i = 0; i < n_cols && all_match; i++) {
            int found = 0;
            for (uint32_t k = 0; k < def->n_cols; k++) if (def->col_positions[k] == positions[i]) { found = 1; break; }
            if (!found) all_match = 0;
        }
        if (all_match) {
            memmove(def, def + 1, (size_t)(tbl->header.n_composite_indexes - d - 1) * sizeof(*def));
            memset(&tbl->header.composite_indexes[tbl->header.n_composite_indexes - 1], 0, sizeof(*def));
            tbl->header.n_composite_indexes--;
            break;
        }
    }

    for (uint32_t i = 0; i < tbl->index_count; i++) {
        if (tbl->indices[i] == idx) {
            kdb_index_free(idx);
            memmove(&tbl->indices[i], &tbl->indices[i + 1], (tbl->index_count - i - 1) * sizeof(KdbIndex *));
            tbl->index_count--;
            break;
        }
    }

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


typedef struct { const char *col_name; KdbType new_type; } KdbAlterTypeCtx;

static int kdb__alter_column_type_transform(KdbRecord *r, void *ud) {
    KdbAlterTypeCtx *ctx = (KdbAlterTypeCtx *)ud;
    if (!r) return 1;
    for (uint32_t i = 0; i < r->field_count; i++) {
        if (strcmp(r->fields[i].col_name, ctx->col_name) != 0) continue;
        KdbValue converted;
        if (kdb_value_cast(&r->fields[i].value, ctx->new_type, &converted) != KDB_OK) {
            kdb_set_error(KDB_ERR_BAD_TYPE,
                "can't change column '%s' to %s -- record id %llu currently has a %s value that "
                "doesn't convert; original table left untouched",
                ctx->col_name, kdb_type_name(ctx->new_type),
                (unsigned long long)r->id, kdb_type_name(r->fields[i].value.type));
            return -1;
        }
        kdb_value_free(&r->fields[i].value);
        r->fields[i].value = converted;
        break;
    }
    return !r->deleted;
}

/* Converts every existing row's value for col_name to new_type (via
 * kdb_value_cast -- NULL stays NULL, otherwise the usual INT/FLOAT/BOOL/
 * STRING coercions, same rules CAST(x AS type) uses in SQL) and updates
 * the column's declared type -- a real data migration, not just a
 * metadata flip. If even one existing value can't convert, the whole
 * change is aborted and the original table file is left completely
 * untouched (kdb_storage_rewrite's transform-abort path) -- never left
 * half-migrated. Any index touching this column (single-column or
 * composite) is rebuilt afterward, since its hash buckets were built
 * from the old-typed values. */
KdbStatus kdb_table_alter_column_type(KdbTable *tbl, const char *col_name, KdbType new_type) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_alter_column_type");
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
    if (col->type == new_type) return KDB_OK; /* no-op: nothing to migrate */

    KdbType old_type = col->type;
    col->type = new_type; /* kdb_storage_rewrite writes tbl->header (already updated) into the new file */

    KdbAlterTypeCtx ctx = { col_name, new_type };
    KdbStatus st = kdb_storage_rewrite(tbl, kdb__alter_column_type_transform, &ctx);
    if (st != KDB_OK) {
        col->type = old_type; /* rewrite aborted -- the file was untouched, keep in-memory state matching it */
        return st;
    }

    for (uint32_t i = 0; i < tbl->index_count; i++) {
        KdbIndex *idx = tbl->indices[i];
        if (!idx) continue;
        int touches = strcmp(idx->col_name, col_name) == 0;
        for (uint32_t e = 0; !touches && e < idx->n_extra_cols; e++) {
            if (strcmp(idx->extra_cols[e], col_name) == 0) touches = 1;
        }
        if (touches) {
            KdbStatus ist = kdb_index_rebuild(idx, tbl);
            if (ist != KDB_OK) return ist;
        }
    }

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

/* Sets col_name's FK metadata only -- doesn't verify ref_table/ref_col
 * actually exist (this handle has no way to look up another table; see
 * kdb_add_foreign_key in kumdb.c, which validates that before calling
 * this). One FK per column -- KDB_ERR_EXISTS if col_name already has one. */
KdbStatus kdb_table_add_foreign_key(KdbTable *tbl, const char *col_name,
                                    const char *ref_table, const char *ref_col,
                                    KdbFkAction on_delete, KdbFkAction on_update) {
    if (!tbl || !col_name || !ref_table || !ref_col) {
        kdb_err_null_arg("tbl/col_name/ref_table/ref_col", "kdb_table_add_foreign_key");
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
    if (col->has_fk) {
        kdb_set_error(KDB_ERR_EXISTS, "Column '%s' on table '%s' already has a foreign key.", col_name, tbl->name);
        return KDB_ERR_EXISTS;
    }
    if ((on_delete == KDB_FK_SET_NULL || on_update == KDB_FK_SET_NULL) && !col->nullable) {
        kdb_set_error(KDB_ERR_BAD_ARG,
            "Column '%s' on table '%s' is NOT NULL -- SET NULL doesn't make sense for its foreign key.",
            col_name, tbl->name);
        return KDB_ERR_BAD_ARG;
    }
    col->has_fk = 1;
    KDB_STRLCPY(col->fk_ref_table, ref_table, KDB_MAX_NAME_LEN);
    KDB_STRLCPY(col->fk_ref_col, ref_col, KDB_MAX_NAME_LEN);
    col->on_delete = (uint8_t)on_delete;
    col->on_update = (uint8_t)on_update;

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

KdbStatus kdb_table_drop_foreign_key(KdbTable *tbl, const char *col_name) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_drop_foreign_key");
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
    if (!col->has_fk) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "Column '%s' on table '%s' has no foreign key.", col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }
    col->has_fk = 0;
    memset(col->fk_ref_table, 0, sizeof(col->fk_ref_table));
    memset(col->fk_ref_col, 0, sizeof(col->fk_ref_col));

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

/* A composite (multi-column) foreign key -- doesn't verify ref_table/
 * ref_cols actually exist (this handle has no way to look up another
 * table; see kdb_add_composite_foreign_key in kumdb.c, which validates
 * that first). n_cols must be 2..KDB_MAX_COMPOSITE_COLS and equal
 * n_ref_cols (positional correspondence: ref_cols[i] is what col_names[i]
 * references). KDB_ERR_EXISTS if a composite FK on exactly this column
 * set (any order) already exists on this table; KDB_ERR_FULL past
 * KDB_MAX_COMPOSITE_FKS. Unlike single-column FK, this never touches the
 * pseudo-columns id/created_at/updated_at on either side. */
KdbStatus kdb_table_add_composite_foreign_key(KdbTable *tbl, const char **col_names, uint32_t n_cols,
                                              const char *ref_table, const char **ref_cols, uint32_t n_ref_cols,
                                              KdbFkAction on_delete, KdbFkAction on_update) {
    if (!tbl || !col_names || !ref_table || !ref_cols || n_cols < 2 || n_cols > KDB_MAX_COMPOSITE_COLS) {
        kdb_err_bad_arg("col_names/n_cols", "kdb_table_add_composite_foreign_key needs 2..KDB_MAX_COMPOSITE_COLS columns");
        return KDB_ERR_BAD_ARG;
    }
    if (n_cols != n_ref_cols) {
        kdb_set_error(KDB_ERR_BAD_ARG,
            "composite foreign key needs the same number of columns on both sides (%u vs %u)", n_cols, n_ref_cols);
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->header.n_composite_fks >= KDB_MAX_COMPOSITE_FKS) {
        kdb_set_error(KDB_ERR_FULL, "table '%s' already has the max %d composite foreign keys", tbl->name, KDB_MAX_COMPOSITE_FKS);
        return KDB_ERR_FULL;
    }

    uint8_t positions[KDB_MAX_COMPOSITE_COLS];
    int all_nullable = 1;
    for (uint32_t i = 0; i < n_cols; i++) {
        const KdbColumn *c = kdb_table_get_column(tbl, col_names[i]);
        if (!c) {
            kdb_err_field_not_found(col_names[i], tbl->name);
            return KDB_ERR_NOT_FOUND;
        }
        positions[i] = (uint8_t)(c - tbl->header.columns);
        if (!c->nullable) all_nullable = 0;
    }
    if ((on_delete == KDB_FK_SET_NULL || on_update == KDB_FK_SET_NULL) && !all_nullable) {
        kdb_set_error(KDB_ERR_BAD_ARG,
            "every column of this composite foreign key on table '%s' must be nullable for SET NULL to make sense", tbl->name);
        return KDB_ERR_BAD_ARG;
    }

    for (uint8_t d = 0; d < tbl->header.n_composite_fks; d++) {
        KdbCompositeFkDef *def = &tbl->header.composite_fks[d];
        if (def->n_cols != n_cols) continue;
        int all_match = 1;
        for (uint32_t i = 0; i < n_cols && all_match; i++) {
            int found = 0;
            for (uint32_t k = 0; k < def->n_cols; k++) if (def->col_positions[k] == positions[i]) { found = 1; break; }
            if (!found) all_match = 0;
        }
        if (all_match) {
            kdb_set_error(KDB_ERR_EXISTS, "a composite foreign key on exactly these columns already exists on table '%s'", tbl->name);
            return KDB_ERR_EXISTS;
        }
    }

    KdbCompositeFkDef *def = &tbl->header.composite_fks[tbl->header.n_composite_fks];
    memset(def, 0, sizeof(*def));
    for (uint32_t i = 0; i < n_cols; i++) {
        def->col_positions[i] = positions[i];
        KDB_STRLCPY(def->ref_cols[i], ref_cols[i], KDB_MAX_NAME_LEN);
    }
    def->n_cols = (uint8_t)n_cols;
    KDB_STRLCPY(def->ref_table, ref_table, KDB_MAX_NAME_LEN);
    def->on_delete = (uint8_t)on_delete;
    def->on_update = (uint8_t)on_update;
    tbl->header.n_composite_fks++;

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

/* Drops a composite foreign key matching exactly this column set (any
 * order) -- KDB_ERR_NOT_FOUND if none does. */
KdbStatus kdb_table_drop_composite_foreign_key(KdbTable *tbl, const char **col_names, uint32_t n_cols) {
    if (!tbl || !col_names || n_cols == 0 || n_cols > KDB_MAX_COMPOSITE_COLS) {
        kdb_err_null_arg("tbl/col_names", "kdb_table_drop_composite_foreign_key");
        return KDB_ERR_BAD_ARG;
    }

    uint8_t positions[KDB_MAX_COMPOSITE_COLS];
    for (uint32_t i = 0; i < n_cols; i++) {
        const KdbColumn *c = kdb_table_get_column(tbl, col_names[i]);
        positions[i] = c ? (uint8_t)(c - tbl->header.columns) : 0xFF;
    }
    for (uint8_t d = 0; d < tbl->header.n_composite_fks; d++) {
        KdbCompositeFkDef *def = &tbl->header.composite_fks[d];
        if (def->n_cols != n_cols) continue;
        int all_match = 1;
        for (uint32_t i = 0; i < n_cols && all_match; i++) {
            int found = 0;
            for (uint32_t k = 0; k < def->n_cols; k++) if (def->col_positions[k] == positions[i]) { found = 1; break; }
            if (!found) all_match = 0;
        }
        if (all_match) {
            memmove(def, def + 1, (size_t)(tbl->header.n_composite_fks - d - 1) * sizeof(*def));
            memset(&tbl->header.composite_fks[tbl->header.n_composite_fks - 1], 0, sizeof(*def));
            tbl->header.n_composite_fks--;
            tbl->dirty = 1;
            return kdb_storage_flush_header(tbl);
        }
    }

    kdb_set_error(KDB_ERR_NOT_FOUND, "no composite foreign key on exactly these columns exists on table '%s'", tbl->name);
    return KDB_ERR_NOT_FOUND;
}

/* CHECK (col_name op literal) -- op restricted to the six plain
 * comparisons (see KdbCheckDef), literal one of INT/FLOAT/BOOL/STRING
 * (not NULL -- a NULL comparison target isn't a meaningful check, reject
 * it explicitly rather than storing something kdb_value_matches would
 * evaluate ambiguously). Enforced from here on by kdb_table_check_
 * insert_constraints/kdb_table_check_update_constraints below, same as
 * NOT NULL/UNIQUE -- existing rows aren't retroactively checked. */
KdbStatus kdb_table_add_check(KdbTable *tbl, const char *col_name, KdbOperator op, const KdbValue *literal) {
    if (!tbl || !col_name || !literal) {
        kdb_err_null_arg("tbl/col_name/literal", "kdb_table_add_check");
        return KDB_ERR_BAD_ARG;
    }
    if (!kdb_table_has_column(tbl, col_name)) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }
    if (op != KDB_OP_EQ && op != KDB_OP_NEQ && op != KDB_OP_GT &&
        op != KDB_OP_GTE && op != KDB_OP_LT && op != KDB_OP_LTE) {
        kdb_set_error(KDB_ERR_BAD_ARG, "CHECK only supports =, !=, >, >=, <, <= comparisons.");
        return KDB_ERR_BAD_ARG;
    }
    if (literal->type != KDB_TYPE_INT && literal->type != KDB_TYPE_FLOAT &&
        literal->type != KDB_TYPE_BOOL && literal->type != KDB_TYPE_STRING) {
        kdb_set_error(KDB_ERR_BAD_ARG, "CHECK's literal must be an INT, FLOAT, BOOL, or STRING value.");
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->header.n_checks >= KDB_MAX_CHECK_CONSTRAINTS) {
        kdb_set_error(KDB_ERR_FULL, "Table '%s' already has %d CHECK constraints (max).", tbl->name, KDB_MAX_CHECK_CONSTRAINTS);
        return KDB_ERR_FULL;
    }

    KdbCheckDef *def = &tbl->header.checks[tbl->header.n_checks];
    memset(def, 0, sizeof(*def));
    KDB_STRLCPY(def->col_name, col_name, KDB_MAX_NAME_LEN);
    def->op       = (uint8_t)op;
    def->val_type = (uint8_t)literal->type;
    switch (literal->type) {
        case KDB_TYPE_INT:    def->as_int   = literal->v.as_int;   break;
        case KDB_TYPE_FLOAT:  def->as_float = literal->v.as_float; break;
        case KDB_TYPE_BOOL:   def->as_bool  = literal->v.as_bool;  break;
        case KDB_TYPE_STRING: KDB_STRLCPY(def->as_string, literal->v.as_string.data, sizeof(def->as_string)); break;
        default: break;
    }
    tbl->header.n_checks++;

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

/* Builds a real (non-owning-except-string) KdbValue out of a KdbCheckDef's
 * fixed-size literal, for handing to kdb_value_matches. Caller must
 * kdb_value_free() the result (the STRING case owns a fresh strdup). */
static KdbStatus kdb__check_def_to_value(const KdbCheckDef *def, KdbValue *out) {
    switch ((KdbType)def->val_type) {
        case KDB_TYPE_INT:    return kdb_value_from_int(def->as_int, out);
        case KDB_TYPE_FLOAT:  return kdb_value_from_float(def->as_float, out);
        case KDB_TYPE_BOOL:   return kdb_value_from_bool(def->as_bool, out);
        case KDB_TYPE_STRING: return kdb_value_from_string(def->as_string, KDB_TYPE_STRING, out);
        default:              return KDB_ERR_BAD_TYPE;
    }
}

/* Sets col_name's DEFAULT value -- an INT/FLOAT/BOOL/STRING literal, same
 * restricted set as CHECK (never NULL: a nullable column already defaults
 * to NULL when omitted, so there's nothing for "DEFAULT NULL" to add).
 * Overwrites any DEFAULT col_name already had (no KDB_ERR_EXISTS, unlike
 * FK/CHECK -- redeclaring a column's default is a normal thing to do, not
 * an error). Enforced from here on by kdb_table_apply_defaults, called
 * from kdb_table_insert/kdb_table_insert_batch before constraint
 * checking -- existing rows aren't retroactively touched. */
KdbStatus kdb_table_set_default(KdbTable *tbl, const char *col_name, const KdbValue *default_val) {
    if (!tbl || !col_name || !default_val) {
        kdb_err_null_arg("tbl/col_name/default_val", "kdb_table_set_default");
        return KDB_ERR_BAD_ARG;
    }
    if (default_val->type != KDB_TYPE_INT && default_val->type != KDB_TYPE_FLOAT &&
        default_val->type != KDB_TYPE_BOOL && default_val->type != KDB_TYPE_STRING) {
        kdb_set_error(KDB_ERR_BAD_ARG, "DEFAULT's literal must be an INT, FLOAT, BOOL, or STRING value.");
        return KDB_ERR_BAD_ARG;
    }
    KdbColumn *col = NULL;
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0) { col = &tbl->header.columns[i]; break; }
    }
    if (!col) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }

    col->has_default = 1;
    col->default_type = (uint8_t)default_val->type;
    col->default_as_int = 0; col->default_as_float = 0; col->default_as_bool = 0;
    memset(col->default_as_string, 0, sizeof(col->default_as_string));
    switch (default_val->type) {
        case KDB_TYPE_INT:    col->default_as_int   = default_val->v.as_int;   break;
        case KDB_TYPE_FLOAT:  col->default_as_float = default_val->v.as_float; break;
        case KDB_TYPE_BOOL:   col->default_as_bool  = default_val->v.as_bool;  break;
        case KDB_TYPE_STRING: KDB_STRLCPY(col->default_as_string, default_val->v.as_string.data, sizeof(col->default_as_string)); break;
        default: break;
    }

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

/* Clears col_name's DEFAULT. KDB_ERR_NOT_FOUND if it doesn't have one. */
KdbStatus kdb_table_drop_default(KdbTable *tbl, const char *col_name) {
    if (!tbl || !col_name) {
        kdb_err_null_arg("tbl/col_name", "kdb_table_drop_default");
        return KDB_ERR_BAD_ARG;
    }
    KdbColumn *col = NULL;
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        if (strcmp(tbl->header.columns[i].name, col_name) == 0) { col = &tbl->header.columns[i]; break; }
    }
    if (!col) {
        kdb_err_field_not_found(col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }
    if (!col->has_default) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "Column '%s' on table '%s' has no DEFAULT.", col_name, tbl->name);
        return KDB_ERR_NOT_FOUND;
    }
    col->has_default = 0;
    col->default_type = 0;
    col->default_as_int = 0; col->default_as_float = 0; col->default_as_bool = 0;
    memset(col->default_as_string, 0, sizeof(col->default_as_string));

    tbl->dirty = 1;
    return kdb_storage_flush_header(tbl);
}

/* Builds a real KdbValue out of a column's fixed-size DEFAULT literal,
 * same idea as kdb__check_def_to_value. Caller must kdb_value_free() the
 * result. */
static KdbStatus kdb__column_default_to_value(const KdbColumn *col, KdbValue *out) {
    switch ((KdbType)col->default_type) {
        case KDB_TYPE_INT:    return kdb_value_from_int(col->default_as_int, out);
        case KDB_TYPE_FLOAT:  return kdb_value_from_float(col->default_as_float, out);
        case KDB_TYPE_BOOL:   return kdb_value_from_bool(col->default_as_bool, out);
        case KDB_TYPE_STRING: return kdb_value_from_string(col->default_as_string, KDB_TYPE_STRING, out);
        default:              return KDB_ERR_BAD_TYPE;
    }
}

/* Fills in every column's DEFAULT for a field r doesn't have at all --
 * not one it has set to NULL (an explicit NULL means the caller asked for
 * NULL, same as real SQL: DEFAULT only ever fires when a column is
 * omitted outright). Runs before constraint checking (kdb_table_check_
 * insert_constraints) so a DEFAULT can satisfy NOT NULL/CHECK the same
 * way an explicitly-supplied value would. */
static KdbStatus kdb_table_apply_defaults(KdbTable *tbl, KdbRecord *r) {
    for (uint32_t i = 0; i < tbl->header.column_count; i++) {
        const KdbColumn *col = &tbl->header.columns[i];
        if (!col->has_default) continue;
        if (kdb_record_get_field(r, col->name)) continue; /* explicitly supplied (even if NULL) -- leave it alone */

        KdbValue def;
        KdbStatus st = kdb__column_default_to_value(col, &def);
        if (st != KDB_OK) return st;
        st = kdb_record_set_field(r, col->name, &def);
        kdb_value_free(&def);
        if (st != KDB_OK) return st;
    }
    return KDB_OK;
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

    for (uint32_t i = 0; i < tbl->header.n_checks; i++) {
        const KdbCheckDef *def = &tbl->header.checks[i];
        const KdbValue *val = NULL;
        for (uint32_t j = 0; j < r->field_count; j++) {
            if (strcmp(r->fields[j].col_name, def->col_name) == 0) { val = &r->fields[j].value; break; }
        }
        if (!val || val->type == KDB_TYPE_NULL) continue; /* NULL never violates a CHECK, same as real SQL */

        KdbValue lit;
        if (kdb__check_def_to_value(def, &lit) != KDB_OK) return kdb_last_status();
        int ok = kdb_value_matches(val, (KdbOperator)def->op, &lit, NULL);
        kdb_value_free(&lit);
        if (!ok) {
            kdb_set_error(KDB_ERR_VALIDATION,
                "CHECK constraint on column '%s' violated on table '%s'.",
                def->col_name, tbl->name);
            return KDB_ERR_VALIDATION;
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
    int any_enforced = tbl->header.n_checks > 0;
    for (uint32_t i = 0; !any_enforced && i < tbl->header.column_count; i++) {
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

    for (uint32_t i = 0; i < tbl->header.n_checks && result == KDB_OK; i++) {
        const KdbCheckDef *def = &tbl->header.checks[i];

        for (size_t r = 0; r < all.count && result == KDB_OK; r++) {
            KdbRecord *ri = &all.rows[r];
            if (!kdb_query_matches(query, ri)) continue;

            const KdbValue *vi = NULL;
            for (uint32_t k = 0; k < patch->field_count; k++) {
                if (strcmp(patch->fields[k].col_name, def->col_name) == 0) { vi = &patch->fields[k].value; break; }
            }
            if (!vi) {
                for (uint32_t k = 0; k < ri->field_count; k++) {
                    if (strcmp(ri->fields[k].col_name, def->col_name) == 0) { vi = &ri->fields[k].value; break; }
                }
            }
            if (!vi || vi->type == KDB_TYPE_NULL) continue;

            KdbValue lit;
            if (kdb__check_def_to_value(def, &lit) != KDB_OK) { result = kdb_last_status(); break; }
            int ok = kdb_value_matches(vi, (KdbOperator)def->op, &lit, NULL);
            kdb_value_free(&lit);
            if (!ok) {
                kdb_set_error(KDB_ERR_VALIDATION,
                    "CHECK constraint on column '%s' on table '%s' would be violated by this UPDATE (row %llu).",
                    def->col_name, tbl->name, (unsigned long long)ri->id);
                result = KDB_ERR_VALIDATION;
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

    st = kdb_table_apply_defaults(tbl, r);
    if (st != KDB_OK) { kdb_lock_release(&lock); return st; }

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
        st = kdb_table_apply_defaults(tbl, &records[i]);
        if (st != KDB_OK) { kdb_lock_release(&lock); return st; }

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