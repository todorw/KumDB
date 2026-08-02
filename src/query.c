#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/query.h"
#include "../include/error.h"
#include "../include/index.h"
#include "../include/storage.h"


void kdb_query_init(KdbQuery *q) {
    if (!q) return;
    memset(q, 0, sizeof(*q));
    q->group_count = 1; /* one empty AND-group; OR starts additional ones */
}

KdbStatus kdb_query_start_or_group(KdbQuery *q) {
    if (!q) {
        kdb_err_null_arg("q", "kdb_query_start_or_group");
        return KDB_ERR_BAD_ARG;
    }
    if (q->group_count >= KDB_MAX_FILTER_GROUPS) {
        kdb_set_error(KDB_ERR_FULL,
            "Query already has %d OR'd groups. That's the limit. "
            "Simplify your query or split it up.",
            KDB_MAX_FILTER_GROUPS);
        return KDB_ERR_FULL;
    }
    q->group_count++;
    return KDB_OK;
}

KdbStatus kdb_query_add_filter(KdbQuery   *q,
                               const char *key,
                               const char *raw_value,
                               const char *raw_value2) {
    if (!q || !key) {
        kdb_err_null_arg("q/key", "kdb_query_add_filter");
        return KDB_ERR_BAD_ARG;
    }
    KdbFilterGroup *grp = &q->groups[q->group_count - 1];
    if (grp->count >= KDB_MAX_FILTER_KEYS) {
        kdb_set_error(KDB_ERR_FULL,
            "Query already has %d filters in this AND-group. That's the limit. "
            "Simplify your query or split it up.",
            KDB_MAX_FILTER_KEYS);
        return KDB_ERR_FULL;
    }

    KdbFilter *f = &grp->filters[grp->count];
    memset(f, 0, sizeof(*f));

    KdbStatus st = kdb_parse_filter_key(key, f->col_name, &f->op);
    if (st != KDB_OK) return st;


    if (raw_value) {
        /* KDB_OP_IN's value is always a comma-delimited list of tokens to
         * be parsed at match time (see the KDB_OP_IN case in
         * kdb_value_matches, types.c), never a single scalar -- so it must
         * stay KDB_TYPE_STRING even when the whole raw text happens to
         * look like one bare number/bool (a one-element IN list, e.g.
         * "age__in=20"). Letting kdb_type_infer have it in that case would
         * store an INT/BOOL value instead of the list text, and the IN
         * matcher's own type guard would then reject it outright --
         * silently matching nothing regardless of the real comparison. */
        KdbType hint = (f->op == KDB_OP_IN) ? KDB_TYPE_STRING : kdb_type_infer(raw_value);
        st = kdb_value_from_string(raw_value, hint, &f->value);
        if (st != KDB_OK) return st;
    } else {
        kdb_value_from_null(&f->value);
    }


    if (raw_value2 && f->op == KDB_OP_BETWEEN) {
        KdbType hint2 = kdb_type_infer(raw_value2);
        st = kdb_value_from_string(raw_value2, hint2, &f->value2);
        if (st != KDB_OK) {
            kdb_value_free(&f->value);
            return st;
        }
    } else {
        kdb_value_from_null(&f->value2);
    }

    grp->count++;
    return KDB_OK;
}

KdbStatus kdb_query_add_filter_value(KdbQuery       *q,
                                     const char     *col_name,
                                     KdbOperator     op,
                                     const KdbValue *value,
                                     const KdbValue *value2) {
    if (!q || !col_name) {
        kdb_err_null_arg("q/col_name", "kdb_query_add_filter_value");
        return KDB_ERR_BAD_ARG;
    }
    KdbFilterGroup *grp = &q->groups[q->group_count - 1];
    if (grp->count >= KDB_MAX_FILTER_KEYS) {
        kdb_set_error(KDB_ERR_FULL, "Query filter limit reached (%d) in this AND-group.", KDB_MAX_FILTER_KEYS);
        return KDB_ERR_FULL;
    }

    KdbFilter *f = &grp->filters[grp->count];
    memset(f, 0, sizeof(*f));
    KDB_STRLCPY(f->col_name, col_name, KDB_MAX_NAME_LEN);
    f->op = op;

    if (value) {
        KdbStatus st = kdb_value_copy(value, &f->value);
        if (st != KDB_OK) return st;
    } else {
        kdb_value_from_null(&f->value);
    }

    if (value2 && op == KDB_OP_BETWEEN) {
        KdbStatus st = kdb_value_copy(value2, &f->value2);
        if (st != KDB_OK) { kdb_value_free(&f->value); return st; }
    } else {
        kdb_value_from_null(&f->value2);
    }

    grp->count++;
    return KDB_OK;
}

