#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

#include "../include/storage.h"
#include "../include/error.h"
#include "../include/lock.h"


void kdb_storage_path(const char *data_dir,
                      const char *table_name,
                      char       *out_buf,
                      size_t      out_size) {
    snprintf(out_buf, out_size, "%s/%s.kdb", data_dir, table_name);
}

static void kdb__ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
    }
}

static void kdb__fsync_parent_dir(const char *path) {
    char dir_path[4096];
    KDB_STRLCPY(dir_path, path, sizeof(dir_path));

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        dir_path[0] = '.'; dir_path[1] = '\0';
    }

    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
}


static KdbStatus kdb__write_header(FILE *fp, const KdbTableHeader *hdr) {
    rewind(fp);
    if (fwrite(hdr, sizeof(*hdr), 1, fp) != 1) return KDB_ERR_IO;
    if (fflush(fp) != 0) return KDB_ERR_IO;
    if (fsync(fileno(fp)) != 0) return KDB_ERR_IO;
    return KDB_OK;
}

static KdbStatus kdb__read_header(FILE *fp, KdbTableHeader *hdr) {
    rewind(fp);
    if (fread(hdr, sizeof(*hdr), 1, fp) != 1) return KDB_ERR_IO;
    return KDB_OK;
}


KdbStatus kdb_storage_validate_header(const KdbTableHeader *hdr,
                                      const char           *path) {
    if (hdr->magic != KDB_MAGIC) {
        kdb_err_io_corrupt(path);
        return KDB_ERR_CORRUPT;
    }
    if (hdr->version_major != KDB_VERSION_MAJOR) {
        kdb_set_error(KDB_ERR_CORRUPT,
            "File '%s' was written by KumDB v%d.x but this is v%d.x. "
            "Versions are incompatible. Upgrade or migrate your data.",
            path, hdr->version_major, KDB_VERSION_MAJOR);
        return KDB_ERR_CORRUPT;
    }
    return KDB_OK;
}


