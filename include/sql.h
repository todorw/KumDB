#ifndef KUMDB_SQL_H
#define KUMDB_SQL_H

#include "kumdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A thin SQL front end over the exact same engine kdb_add/kdb_find/etc.
 * use underneath -- same storage, same query semantics, just a different
 * syntax on top. Single table only: no JOIN, no subqueries. One statement
 * per call.
 *
 * Supported:
 *   CREATE TABLE t (col TYPE [NOT NULL] [INDEX], ...)
 *   ALTER TABLE t ADD [COLUMN] col TYPE [NOT NULL] [INDEX]
 *   ALTER TABLE t DROP [COLUMN] col
 *   DROP TABLE t
 *   INSERT INTO t (col, ...) VALUES (val, ...)
 *   SELECT * | item, ... FROM t [WHERE cond [AND|OR cond ...]]
 *                               [GROUP BY col]
 *                               [ORDER BY col [ASC|DESC]]
 *                               [LIMIT n [OFFSET m]]
 *   UPDATE t SET col = val, ... [WHERE cond [AND|OR cond ...]]
 *   DELETE FROM t [WHERE cond [AND|OR cond ...]]
 *
 * AND binds tighter than OR, standard SQL precedence, no parens/nesting:
 * "a=1 AND b=2 OR c=3" means "(a=1 AND b=2) OR (c=3)".
 *
 * A SELECT item is a plain column, or an aggregate call --
 * COUNT(*)/COUNT(col)/SUM(col)/AVG(col)/MIN(col)/MAX(col) -- optionally
 * renamed with "AS alias" (default alias is e.g. "SUM(amount)"). Without
 * GROUP BY, one or more aggregate items collapse the whole result into a
 * single summary row. With GROUP BY col, you get one row per distinct
 * value of col; every other selected item must be an aggregate call (same
 * rule SQL uses -- a plain column that isn't the GROUP BY column is
 * rejected). "SELECT col FROM t GROUP BY col" with no aggregate at all
 * is a valid way to get distinct values. SUM/AVG always come back as
 * FLOAT regardless of the source column's type. No HAVING, no grouping
 * by more than one column, no window functions.
 *
 * Types: INT/INTEGER, FLOAT/REAL/DOUBLE, BOOL/BOOLEAN, TEXT/STRING/VARCHAR,
 * BLOB. VARCHAR(n)/CHAR(n) length specs are accepted and ignored (KumDB
 * strings aren't fixed-width). "id", "created_at", "updated_at" are
 * reserved -- KumDB already manages them; declaring them in CREATE TABLE
 * or ALTER TABLE ADD is silently ignored / rejected rather than corrupting
 * anything, so a copy-pasted "id INTEGER PRIMARY KEY" doesn't blow up on you.
 *
 * ALTER TABLE ADD only changes the schema -- existing rows don't get the
 * new column's value until you UPDATE them, same as any other missing
 * field. ALTER TABLE DROP rewrites the whole table file to strip that
 * field from every row, same cost as kdb_compact().
 *
 * WHERE conditions: col = val, != / <>, >, >=, <, <=, BETWEEN a AND b,
 * IN (a, b, c), IS NULL, IS NOT NULL, LIKE 'pat' (pat may have a leading and/or
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