void kdb_query_free(KdbQuery *q) {
    if (!q) return;
    for (uint32_t g = 0; g < q->group_count; g++) {
        KdbFilterGroup *grp = &q->groups[g];
        for (uint32_t i = 0; i < grp->count; i++) {
            kdb_value_free(&grp->filters[i].value);
            kdb_value_free(&grp->filters[i].value2);
        }
        grp->count = 0;
    }
    q->group_count = 0;
}

int kdb_query_is_empty(const KdbQuery *q) {
    return !q || q->group_count == 0 || (q->group_count == 1 && q->groups[0].count == 0);
}


static int kdb__pseudo_column_value(const KdbRecord *r, const char *col_name, KdbValue *out) {
    if (strcmp(col_name, "id") == 0) {
        kdb_value_from_int((int64_t)r->id, out);
        return 1;
    }
    if (strcmp(col_name, "created_at") == 0) {
        kdb_value_from_int((int64_t)r->created_at, out);
        return 1;
    }
    if (strcmp(col_name, "updated_at") == 0) {
        kdb_value_from_int((int64_t)r->updated_at, out);
        return 1;
    }
    return 0;
}

static int kdb__group_matches(const KdbFilterGroup *grp, const KdbRecord *r) {
    for (uint32_t i = 0; i < grp->count; i++) {
        const KdbFilter *f = &grp->filters[i];

        KdbValue pseudo_val;
        const KdbValue *val;
        if (kdb__pseudo_column_value(r, f->col_name, &pseudo_val)) {
            val = &pseudo_val;
        } else {
            const KdbRecordField *field = kdb_record_get_field(r, f->col_name);
            if (!field) {
                if (f->op == KDB_OP_IS_NULL) { continue; }
                return 0;
            }
            val = &field->value;
        }

        if (!kdb_value_matches(val, f->op, &f->value, &f->value2))
            return 0;
    }
    return 1;
}

int kdb_query_matches(const KdbQuery *q, const KdbRecord *r) {
    if (!q || !r) return 0;
    if (r->deleted) return 0;
    if (q->group_count == 0) return 1;

    for (uint32_t g = 0; g < q->group_count; g++) {
        if (kdb__group_matches(&q->groups[g], r)) return 1;
    }
    return 0;
}


KdbStatus kdb_result_init(KdbResult *res, size_t initial_capacity) {
    if (!res) return KDB_ERR_BAD_ARG;
    memset(res, 0, sizeof(*res));
    if (initial_capacity == 0) initial_capacity = 16;

    res->rows = (KdbRecord *)calloc(initial_capacity, sizeof(KdbRecord));
    if (!res->rows) { kdb_err_oom("result set"); return KDB_ERR_OOM; }
    res->capacity = initial_capacity;
    res->count    = 0;
    return KDB_OK;
}

KdbStatus kdb_result_append(KdbResult *res, const KdbRecord *r) {
    if (!res || !r) return KDB_ERR_BAD_ARG;

    if (res->count >= res->capacity) {
        size_t new_cap = res->capacity * 2;
        KdbRecord *new_rows = realloc(res->rows, new_cap * sizeof(KdbRecord));
        if (!new_rows) { kdb_err_oom("result set grow"); return KDB_ERR_OOM; }
        res->rows     = new_rows;
        res->capacity = new_cap;
    }

    KdbRecord *dst = &res->rows[res->count];
    memset(dst, 0, sizeof(*dst));

    
    dst->id          = r->id;
    dst->created_at  = r->created_at;
    dst->updated_at  = r->updated_at;
    dst->deleted     = r->deleted;
    dst->field_count = r->field_count;

    if (r->field_count > 0) {
        dst->fields = (KdbRecordField *)calloc(r->field_count, sizeof(KdbRecordField));
        if (!dst->fields) { kdb_err_oom("result record fields"); dst->field_count = 0; return KDB_ERR_OOM; }

        for (uint32_t i = 0; i < r->field_count; i++) {
            KDB_STRLCPY(dst->fields[i].col_name, r->fields[i].col_name, KDB_MAX_NAME_LEN);
            KdbStatus st = kdb_value_copy(&r->fields[i].value, &dst->fields[i].value);
            if (st != KDB_OK) {
                for (uint32_t j = 0; j < i; j++)
                    kdb_value_free(&dst->fields[j].value);
                free(dst->fields);
                dst->fields      = NULL;
                dst->field_count = 0;
                return st;
            }
        }
    }

    res->count++;
    return KDB_OK;
}