int kdb_storage_exists(const char *data_dir, const char *table_name) {
    char path[4096];
    kdb_storage_path(data_dir, table_name, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

KdbStatus kdb_storage_create(const char      *data_dir,
                             const char      *table_name,
                             const KdbColumn *columns,
                             uint32_t         column_count) {
    if (!data_dir || !table_name) {
        kdb_err_null_arg("data_dir/table_name", "kdb_storage_create");
        return KDB_ERR_BAD_ARG;
    }
    if (column_count > KDB_MAX_COLUMNS) {
        kdb_err_bad_arg("column_count", "exceeds KDB_MAX_COLUMNS");
        return KDB_ERR_FULL;
    }

    kdb__ensure_dir(data_dir);

    char path[4096];
    kdb_storage_path(data_dir, table_name, path, sizeof(path));

    if (kdb_storage_exists(data_dir, table_name)) {
        kdb_err_table_exists(table_name);
        return KDB_ERR_EXISTS;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) { kdb_err_io(path, "fopen create"); return KDB_ERR_IO; }

    KdbTableHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic         = KDB_MAGIC;
    hdr.version_major = KDB_VERSION_MAJOR;
    hdr.version_minor = KDB_VERSION_MINOR;
    hdr.version_patch = KDB_VERSION_PATCH;
    KDB_STRLCPY(hdr.table_name, table_name, KDB_MAX_NAME_LEN);
    hdr.column_count  = column_count;
    hdr.record_count  = 0;
    hdr.next_id       = 1;
    hdr.created_at    = (uint64_t)time(NULL);
    hdr.updated_at    = hdr.created_at;
    hdr.data_offset   = sizeof(KdbTableHeader);
    hdr.index_offset  = 0;

    if (columns && column_count > 0)
        memcpy(hdr.columns, columns, column_count * sizeof(KdbColumn));

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        unlink(path);
        kdb_err_io(path, "write header");
        return KDB_ERR_IO;
    }

    fclose(fp);
    return KDB_OK;
}

KdbStatus kdb_storage_open(KdbTable   *tbl,
                           const char *data_dir,
                           const char *table_name) {
    if (!tbl || !data_dir || !table_name) {
        kdb_err_null_arg("tbl/data_dir/table_name", "kdb_storage_open");
        return KDB_ERR_BAD_ARG;
    }

    memset(tbl, 0, sizeof(*tbl));
    tbl->lock_fd = -1;

    kdb_storage_path(data_dir, table_name, tbl->path, sizeof(tbl->path));

    if (!kdb_storage_exists(data_dir, table_name)) {
        kdb_err_table_not_found(table_name);
        return KDB_ERR_NOT_FOUND;
    }

    tbl->fp = fopen(tbl->path, "r+b");
    if (!tbl->fp) {
        kdb_err_io(tbl->path, "fopen");
        return KDB_ERR_IO;
    }

    KdbStatus st = kdb__read_header(tbl->fp, &tbl->header);
    if (st != KDB_OK) {
        fclose(tbl->fp); tbl->fp = NULL;
        kdb_err_io(tbl->path, "read header");
        return st;
    }

    st = kdb_storage_validate_header(&tbl->header, tbl->path);
    if (st != KDB_OK) {
        fclose(tbl->fp); tbl->fp = NULL;
        return st;
    }

    KDB_STRLCPY(tbl->name, table_name, KDB_MAX_NAME_LEN);
    return KDB_OK;
}

KdbStatus kdb_storage_flush_header(KdbTable *tbl) {
    if (!tbl || !tbl->fp) return KDB_ERR_BAD_ARG;
    tbl->header.updated_at = (uint64_t)time(NULL);
    KdbStatus st = kdb__write_header(tbl->fp, &tbl->header);
    if (st == KDB_OK) tbl->dirty = 0;
    else kdb_err_io(tbl->path, "flush header");
    return st;
}

void kdb_storage_close(KdbTable *tbl) {
    if (!tbl) return;
    if (tbl->fp) {
        if (tbl->dirty) kdb_storage_flush_header(tbl);
        fclose(tbl->fp);
        tbl->fp = NULL;
    }
}

KdbStatus kdb_storage_drop(const char *data_dir, const char *table_name) {
    char path[4096], lock_path[4096];
    kdb_storage_path(data_dir, table_name, path, sizeof(path));
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);

    if (unlink(path) != 0 && errno != ENOENT) {
        kdb_err_io(path, "unlink");
        return KDB_ERR_IO;
    }
    unlink(lock_path); 
    return KDB_OK;
}


KdbStatus kdb_storage_append(KdbTable *tbl, KdbRecord *r) {
    if (!tbl || !r) {
        kdb_err_null_arg("tbl/r", "kdb_storage_append");
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->read_only) {
        kdb_err_table_read_only(tbl->name);
        return KDB_ERR_READ_ONLY;
    }
    if (tbl->header.record_count >= KDB_MAX_RECORDS) {
        kdb_err_table_full(tbl->name);
        return KDB_ERR_FULL;
    }

    
    r->id = tbl->header.next_id++;

    
    if (fseek(tbl->fp, 0, SEEK_END) != 0) {
        kdb_err_io(tbl->path, "fseek end");
        return KDB_ERR_IO;
    }

    KdbStatus st = kdb_record_write(r, tbl->fp);
    if (st != KDB_OK) return st;

    tbl->header.record_count++;
    tbl->dirty = 1;
    return KDB_OK;
}

KdbRecord *kdb_storage_read_at(KdbTable *tbl, uint64_t file_offset) {
    if (!tbl || !tbl->fp) return NULL;
    if (fseek(tbl->fp, (long)file_offset, SEEK_SET) != 0) {
        kdb_err_io(tbl->path, "fseek read_at");
        return NULL;
    }
    return kdb_record_read(tbl->fp);
}


KdbStatus kdb_storage_scan(KdbTable       *tbl,
                           KdbScanCallback callback,
                           void           *user_data) {
    if (!tbl || !tbl->fp || !callback) {
        kdb_err_null_arg("tbl/callback", "kdb_storage_scan");
        return KDB_ERR_BAD_ARG;
    }

    if (fseek(tbl->fp, (long)tbl->header.data_offset, SEEK_SET) != 0) {
        kdb_err_io(tbl->path, "fseek scan start");
        return KDB_ERR_IO;
    }

    KdbRecord *r;
    while ((r = kdb_record_read(tbl->fp)) != NULL) {
        int cont = 1;
        if (!r->deleted) {
            cont = callback(r, user_data);
        }
        kdb_record_free(r);
        if (!cont) break;
    }

    return KDB_OK;
}


