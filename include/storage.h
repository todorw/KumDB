#ifndef KUMDB_STORAGE_H
#define KUMDB_STORAGE_H

#include "internal.h"
#include "record.h"
#include "lock.h"

KdbStatus kdb_storage_create(const char       *data_dir,
                             const char       *table_name,
                             const KdbColumn  *columns,
                             uint32_t          column_count);

KdbStatus kdb_storage_open(KdbTable   *tbl,
                           const char *data_dir,
                           const char *table_name);

KdbStatus kdb_storage_flush_header(KdbTable *tbl);

void kdb_storage_close(KdbTable *tbl);

KdbStatus kdb_storage_drop(const char *data_dir, const char *table_name);

int kdb_storage_exists(const char *data_dir, const char *table_name);

KdbStatus kdb_storage_append(KdbTable *tbl, KdbRecord *r);

KdbRecord *kdb_storage_read_at(KdbTable *tbl, uint64_t file_offset);

typedef int (*KdbScanCallback)(const KdbRecord *r, void *user_data);

KdbStatus kdb_storage_scan(KdbTable      *tbl,
                           KdbScanCallback callback,
                           void           *user_data);

/* Return 1 to keep the record, 0 to drop it (soft-delete/skip), or a
 * negative value to abort the whole rewrite -- the original table file is
 * left completely untouched (the half-written replacement is discarded),
 * and kdb_storage_rewrite returns whatever status/message the transform
 * itself already set via kdb_set_error before returning negative (e.g.
 * kdb_table_alter_column_type aborting because one existing value can't
 * convert to the new type). */
typedef int (*KdbTransformFn)(KdbRecord *r, void *user_data);

KdbStatus kdb_storage_rewrite(KdbTable      *tbl,
                              KdbTransformFn transform_fn,
                              void          *user_data);

KdbStatus kdb_storage_compact(KdbTable *tbl);

KdbStatus kdb_storage_append_batch(KdbTable        *tbl,
                                   KdbRecord       *records,
                                   size_t           count);

void kdb_storage_path(const char *data_dir,
                      const char *table_name,
                      char       *out_buf,
                      size_t      out_size);

KdbStatus kdb_storage_list_tables(const char *data_dir,
                                  char        names_out[][KDB_MAX_NAME_LEN],
                                  uint32_t   *count_out);

KdbStatus kdb_storage_validate_header(const KdbTableHeader *hdr,
                                      const char           *path);

typedef struct {
    uint64_t file_size_bytes;
    uint64_t record_count;
    uint64_t deleted_count;
    uint64_t live_count;
    double   fragmentation_ratio;  
} KdbStorageStats;

KdbStatus kdb_storage_stats(KdbTable *tbl, KdbStorageStats *out);

#endif 