void kdb_result_free(KdbResult *res) {
    if (!res) return;
    if (res->rows) {
        for (size_t i = 0; i < res->count; i++) {
            if (res->rows[i].fields) {
                for (uint32_t j = 0; j < res->rows[i].field_count; j++)
                    kdb_value_free(&res->rows[i].fields[j].value);
                free(res->rows[i].fields);
            }
        }
        free(res->rows);
        res->rows = NULL;
    }
    res->count    = 0;
    res->capacity = 0;
}


static const char *kdb__sort_col    = NULL;
static int         kdb__sort_asc    = 1;

static int kdb__sort_cmp(const void *a, const void *b) {
    const KdbRecord *ra = (const KdbRecord *)a;
    const KdbRecord *rb = (const KdbRecord *)b;

    KdbValue pa, pb;
    if (kdb__pseudo_column_value(ra, kdb__sort_col, &pa) &&
        kdb__pseudo_column_value(rb, kdb__sort_col, &pb)) {
        int cmp = kdb_value_compare(&pa, &pb);
        return kdb__sort_asc ? cmp : -cmp;
    }

    const KdbRecordField *fa = kdb_record_get_field(ra, kdb__sort_col);
    const KdbRecordField *fb = kdb_record_get_field(rb, kdb__sort_col);

    if (!fa && !fb) return 0;
    if (!fa)        return kdb__sort_asc ? -1 :  1;
    if (!fb)        return kdb__sort_asc ?  1 : -1;

    int cmp = kdb_value_compare(&fa->value, &fb->value);
    return kdb__sort_asc ? cmp : -cmp;
}

KdbStatus kdb_result_sort(KdbResult  *res,
                          const char *col_name,
                          int         ascending) {
    if (!res || !col_name) return KDB_ERR_BAD_ARG;
    if (res->count == 0)   return KDB_OK;

    kdb__sort_col = col_name;
    kdb__sort_asc = ascending;
    qsort(res->rows, res->count, sizeof(KdbRecord), kdb__sort_cmp);
    kdb__sort_col = NULL;
    return KDB_OK;
}

void kdb_result_limit(KdbResult *res, size_t max_rows) {
    if (!res || res->count <= max_rows) return;
    for (size_t i = max_rows; i < res->count; i++) {
        if (res->rows[i].fields) {
            for (uint32_t j = 0; j < res->rows[i].field_count; j++)
                kdb_value_free(&res->rows[i].fields[j].value);
            free(res->rows[i].fields);
        }
    }
    res->count = max_rows;
}

void kdb_result_offset(KdbResult *res, size_t offset) {
    if (!res || offset == 0) return;
    if (offset >= res->count) {
        kdb_result_limit(res, 0);
        return;
    }
    
    for (size_t i = 0; i < offset; i++) {
        if (res->rows[i].fields) {
            for (uint32_t j = 0; j < res->rows[i].field_count; j++)
                kdb_value_free(&res->rows[i].fields[j].value);
            free(res->rows[i].fields);
        }
    }
    memmove(res->rows, res->rows + offset, (res->count - offset) * sizeof(KdbRecord));
    res->count -= offset;
}

void kdb_result_print(const KdbResult *res, FILE *fp) {
    if (!res || !fp) return;
    fprintf(fp, "Results: %zu row(s)\n", res->count);
    fprintf(fp, "%-6s  ", "id");
    if (res->count > 0) {
        for (uint32_t j = 0; j < res->rows[0].field_count; j++)
            fprintf(fp, "%-20s  ", res->rows[0].fields[j].col_name);
        fprintf(fp, "\n");
    }
    char vbuf[256];
    for (size_t i = 0; i < res->count; i++) {
        fprintf(fp, "%-6llu  ", (unsigned long long)res->rows[i].id);
        for (uint32_t j = 0; j < res->rows[i].field_count; j++) {
            kdb_value_to_str(&res->rows[i].fields[j].value, vbuf, sizeof(vbuf));
            fprintf(fp, "%-20s  ", vbuf);
        }
        fprintf(fp, "\n");
    }
}


typedef struct {
    const KdbQuery *query;
    KdbResult      *result;
    int             stop_after_one;
} KdbExecCtx;

static int kdb__exec_scan_cb(const KdbRecord *r, void *ud) {
    KdbExecCtx *ctx = (KdbExecCtx *)ud;
    if (!kdb_query_matches(ctx->query, r)) return 1;
    kdb_result_append(ctx->result, r);
    if (ctx->stop_after_one) return 0;
    return 1;
}

