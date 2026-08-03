#ifndef KUMDB_INDEX_H
#define KUMDB_INDEX_H

#include "internal.h"
#include "record.h"


KdbIndex *kdb_index_new(const char *col_name);

/* A real composite (multi-column) index over col_names[0..n_cols-1], in
 * that order (n_cols must be between 1 and KDB_MAX_COMPOSITE_COLS; n_cols
 * ==1 behaves exactly like kdb_index_new). Every kdb_index_insert/
 * kdb_index_rebuild call against the result automatically hashes all
 * n_cols columns' values together instead of just col_name's -- no
 * separate "composite" insert path to call. */
KdbIndex *kdb_index_new_composite(const char **col_names, uint32_t n_cols);


void kdb_index_free(KdbIndex *idx);


void kdb_index_free_array(KdbIndex **indices, uint32_t count);


KdbStatus kdb_index_insert(KdbIndex       *idx,
                           const KdbRecord *r,
                           uint64_t         file_offset);


KdbStatus kdb_index_remove(KdbIndex *idx, uint64_t record_id);


KdbStatus kdb_index_rebuild(KdbIndex *idx, KdbTable *tbl);


KdbStatus kdb_index_lookup(const KdbIndex *idx,
                           const KdbValue *value,
                           uint64_t       *file_offsets_out,
                           size_t          max_results,
                           size_t         *count_out);

/* Same as kdb_index_lookup, but for a composite index -- values[0..n-1]
 * must line up with idx's own column order (n must equal 1 + idx's
 * n_extra_cols). Works on a single-column index too (n==1), identical to
 * calling kdb_index_lookup directly. */
KdbStatus kdb_index_lookup_multi(const KdbIndex *idx,
                                 const KdbValue *values,
                                 size_t          n,
                                 uint64_t       *file_offsets_out,
                                 size_t          max_results,
                                 size_t         *count_out);


uint64_t kdb_index_lookup_one(const KdbIndex *idx, const KdbValue *value);


KdbStatus kdb_index_build_for_table(const KdbColumn *columns,
                                    uint32_t         column_count,
                                    KdbIndex       **indices_out,
                                    uint32_t        *count_out);

/* Builds one KdbIndex per composite index definition in defs[0..n_defs-1]
 * (resolving each definition's column positions against columns[]),
 * appending to indices_out starting at *count_out (so this can run right
 * after kdb_index_build_for_table without clobbering its single-column
 * indices -- *count_out is both read and updated). */
KdbStatus kdb_index_build_composite_for_table(const KdbColumn             *columns,
                                              const KdbCompositeIndexDef *defs,
                                              uint8_t                     n_defs,
                                              KdbIndex                  **indices_out,
                                              uint32_t                   *count_out);


KdbIndex *kdb_index_find(KdbIndex **indices,
                         uint32_t   count,
                         const char *col_name);

/* Finds a composite index whose column set exactly matches col_names[0..
 * n_cols-1] (same columns, order doesn't matter) -- NULL if none does.
 * Also matches a single-column index when n_cols==1. */
KdbIndex *kdb_index_find_composite(KdbIndex   **indices,
                                   uint32_t     count,
                                   const char **col_names,
                                   uint32_t     n_cols);


uint32_t kdb_index_hash(const KdbValue *v);

/* Combines values[0..n-1] into one hash bucket, same FNV-mix kdb_index_hash
 * uses for a single value but continued across all n values in order --
 * kdb_index_hash(v) is exactly kdb_index_hash_multi(v, 1). */
uint32_t kdb_index_hash_multi(const KdbValue *values, size_t n);


typedef struct {
    size_t   entry_count;
    size_t   bucket_count;
    size_t   collision_count;
    double   load_factor;
    size_t   longest_chain;
} KdbIndexStats;

void kdb_index_stats(const KdbIndex *idx, KdbIndexStats *out);

#endif