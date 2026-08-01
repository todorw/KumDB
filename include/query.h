#ifndef KUMDB_QUERY_H
#define KUMDB_QUERY_H

#include "internal.h"
#include "record.h"
#include "types.h"
#include "table.h"

void kdb_query_init(KdbQuery *q);

/* Starts a new AND-group, OR'd against whatever groups came before it.
 * Filters added after this call land in the new group until the next
 * call (if any). Same precedence as SQL: AND binds tighter than OR. */
KdbStatus kdb_query_start_or_group(KdbQuery *q);

KdbStatus kdb_query_add_filter(KdbQuery   *q,
                               const char *key,
                               const char *raw_value,
                               const char *raw_value2);

KdbStatus kdb_query_add_filter_value(KdbQuery       *q,
                                     const char     *col_name,
                                     KdbOperator     op,
                                     const KdbValue *value,
                                     const KdbValue *value2);

void kdb_query_free(KdbQuery *q);

int kdb_query_is_empty(const KdbQuery *q);

int kdb_query_matches(const KdbQuery *q, const KdbRecord *r);

KdbStatus kdb_result_init(KdbResult *res, size_t initial_capacity);

KdbStatus kdb_result_append(KdbResult *res, const KdbRecord *r);

void kdb_result_free(KdbResult *res);

KdbStatus kdb_result_sort(KdbResult  *res,
                          const char *col_name,
                          int         ascending);

void kdb_result_limit(KdbResult *res, size_t max_rows);

void kdb_result_offset(KdbResult *res, size_t offset);

void kdb_result_print(const KdbResult *res, FILE *fp);

KdbStatus kdb_query_execute(KdbTable        *tbl,
                            const KdbQuery  *q,
                            KdbResult       *res_out);

KdbStatus kdb_query_execute_one(KdbTable       *tbl,
                                const KdbQuery *q,
                                KdbResult      *res_out);

KdbStatus kdb_query_count(KdbTable       *tbl,
                          const KdbQuery *q,
                          size_t         *count_out);

void kdb_query_print(const KdbQuery *q, FILE *fp);

#endif 