KdbStatus kdb_query_execute(KdbTable       *tbl,
                            const KdbQuery *q,
                            KdbResult      *res_out) {
    if (!tbl || !q || !res_out) {
        kdb_err_null_arg("tbl/q/res_out", "kdb_query_execute");
        return KDB_ERR_BAD_ARG;
    }

    KdbStatus st = kdb_result_init(res_out, 16);
    if (st != KDB_OK) return st;

    

    if (q->group_count == 1 && q->groups[0].count == 1 &&
        q->groups[0].filters[0].op == KDB_OP_EQ && tbl->index_count > 0) {
        const KdbFilter *f   = &q->groups[0].filters[0];
        KdbIndex        *idx = kdb_index_find(tbl->indices, tbl->index_count, f->col_name);
        if (idx) {
            uint64_t offsets[KDB_INDEX_BUCKETS];
            size_t   found = 0;
            KdbStatus ist = kdb_index_lookup(idx, &f->value, offsets, KDB_INDEX_BUCKETS, &found);
            if (ist == KDB_OK) {
                for (size_t i = 0; i < found; i++) {
                    KdbRecord *r = kdb_storage_read_at(tbl, offsets[i]);
                    if (!r) continue;
                    if (kdb_query_matches(q, r))
                        kdb_result_append(res_out, r);
                    kdb_record_free(r);
                }
                return KDB_OK;
            }
            
        }
    }

    
    KdbExecCtx ctx = { .query = q, .result = res_out, .stop_after_one = 0 };
    return kdb_storage_scan(tbl, kdb__exec_scan_cb, &ctx);
}

KdbStatus kdb_query_execute_one(KdbTable       *tbl,
                                const KdbQuery *q,
                                KdbResult      *res_out) {
    if (!tbl || !q || !res_out) {
        kdb_err_null_arg("tbl/q/res_out", "kdb_query_execute_one");
        return KDB_ERR_BAD_ARG;
    }

    KdbStatus st = kdb_result_init(res_out, 1);
    if (st != KDB_OK) return st;

    KdbExecCtx ctx = { .query = q, .result = res_out, .stop_after_one = 1 };
    st = kdb_storage_scan(tbl, kdb__exec_scan_cb, &ctx);
    if (st != KDB_OK) return st;

    if (res_out->count == 0) {
        kdb_set_error(KDB_ERR_NOT_FOUND,
            "No record matched your query. It either doesn't exist or you got the filters wrong.");
        return KDB_ERR_NOT_FOUND;
    }
    return KDB_OK;
}

typedef struct { const KdbQuery *q; size_t count; } KdbCountCtx;

static int kdb__count_cb(const KdbRecord *r, void *ud) {
    KdbCountCtx *ctx = (KdbCountCtx *)ud;
    if (kdb_query_matches(ctx->q, r)) ctx->count++;
    return 1;
}

KdbStatus kdb_query_count(KdbTable       *tbl,
                          const KdbQuery *q,
                          size_t         *count_out) {
    if (!tbl || !q || !count_out) {
        kdb_err_null_arg("tbl/q/count_out", "kdb_query_count");
        return KDB_ERR_BAD_ARG;
    }
    *count_out = 0;
    KdbCountCtx ctx = { .q = q, .count = 0 };
    KdbStatus st = kdb_storage_scan(tbl, kdb__count_cb, &ctx);
    if (st == KDB_OK) *count_out = ctx.count;
    return st;
}


void kdb_query_print(const KdbQuery *q, FILE *fp) {
    if (!q || !fp) return;
    fprintf(fp, "Query (%u OR'd group(s)):\n", q->group_count);
    char vbuf[256];
    for (uint32_t g = 0; g < q->group_count; g++) {
        const KdbFilterGroup *grp = &q->groups[g];
        if (g > 0) fprintf(fp, "  OR\n");
        for (uint32_t i = 0; i < grp->count; i++) {
            const KdbFilter *f = &grp->filters[i];
            kdb_value_to_str(&f->value, vbuf, sizeof(vbuf));
            fprintf(fp, "  [%u.%u] %s __%s %s", g, i, f->col_name, kdb_op_name(f->op), vbuf);
            if (f->op == KDB_OP_BETWEEN) {
                char vbuf2[256];
                kdb_value_to_str(&f->value2, vbuf2, sizeof(vbuf2));
                fprintf(fp, " AND %s", vbuf2);
            }
            fprintf(fp, "\n");
        }
    }
}