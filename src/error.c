#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../include/error.h"
#include "../include/types.h"


#define KDB_ERR_BUF_SIZE 1024


#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define KDB_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__)
  #define KDB_THREAD_LOCAL __thread
#else
  
  #define KDB_THREAD_LOCAL
#endif

static KDB_THREAD_LOCAL char      kdb__err_buf[KDB_ERR_BUF_SIZE] = {0};
static KDB_THREAD_LOCAL KdbStatus kdb__err_status = KDB_OK;


void kdb_set_error(KdbStatus status, const char *fmt, ...) {
    kdb__err_status = status;
    va_list args;
    va_start(args, fmt);
    vsnprintf(kdb__err_buf, KDB_ERR_BUF_SIZE, fmt, args);
    va_end(args);
    
    kdb__err_buf[KDB_ERR_BUF_SIZE - 1] = '\0';
}

const char *kdb_last_error(void) {
    if (kdb__err_buf[0] == '\0') return "no error";
    return kdb__err_buf;
}

KdbStatus kdb_last_status(void) {
    return kdb__err_status;
}

void kdb_clear_error(void) {
    kdb__err_buf[0]  = '\0';
    kdb__err_status  = KDB_OK;
}

const char *kdb_status_name(KdbStatus status) {
    switch (status) {
        case KDB_OK:             return "KDB_OK";
        case KDB_ERR_OOM:        return "KDB_ERR_OOM";
        case KDB_ERR_IO:         return "KDB_ERR_IO";
        case KDB_ERR_NOT_FOUND:  return "KDB_ERR_NOT_FOUND";
        case KDB_ERR_EXISTS:     return "KDB_ERR_EXISTS";
        case KDB_ERR_BAD_TYPE:   return "KDB_ERR_BAD_TYPE";
        case KDB_ERR_BAD_FILTER: return "KDB_ERR_BAD_FILTER";
        case KDB_ERR_BAD_ARG:    return "KDB_ERR_BAD_ARG";
        case KDB_ERR_LOCKED:     return "KDB_ERR_LOCKED";
        case KDB_ERR_CORRUPT:    return "KDB_ERR_CORRUPT";
        case KDB_ERR_FULL:       return "KDB_ERR_FULL";
        case KDB_ERR_VALIDATION: return "KDB_ERR_VALIDATION";
        case KDB_ERR_READ_ONLY:  return "KDB_ERR_READ_ONLY";
        case KDB_ERR_SQL_SYNTAX: return "KDB_ERR_SQL_SYNTAX";
        default:                 return "KDB_ERR_UNKNOWN";
    }
}


void kdb_err_oom(const char *what) {
    kdb_set_error(KDB_ERR_OOM,
        "Out of memory trying to allocate %s. "
        "Your system is cooked — try freeing some RAM, or just buy more.",
        what ? what : "something");
}

void kdb_err_io(const char *path, const char *op) {
    kdb_set_error(KDB_ERR_IO,
        "I/O failure during '%s' on '%s'. "
        "Check your disk, your permissions, and your life choices.",
        op   ? op   : "unknown operation",
        path ? path : "unknown path");
}

void kdb_err_io_corrupt(const char *path) {
    kdb_set_error(KDB_ERR_CORRUPT,
        "File '%s' is corrupt — magic bytes don't match or version is wrong. "
        "Did you edit it by hand? Don't do that.",
        path ? path : "unknown path");
}

void kdb_err_io_locked(const char *path) {
    kdb_set_error(KDB_ERR_LOCKED,
        "File '%s' is locked by another process. "
        "Someone else is using this table. Wait your turn.",
        path ? path : "unknown path");
}

void kdb_err_null_arg(const char *arg_name, const char *fn_name) {
    kdb_set_error(KDB_ERR_BAD_ARG,
        "NULL passed for '%s' in %s(). "
        "We don't accept NULL here. Read the docs.",
        arg_name ? arg_name : "argument",
        fn_name  ? fn_name  : "function");
}

void kdb_err_bad_arg(const char *arg_name, const char *reason) {
    kdb_set_error(KDB_ERR_BAD_ARG,
        "Bad argument '%s': %s.",
        arg_name ? arg_name : "argument",
        reason   ? reason   : "it's just wrong");
}

void kdb_err_table_not_found(const char *table_name) {
    kdb_set_error(KDB_ERR_NOT_FOUND,
        "Table '%s' doesn't exist. "
        "Did you spell it right? It's case-sensitive. We're not Google.",
        table_name ? table_name : "unknown");
}

void kdb_err_table_exists(const char *table_name) {
    kdb_set_error(KDB_ERR_EXISTS,
        "Table '%s' already exists. "
        "Pick a different name or drop the existing one first.",
        table_name ? table_name : "unknown");
}

void kdb_err_table_full(const char *table_name) {
    kdb_set_error(KDB_ERR_FULL,
        "Table '%s' has hit the record limit (%d records). "
        "How did you even get here? Compact and clean up first.",
        table_name ? table_name : "unknown",
        KDB_MAX_RECORDS);
}

void kdb_err_table_read_only(const char *table_name) {
    kdb_set_error(KDB_ERR_READ_ONLY,
        "Table '%s' is open in read-only mode. "
        "You tried to write to it. That's not how read-only works.",
        table_name ? table_name : "unknown");
}

void kdb_err_record_not_found(uint64_t id, const char *table_name) {
    kdb_set_error(KDB_ERR_NOT_FOUND,
        "Record with id=%llu not found in table '%s'. "
        "It doesn't exist, was deleted, or you have the wrong id.",
        (unsigned long long)id,
        table_name ? table_name : "unknown");
}

void kdb_err_field_not_found(const char *col_name, const char *table_name) {
    kdb_set_error(KDB_ERR_NOT_FOUND,
        "Column '%s' not found in table '%s'. "
        "Check the schema — that column doesn't exist.",
        col_name   ? col_name   : "unknown",
        table_name ? table_name : "unknown");
}

void kdb_err_bad_type(const char *col_name, KdbType expected, KdbType got) {
    kdb_set_error(KDB_ERR_BAD_TYPE,
        "Type mismatch on column '%s': expected %s, got %s. "
        "KumDB inferred the schema — if it got it wrong, define it explicitly.",
        col_name ? col_name : "unknown",
        kdb_type_name(expected),
        kdb_type_name(got));
}

void kdb_err_bad_filter(const char *filter_key, const char *reason) {
    kdb_set_error(KDB_ERR_BAD_FILTER,
        "Invalid filter key '%s': %s.",
        filter_key ? filter_key : "unknown",
        reason     ? reason     : "it's malformed");
}

void kdb_err_validation(const char *table_name, const char *reason) {
    kdb_set_error(KDB_ERR_VALIDATION,
        "Validation failed for table '%s': %s.",
        table_name ? table_name : "unknown",
        reason     ? reason     : "your validator said no");
}

void kdb_err_batch_too_large(size_t count, size_t limit) {
    kdb_set_error(KDB_ERR_FULL,
        "Batch size %zu exceeds the limit of %zu. "
        "Split it up into smaller chunks.",
        count, limit);
}