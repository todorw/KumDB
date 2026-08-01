#ifndef KUMDB_ERROR_H
#define KUMDB_ERROR_H

#include "internal.h"
#include <stdarg.h>

const char *kdb_last_error(void);

void kdb_set_error(KdbStatus status, const char *fmt, ...);

void kdb_clear_error(void);

KdbStatus kdb_last_status(void);

const char *kdb_status_name(KdbStatus status);

void kdb_err_oom(const char *what);

void kdb_err_io           (const char *path, const char *op);
void kdb_err_io_corrupt   (const char *path);
void kdb_err_io_locked    (const char *path);

void kdb_err_null_arg     (const char *arg_name, const char *fn_name);
void kdb_err_bad_arg      (const char *arg_name, const char *reason);

void kdb_err_table_not_found (const char *table_name);
void kdb_err_table_exists    (const char *table_name);
void kdb_err_table_full      (const char *table_name);
void kdb_err_table_read_only (const char *table_name);

void kdb_err_record_not_found(uint64_t id, const char *table_name);
void kdb_err_field_not_found (const char *col_name, const char *table_name);
void kdb_err_column_exists   (const char *col_name, const char *table_name);
void kdb_err_bad_type        (const char *col_name, KdbType expected, KdbType got);
void kdb_err_bad_filter      (const char *filter_key, const char *reason);

void kdb_err_validation      (const char *table_name, const char *reason);

void kdb_err_batch_too_large (size_t count, size_t limit);

#ifdef KDB_DEBUG
  #include <stdio.h>
  #define KDB_ASSERT(cond, msg) \
      do { \
          if (!(cond)) { \
              fprintf(stderr, "[KumDB ASSERT] %s:%d — %s\n", \
                      __FILE__, __LINE__, (msg)); \
              __builtin_trap(); \
          } \
      } while (0)
#else
  #define KDB_ASSERT(cond, msg)  ((void)0)
#endif

#define KDB_FAIL(status, error_call) \
    do { \
        error_call; \
        return (status); \
    } while (0)

#define KDB_FAIL_NULL(error_call) \
    do { \
        error_call; \
        return NULL; \
    } while (0)

#endif 