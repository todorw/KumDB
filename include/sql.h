#ifndef KUMDB_SQL_H
#define KUMDB_SQL_H

#include "kumdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A thin SQL front end over the exact same engine kdb_add/kdb_find/etc.
 * use underneath -- same storage, same query semantics, just a different
 * syntax on top. One statement per call.
 *
 * "-- to end of line" comments and C-style block comments (can span
 * lines) are both stripped like whitespace anywhere a token could start.
 *
 * Supported:
 *   CREATE TABLE t (col TYPE [NOT NULL] [INDEX], ...)
 *   ALTER TABLE t ADD [COLUMN] col TYPE [NOT NULL] [INDEX]
 *   ALTER TABLE t DROP [COLUMN] col
 *   DROP TABLE t
 *   CREATE VIEW v AS SELECT ...
 *   DROP VIEW v
 *   INSERT INTO t (col, ...) VALUES (val, ...)
 *   [WITH name AS (SELECT ...) [, name2 AS (SELECT ...)]*]
 *   SELECT [DISTINCT] * | item, ... FROM t [[AS] alias]
 *                               [[INNER|LEFT [OUTER]] JOIN t2 [[AS] alias2] ON a.col = b.col [AND ...]]*
 *                               [WHERE cond [AND|OR cond ...]]
 *                               [GROUP BY col, ...]
 *                               [HAVING cond [AND|OR cond ...]]
 *                               [(UNION|INTERSECT|EXCEPT) [ALL] SELECT ...]*
 *                               [ORDER BY col [ASC|DESC], ...]
 *                               [LIMIT n [OFFSET m]]
 *   UPDATE t SET col = val, ... [WHERE cond [AND|OR cond ...]]
 *   DELETE FROM t [WHERE cond [AND|OR cond ...]]
 *
 * ORDER BY (top-level, not OVER's own) takes one or more comma-separated
 * columns, each with its own optional ASC/DESC, ties broken left to right
 * -- max 4 columns. Applies once, after everything else (GROUP BY/HAVING/
 * UNION), same as before.
 *
 * AND binds tighter than OR, standard SQL precedence: "a=1 AND b=2 OR c=3"
 * means "(a=1 AND b=2) OR (c=3)" -- parenthesize to override, nested up to
 * 16 levels deep: "(a=1 OR b=2) AND c=3". Parens on UPDATE/DELETE's WHERE
 * cost a full table scan to resolve (the storage engine's own pushdown
 * only understands flat OR'd AND-groups); parens on a SELECT's WHERE/
 * HAVING are free (evaluated in memory either way).
 *
 * A SELECT item is a plain column, an aggregate call --
 * COUNT(*)/COUNT(col)/SUM(col)/AVG(col)/MIN(col)/MAX(col) -- or a CASE
 * expression (below), optionally renamed with "AS alias" (default alias
 * is e.g. "SUM(amount)", or "case" for an unaliased CASE). Without
 * GROUP BY, one or more aggregate items collapse the whole result into a
 * single summary row. GROUP BY takes one or more comma-separated columns;
 * you get one row per distinct combination of values across all of them.
 * Every other selected item must be an aggregate call (same rule SQL uses
 * -- a plain column that isn't one of the GROUP BY columns is rejected).
 * "SELECT col FROM t GROUP BY col" with no aggregate at all is a valid way
 * to get distinct values. SUM/AVG always come back as FLOAT regardless of
 * the source column's type. HAVING filters the aggregated output (needs
 * GROUP BY or an aggregate item).
 *
 * Scalar functions also work as a SELECT item, freely alongside plain
 * columns/CASE/window functions (not combined with GROUP BY/a plain
 * aggregate, same restriction CASE and window functions have): UPPER(x)/
 * LOWER(x), LENGTH(x), TRIM(x), SUBSTR(x,start[,len])/SUBSTRING(...) (1-
 * based start), CONCAT(a,b,...) (2-4 args), ROUND(x[,ndigits]) (always
 * FLOAT), ABS(x) (preserves INT/FLOAT), CEIL(x)/CEILING(x)/FLOOR(x)
 * (always INT), MOD(a,b) (INT if both args are INT, FLOAT otherwise),
 * COALESCE(a,b,...) (2-4 args), NULLIF(a,b), CAST(x AS type) (same type
 * names as CREATE TABLE), NOW() (epoch seconds, INT). Every argument is a
 * plain column or a literal -- never another function call/aggregate/
 * CASE, no arbitrary expression nesting. A type-mismatched argument
 * produces NULL for that row rather than erroring the query (CAST fails
 * the same soft way -- an unparseable string casts to 0/0.0).
 *
 * Window functions: ROW_NUMBER()/RANK()/DENSE_RANK() and COUNT/SUM/AVG/
 * MIN/MAX all work with OVER ([PARTITION BY col,...] [ORDER BY col
 * [ASC|DESC],...]) -- unlike GROUP BY, rows aren't collapsed, every row
 * keeps its own computed value. RANK/DENSE_RANK give tied rows (equal
 * ORDER BY values within a partition) the same rank, RANK leaving a gap
 * afterward (1,1,3), DENSE_RANK not (1,1,2). A windowed aggregate covers
 * the whole partition (no running/cumulative ROWS/RANGE BETWEEN frame
 * clause). PARTITION BY/ORDER BY are both optional. ROW_NUMBER/RANK/
 * DENSE_RANK always need OVER; COUNT/SUM/AVG/MIN/MAX use OVER to switch
 * from their GROUP BY-collapsing form to this one -- a SELECT can't mix a
 * window function with GROUP BY or a plain aggregate. Works after a JOIN,
 * on qualified columns, same as anything else there.
 *
 * JOIN (INNER or LEFT) matches rows via a conjunction of col = col
 * equalities in ON (no OR, no comparing to a literal there -- that's what
 * WHERE, applied after the join, is for). Chain as many JOIN clauses as
 * you want -- "FROM t1 JOIN t2 ON ... JOIN t3 ON ..." -- each one matches
 * against everything accumulated so far, so a later ON can reference any
 * earlier alias in the chain, not just the table immediately before it.
 * Every column reference anywhere after a JOIN -- SELECT list, ON, WHERE,
 * ORDER BY -- must be table-qualified ("alias.col" or "table.col"),
 * including the synthetic "alias.id"/"alias.created_at"/"alias.updated_at"
 * pseudo-columns each table gets for joining against (id/created_at/
 * updated_at aren't ordinarily part of a row's field list, but they're
 * common join keys, e.g. "ON orders.user_id = users.id"). LEFT JOIN pads
 * an unmatched row with NULL for every column from that step's table.
 * GROUP BY/aggregates work fine after a JOIN too -- group/aggregate on
 * qualified columns same as any other post-JOIN reference. An alias
 * doesn't need AS --
 * "FROM t alias"/"JOIN t2 alias2" work the same as with it -- except for a
 * short list of words (WHERE/GROUP/HAVING/ORDER/LIMIT/UNION/JOIN/INNER/
 * LEFT/ON/AS) that always mean the keyword, never a bare alias. A
 * non-JOIN query can also qualify its own columns with its own alias if
 * one's in scope ("FROM users u WHERE u.name=..." works same as
 * unqualified); mostly useful for EXISTS's inner query (below).
 *
 * CASE (as a SELECT item): "CASE WHEN cond THEN val [WHEN cond THEN val
 * ...] [ELSE val] END". First matching WHEN wins; NULL if nothing matches
 * and there's no ELSE. Each WHEN condition is one or more WHERE-style
 * conditions (same operators) combined with AND/OR, same precedence as
 * WHERE -- no parens within one WHEN, max 3 conditions per WHEN, max 4
 * WHEN branches. THEN/ELSE values are literals, resolved once at parse
 * time -- not column references. Works fine after a JOIN (conditions can
 * reference qualified columns), not combined with GROUP BY/aggregates.
 *
 * DISTINCT dedupes the result by the exact selected columns, after
 * projection. UNION/UNION ALL/INTERSECT/INTERSECT ALL/EXCEPT/EXCEPT ALL
 * chain multiple SELECTs (same column count required): UNION/INTERSECT/
 * EXCEPT dedupe (concat, set-intersect, set-minus respectively), the ALL
 * variants use multiset semantics instead (ALL keeps duplicates; INTERSECT
 * ALL/EXCEPT ALL cap a run of duplicates at how many matching rows the
 * other side has). Mixing different operators, or ALL and non-ALL, in one
 * chain isn't supported -- pick one for the whole statement. ORDER BY/
 * LIMIT, when a chain is present, apply once to the combined result, not
 * to an individual arm.
 *
 * (SELECT ...) works as a value in WHERE/HAVING: scalar form for
 * =/!=/>/>=/</<= (must return exactly one row, one column), or
 * "IN (SELECT col FROM ...)" in place of a literal list. Can be
 * correlated -- the inner query may reference the outer row, qualified
 * with the outer query's own alias, same as EXISTS below -- decided
 * automatically per-subquery by whether its text actually references that
 * alias; nothing new to write. A non-correlated one keeps running once,
 * up front, like any other SELECT; a correlated one re-runs once per
 * outer row (real correlated-subquery cost, same as EXISTS). Correlated
 * scalar/IN subqueries combine with AND/OR and parens like any other
 * condition, and work in UPDATE/DELETE's WHERE too; not supported in
 * HAVING or inside a CASE WHEN condition (no real outer row there).
 *
 * EXISTS/NOT EXISTS (SELECT ...) in WHERE is always correlated -- the
 * inner query can reference the outer row, qualified with the outer
 * query's own alias. Only row existence matters, not what gets projected
 * ("SELECT *" is the usual choice). Re-runs the inner query once per
 * outer row (real correlated-subquery cost); combines with AND/OR and
 * parens like any other condition; works in UPDATE/DELETE's WHERE too;
 * not supported in HAVING.
 *
 * CREATE VIEW v AS SELECT ... stores a named query; SELECT ... FROM v
 * re-runs it fresh every time (not materialized/cached). Validates the
 * underlying query immediately (fails at creation, not first use, if it
 * references something that doesn't exist). WHERE/ORDER BY/LIMIT/DISTINCT/
 * aggregates all work on top of a view same as a real table; a view can't
 * be JOINed (either side) yet. Views are stored as rows in a reserved
 * internal table, "__kumdb_views__" -- it'll show up in
 * kdb_list_tables()/the CLI's "tables" command like any other table
 * (querying it directly works fine, it's just not hidden), so don't name
 * a real table that.
 *
 * WITH name AS (SELECT ...) SELECT ... FROM name (a CTE) is a view scoped
 * to one statement instead of persisted -- same validate-immediately/
 * re-run-fresh/no-JOIN behavior as CREATE VIEW, gone the moment the
 * statement finishes either way. Chain more with a comma; a later one can
 * reference an earlier one (each validated and made visible in
 * declaration order), never the reverse and never itself -- no RECURSIVE.
 * Only ever precedes a SELECT, not UPDATE/DELETE/INSERT.
 *
 * FROM (SELECT ...) AS alias (a derived table) works the same way -- a
 * subquery standing in for a real table, needing its own alias (nothing
 * else identifies it). WHERE/ORDER BY/LIMIT/DISTINCT/aggregates all work
 * on top same as CREATE VIEW; only valid as the primary FROM target, not
 * a JOIN target. Unlike WITH, nothing gets registered anywhere even
 * temporarily -- parsed and re-run inline every time, and (unlike CREATE
 * VIEW) not validated until the moment it actually runs.
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
 * IN (a, b, c), IS NULL, IS NOT NULL, LIKE 'pat' (standard SQL wildcards --
 * '%' any run of characters including none, '_' exactly one -- anywhere in
 * the pattern; no ESCAPE clause, no way to match a literal '%'/'_').
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
