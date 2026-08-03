#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/index.h"
#include "../include/storage.h"
#include "../include/error.h"
#include "../include/types.h"


/* Mixes one value's bytes into *hash (FNV-1a-style) -- shared by
 * kdb_index_hash_multi so a single-value hash and a composite hash use
 * exactly the same per-value mixing, just continued across more than one
 * value for a composite index. */
static void kdb__index_hash_mix(uint32_t *hash, const KdbValue *v) {
#define FNV_MIX(byte) *hash ^= (uint8_t)(byte); *hash *= 16777619u

    switch (v->type) {
        case KDB_TYPE_INT: {
            uint64_t n = (uint64_t)v->v.as_int;
            for (int i = 0; i < 8; i++) { FNV_MIX(n); n >>= 8; }
            break;
        }
        case KDB_TYPE_FLOAT: {
            uint64_t bits;
            memcpy(&bits, &v->v.as_float, 8);
            for (int i = 0; i < 8; i++) { FNV_MIX(bits); bits >>= 8; }
            break;
        }
        case KDB_TYPE_BOOL:
            FNV_MIX(v->v.as_bool);
            break;
        case KDB_TYPE_STRING:
            if (v->v.as_string.data) {
                const char *p = v->v.as_string.data;
                while (*p) { FNV_MIX(*p); p++; }
            }
            break;
        case KDB_TYPE_BLOB:
            if (v->v.as_blob.data) {
                const uint8_t *p = v->v.as_blob.data;
                for (size_t i = 0; i < v->v.as_blob.len; i++) { FNV_MIX(p[i]); }
            }
            break;
        case KDB_TYPE_NULL:
        default:
            FNV_MIX(0xff);
            break;
    }
#undef FNV_MIX
}

uint32_t kdb_index_hash_multi(const KdbValue *values, size_t n) {
    if (!values || n == 0) return 0; /* same "nothing to hash" convention kdb_index_hash always had */
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < n; i++) kdb__index_hash_mix(&hash, &values[i]);
    return hash % KDB_INDEX_BUCKETS;
}

uint32_t kdb_index_hash(const KdbValue *v) {
    return kdb_index_hash_multi(v, v ? 1 : 0);
}


KdbIndex *kdb_index_new(const char *col_name) {
    KdbIndex *idx = (KdbIndex *)calloc(1, sizeof(KdbIndex));
    if (!idx) { kdb_err_oom("KdbIndex"); return NULL; }
    if (col_name)
        KDB_STRLCPY(idx->col_name, col_name, KDB_MAX_NAME_LEN);

    return idx;
}

KdbIndex *kdb_index_new_composite(const char **col_names, uint32_t n_cols) {
    if (!col_names || n_cols == 0 || n_cols > KDB_MAX_COMPOSITE_COLS) {
        kdb_err_bad_arg("col_names/n_cols", "kdb_index_new_composite needs 1..KDB_MAX_COMPOSITE_COLS columns");
        return NULL;
    }
    KdbIndex *idx = kdb_index_new(col_names[0]);
    if (!idx) return NULL;
    for (uint32_t i = 1; i < n_cols; i++) {
        KDB_STRLCPY(idx->extra_cols[i - 1], col_names[i], KDB_MAX_NAME_LEN);
    }
    idx->n_extra_cols = n_cols - 1;
    return idx;
}

void kdb_index_free(KdbIndex *idx) {
    if (!idx) return;
    for (uint32_t i = 0; i < KDB_INDEX_BUCKETS; i++) {
        KdbIndexNode *node = idx->buckets[i];
        while (node) {
            KdbIndexNode *next = node->next;
            free(node);
            node = next;
        }
        idx->buckets[i] = NULL;
    }
    free(idx);
}

void kdb_index_free_array(KdbIndex **indices, uint32_t count) {
    if (!indices) return;
    for (uint32_t i = 0; i < count; i++)
        kdb_index_free(indices[i]);
    free(indices);
}