typedef struct {
    FILE          *out_fp;
    KdbTransformFn transform_fn;
    void          *user_data;
    uint64_t       record_count;
    uint64_t       next_id;
    int            aborted;
} KdbRewriteCtx;

static int kdb__rewrite_scan_cb(const KdbRecord *r_const, void *ud) {
    KdbRewriteCtx *ctx = (KdbRewriteCtx *)ud;


    KdbRecord *r = kdb_record_copy(r_const);
    if (!r) { ctx->aborted = 1; return 0; }

    int keep = ctx->transform_fn(r, ctx->user_data);
    if (keep && !r->deleted) {
        if (kdb_record_write(r, ctx->out_fp) != KDB_OK) {
            ctx->aborted = 1;
            kdb_record_free(r);
            return 0;
        }
        ctx->record_count++;
        if (r->id >= ctx->next_id) ctx->next_id = r->id + 1;
    }
    kdb_record_free(r);
    return 1;
}

KdbStatus kdb_storage_rewrite(KdbTable      *tbl,
                              KdbTransformFn transform_fn,
                              void          *user_data) {
    if (!tbl || !transform_fn) {
        kdb_err_null_arg("tbl/transform_fn", "kdb_storage_rewrite");
        return KDB_ERR_BAD_ARG;
    }
    if (tbl->read_only) {
        kdb_err_table_read_only(tbl->name);
        return KDB_ERR_READ_ONLY;
    }

    
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", tbl->path);

    FILE *out_fp = fopen(tmp_path, "wb");
    if (!out_fp) {
        kdb_err_io(tmp_path, "fopen rewrite temp");
        return KDB_ERR_IO;
    }

    
    KdbTableHeader new_hdr;
    memcpy(&new_hdr, &tbl->header, sizeof(new_hdr));
    new_hdr.record_count = 0;
    new_hdr.updated_at   = (uint64_t)time(NULL);
    if (fwrite(&new_hdr, sizeof(new_hdr), 1, out_fp) != 1) {
        fclose(out_fp); unlink(tmp_path);
        kdb_err_io(tmp_path, "write header placeholder");
        return KDB_ERR_IO;
    }


    KdbRewriteCtx ctx = {
        .out_fp       = out_fp,
        .transform_fn = transform_fn,
        .user_data    = user_data,
        .record_count = 0,
        .next_id      = tbl->header.next_id,
        .aborted      = 0
    };

    KdbStatus scan_st = kdb_storage_scan(tbl, kdb__rewrite_scan_cb, &ctx);
    if (scan_st != KDB_OK) {
        fclose(out_fp); unlink(tmp_path);
        return scan_st;
    }
    if (ctx.aborted) {

        fclose(out_fp); unlink(tmp_path);
        kdb_err_oom("rewrite: failed to copy/write a record mid-compaction, original table left untouched");
        return KDB_ERR_OOM;
    }

    
    new_hdr.record_count = ctx.record_count;
    new_hdr.next_id      = ctx.next_id;
    rewind(out_fp);
    if (fwrite(&new_hdr, sizeof(new_hdr), 1, out_fp) != 1) {
        fclose(out_fp); unlink(tmp_path);
        kdb_err_io(tmp_path, "patch header");
        return KDB_ERR_IO;
    }

    
    if (fflush(out_fp) != 0 || fsync(fileno(out_fp)) != 0) {
        fclose(out_fp); unlink(tmp_path);
        kdb_err_io(tmp_path, "fsync rewrite");
        return KDB_ERR_IO;
    }
    fclose(out_fp);

    
    fclose(tbl->fp);
    tbl->fp = NULL;

    
    if (rename(tmp_path, tbl->path) != 0) {
        unlink(tmp_path);
        kdb_err_io(tbl->path, "rename rewrite");
        return KDB_ERR_IO;
    }
    kdb__fsync_parent_dir(tbl->path);


    tbl->fp = fopen(tbl->path, "r+b");
    if (!tbl->fp) {
        kdb_err_io(tbl->path, "reopen after rewrite");
        return KDB_ERR_IO;
    }

    memcpy(&tbl->header, &new_hdr, sizeof(new_hdr));
    tbl->dirty = 0;
    return KDB_OK;
}


