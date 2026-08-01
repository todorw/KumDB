#ifndef KUMDB_SQL_H
#define KUMDB_SQL_H

#include "kumdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A thin SQL front end over the exact same engine kdb_add/kdb_find/etc.
 * use underneath -- same storage, same query semantics, just a different
 * syntax on top. Single table only: no JOIN, no subqueries, no OR (the
 * engine itself is AND-only). One statement per call.
 *
 * Supported:
 *   CREATE TABLE t (col TYPE [NOT NULL] [INDEX], ...)
 *   DROP TABLE t
 *   INSERT INTO t (col, ...) VALUES (val, ...)
 *   SELECT * | col, ... FROM t [WHERE cond [AND cond ...]]
 *                              [ORDER BY col [ASC|DESC]]
 *                              [LIMIT n [OFFSET m]]
 *   UPDATE t SET col = val, ... [WHERE cond [AND cond ...]]
 *   DELETE FROM t [WHERE cond [AND cond ...]]
 *
 * Types: INT/INTEGER, FLOAT/REAL/DOUBLE, BOOL/BOOLEAN, TEXT/STRING/VARCHAR,
 * BLOB. VARCHAR(n)/CHAR(n) length specs are accepted and ignored (KumDB
 * strings aren't fixed-width). "id", "created_at", "updated_at" are
 * reserved -- KumDB already manages them; declaring them in CREATE TABLE
 * is silently ignored rather than an error, so a copy-pasted
 * "id INTEGER PRIMARY KEY" doesn't blow up on you.
 *
 * WHERE conditions: col = val, != / <>, >, >=, <, <=, BETWEEN a AND b,
 * IS NULL, IS NOT NULL, LIKE 'pat' (pat may have a leading and/or
 * trailing '%' for startswith/endswith/contains -- no '_' wildcard, no
 * mid-string '%').
 *
 * String literals in SQL text are capped at a couple KB. There's no SQL
 * literal syntax for BLOB values -- set/read blob columns through the C
 * API (kdb_field_blob / kdb_row_get_blob) or the NoSQL kdb_add() path.
 *
 * On a SELECT: *rows_out (if non-NULL) receives a freshly allocated
 * KdbRows* that you must free with kdb_rows_free(). *affected_out is
 * left untouched.
 * On INSERT/UPDATE/DELETE: *affected_out (if non-NULL) receives the
 * number of rows affected. *rows_out is left untouched.
 * On CREATE TABLE/DROP TABLE: neither out-param is touched.
 * Either out-param may be NULL if you don't care about it.
 */
KdbStatus kdb_exec_sql(KumDB *db, const char *sql,
                       KdbRows **rows_out, size_t *affected_out);

#ifdef __cplusplus
}
#endif

#endif