KdbStatus kdb_index_insert(KdbIndex       *idx,
                           const KdbRecord *r,
                           uint64_t         file_offset) {
    if (!idx || !r) {
        kdb_err_null_arg("idx/r", "kdb_index_insert");
        return KDB_ERR_BAD_ARG;
    }

    /* gather every column this index covers -- col_name plus, for a
     * composite index, extra_cols in order. A record missing any one of
     * them isn't indexed at all (same "missing = no entry" convention the
     * single-column case always had), not partially indexed on whatever
     * it does have. */
    KdbValue vals[KDB_MAX_COMPOSITE_COLS];
    const KdbRecordField *f = kdb_record_get_field(r, idx->col_name);
    if (!f) return KDB_OK;
    vals[0] = f->value;
    for (uint32_t i = 0; i < idx->n_extra_cols; i++) {
        const KdbRecordField *ef = kdb_record_get_field(r, idx->extra_cols[i]);
        if (!ef) return KDB_OK;
        vals[i + 1] = ef->value;
    }

    uint32_t bucket = kdb_index_hash_multi(vals, (size_t)idx->n_extra_cols + 1);

    KdbIndexNode *node = (KdbIndexNode *)calloc(1, sizeof(KdbIndexNode));
    if (!node) { kdb_err_oom("KdbIndexNode"); return KDB_ERR_OOM; }

    node->record_id   = r->id;
    node->file_offset = file_offset;

    
    node->next         = idx->buckets[bucket];
    idx->buckets[bucket] = node;

    return KDB_OK;
}


KdbStatus kdb_index_remove(KdbIndex *idx, uint64_t record_id) {
    if (!idx) {
        kdb_err_null_arg("idx", "kdb_index_remove");
        return KDB_ERR_BAD_ARG;
    }

    for (uint32_t i = 0; i < KDB_INDEX_BUCKETS; i++) {
        KdbIndexNode **pp = &idx->buckets[i];
        while (*pp) {
            if ((*pp)->record_id == record_id) {
                KdbIndexNode *to_free = *pp;
                *pp = to_free->next;
                free(to_free);
                return KDB_OK;
            }
            pp = &(*pp)->next;
        }
    }

    kdb_err_record_not_found(record_id, idx->col_name);
    return KDB_ERR_NOT_FOUND;
}


typedef struct {
    KdbIndex *idx;
    uint64_t  file_offset;
    FILE     *fp;
    uint64_t  data_offset;
} KdbRebuildCtx;

static int kdb__rebuild_cb(const KdbRecord *r, void *ud) {
    KdbRebuildCtx *ctx = (KdbRebuildCtx *)ud;
    

    kdb_index_insert(ctx->idx, r, ctx->file_offset);
    

    ctx->file_offset += 4 + kdb_record_serial_size(r);
    return 1;
}

KdbStatus kdb_index_rebuild(KdbIndex *idx, KdbTable *tbl) {
    if (!idx || !tbl) {
        kdb_err_null_arg("idx/tbl", "kdb_index_rebuild");
        return KDB_ERR_BAD_ARG;
    }

    
    for (uint32_t i = 0; i < KDB_INDEX_BUCKETS; i++) {
        KdbIndexNode *node = idx->buckets[i];
        while (node) {
            KdbIndexNode *next = node->next;
            free(node);
            node = next;
        }
        idx->buckets[i] = NULL;
    }

    KdbRebuildCtx ctx = {
        .idx         = idx,
        .file_offset = tbl->header.data_offset,
        .fp          = tbl->fp,
        .data_offset = tbl->header.data_offset
    };

    return kdb_storage_scan(tbl, kdb__rebuild_cb, &ctx);
}


KdbStatus kdb_index_lookup_multi(const KdbIndex *idx,
                                 const KdbValue *values,
                                 size_t          n,
                                 uint64_t       *file_offsets_out,
                                 size_t          max_results,
                                 size_t         *count_out) {
    if (!idx || !values || n == 0 || !file_offsets_out || !count_out) {
        kdb_err_null_arg("idx/values/file_offsets_out/count_out", "kdb_index_lookup_multi");
        return KDB_ERR_BAD_ARG;
    }

    *count_out = 0;
    uint32_t bucket = kdb_index_hash_multi(values, n);

    const KdbIndexNode *node = idx->buckets[bucket];
    while (node && *count_out < max_results) {
        file_offsets_out[(*count_out)++] = node->file_offset;
        node = node->next;
    }

    if (*count_out == 0) {
        kdb_set_error(KDB_ERR_NOT_FOUND, "No index entries found for value in column '%s'.",
                      idx->col_name);
        return KDB_ERR_NOT_FOUND;
    }
    return KDB_OK;
}