static int kdb__identity_transform(KdbRecord *r, void *ud) {
    KDB_UNUSED(ud);
    return !r->deleted;
}

KdbStatus kdb_storage_compact(KdbTable *tbl) {
    return kdb_storage_rewrite(tbl, kdb__identity_transform, NULL);
}


KdbStatus kdb_storage_append_batch(KdbTable  *tbl,
                                   KdbRecord *records,
                                   size_t     count) {
    if (!tbl || !records) {
        kdb_err_null_arg("tbl/records", "kdb_storage_append_batch");
        return KDB_ERR_BAD_ARG;
    }
    if (count > KDB_MAX_BATCH_SIZE) {
        kdb_err_batch_too_large(count, KDB_MAX_BATCH_SIZE);
        return KDB_ERR_FULL;
    }

    if (fseek(tbl->fp, 0, SEEK_END) != 0) {
        kdb_err_io(tbl->path, "fseek batch end");
        return KDB_ERR_IO;
    }

    for (size_t i = 0; i < count; i++) {
        records[i].id = tbl->header.next_id++;
        KdbStatus st = kdb_record_write(&records[i], tbl->fp);
        if (st != KDB_OK) return st;
        tbl->header.record_count++;
    }

    tbl->dirty = 1;
    return KDB_OK;
}


KdbStatus kdb_storage_list_tables(const char *data_dir,
                                  char        names_out[][KDB_MAX_NAME_LEN],
                                  uint32_t   *count_out) {
    if (!data_dir || !names_out || !count_out) {
        kdb_err_null_arg("data_dir/names_out/count_out", "kdb_storage_list_tables");
        return KDB_ERR_BAD_ARG;
    }

    *count_out = 0;
    DIR *dir = opendir(data_dir);
    if (!dir) {
        kdb_err_io(data_dir, "opendir");
        return KDB_ERR_IO;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *count_out < KDB_MAX_TABLES) {
        const char *name = entry->d_name;
        size_t nlen = strlen(name);
        if (nlen <= 4) continue;
        if (strcmp(name + nlen - 4, ".kdb") != 0) continue;

        size_t base_len = nlen - 4;
        if (base_len >= KDB_MAX_NAME_LEN) continue;

        memcpy(names_out[*count_out], name, base_len);
        names_out[*count_out][base_len] = '\0';
        (*count_out)++;
    }

    closedir(dir);
    return KDB_OK;
}


typedef struct { uint64_t deleted; } KdbCountDeletedCtx;

static int kdb__count_deleted_cb(const KdbRecord *r, void *ud) {
    KDB_UNUSED(r);
    KDB_UNUSED(ud);
    return 1; 
}

KdbStatus kdb_storage_stats(KdbTable *tbl, KdbStorageStats *out) {
    if (!tbl || !out) {
        kdb_err_null_arg("tbl/out", "kdb_storage_stats");
        return KDB_ERR_BAD_ARG;
    }

    memset(out, 0, sizeof(*out));

    struct stat st;
    if (stat(tbl->path, &st) == 0) {
        out->file_size_bytes = (uint64_t)st.st_size;
    }

    
    if (fseek(tbl->fp, (long)tbl->header.data_offset, SEEK_SET) == 0) {
        KdbRecord *r;
        while ((r = kdb_record_read(tbl->fp)) != NULL) {
            if (r->deleted) out->deleted_count++;
            else            out->live_count++;
            kdb_record_free(r);
        }
    }

    out->record_count = out->live_count + out->deleted_count;
    out->fragmentation_ratio = (out->record_count > 0)
        ? (double)out->deleted_count / (double)out->record_count
        : 0.0;

    
    fseek(tbl->fp, (long)tbl->header.data_offset, SEEK_SET);

    KDB_UNUSED(kdb__count_deleted_cb);
    return KDB_OK;
}