KdbStatus kdb_index_lookup(const KdbIndex *idx,
                           const KdbValue *value,
                           uint64_t       *file_offsets_out,
                           size_t          max_results,
                           size_t         *count_out) {
    if (!value) { kdb_err_null_arg("value", "kdb_index_lookup"); return KDB_ERR_BAD_ARG; }
    return kdb_index_lookup_multi(idx, value, 1, file_offsets_out, max_results, count_out);
}

uint64_t kdb_index_lookup_one(const KdbIndex *idx, const KdbValue *value) {
    if (!idx || !value) return UINT64_MAX;
    uint64_t offset = 0;
    size_t   count  = 0;
    KdbStatus st = kdb_index_lookup(idx, value, &offset, 1, &count);
    return (st == KDB_OK && count > 0) ? offset : UINT64_MAX;
}


KdbStatus kdb_index_build_for_table(const KdbColumn *columns,
                                    uint32_t         column_count,
                                    KdbIndex       **indices_out,
                                    uint32_t        *count_out) {
    if (!columns || !indices_out || !count_out) {
        kdb_err_null_arg("columns/indices_out/count_out", "kdb_index_build_for_table");
        return KDB_ERR_BAD_ARG;
    }

    *count_out = 0;
    for (uint32_t i = 0; i < column_count; i++) {
        if (!columns[i].indexed) continue;
        KdbIndex *idx = kdb_index_new(columns[i].name);
        if (!idx) return KDB_ERR_OOM;
        indices_out[(*count_out)++] = idx;
    }
    return KDB_OK;
}

KdbStatus kdb_index_build_composite_for_table(const KdbColumn             *columns,
                                              const KdbCompositeIndexDef *defs,
                                              uint8_t                     n_defs,
                                              KdbIndex                  **indices_out,
                                              uint32_t                   *count_out) {
    if (!columns || !indices_out || !count_out) {
        kdb_err_null_arg("columns/indices_out/count_out", "kdb_index_build_composite_for_table");
        return KDB_ERR_BAD_ARG;
    }
    if (!defs) return KDB_OK; /* nothing to build */

    for (uint8_t d = 0; d < n_defs; d++) {
        if (defs[d].n_cols == 0) continue; /* unused slot */
        const char *names[KDB_MAX_COMPOSITE_COLS];
        for (uint8_t c = 0; c < defs[d].n_cols; c++) names[c] = columns[defs[d].col_positions[c]].name;
        KdbIndex *idx = kdb_index_new_composite(names, defs[d].n_cols);
        if (!idx) return KDB_ERR_OOM;
        indices_out[(*count_out)++] = idx;
    }
    return KDB_OK;
}

KdbIndex *kdb_index_find(KdbIndex **indices,
                         uint32_t   count,
                         const char *col_name) {
    if (!indices || !col_name) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (indices[i] && indices[i]->n_extra_cols == 0 && strcmp(indices[i]->col_name, col_name) == 0)
            return indices[i];
    }
    return NULL;
}

KdbIndex *kdb_index_find_composite(KdbIndex   **indices,
                                   uint32_t     count,
                                   const char **col_names,
                                   uint32_t     n_cols) {
    if (!indices || !col_names || n_cols == 0) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        KdbIndex *idx = indices[i];
        if (!idx || idx->n_extra_cols + 1 != n_cols) continue;

        int all_found = 1;
        for (uint32_t a = 0; a < n_cols && all_found; a++) {
            int found = (strcmp(idx->col_name, col_names[a]) == 0);
            for (uint32_t e = 0; !found && e < idx->n_extra_cols; e++) {
                if (strcmp(idx->extra_cols[e], col_names[a]) == 0) found = 1;
            }
            if (!found) all_found = 0;
        }
        if (all_found) return idx;
    }
    return NULL;
}


void kdb_index_stats(const KdbIndex *idx, KdbIndexStats *out) {
    if (!idx || !out) return;
    memset(out, 0, sizeof(*out));
    out->bucket_count = KDB_INDEX_BUCKETS;

    for (uint32_t i = 0; i < KDB_INDEX_BUCKETS; i++) {
        const KdbIndexNode *node = idx->buckets[i];
        if (!node) continue;
        size_t chain_len = 0;
        while (node) {
            out->entry_count++;
            chain_len++;
            node = node->next;
        }
        if (chain_len > 1)
            out->collision_count += chain_len - 1;
        if (chain_len > out->longest_chain)
            out->longest_chain = chain_len;
    }

    out->load_factor = (double)out->entry_count / (double)KDB_INDEX_BUCKETS;
}