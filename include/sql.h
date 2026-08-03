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
 *   CREATE TABLE t (col TYPE [NOT NULL] [UNIQUE | PRIMARY KEY] [INDEX] [REFERENCES t2(col2) [ON DELETE|UPDATE action]], ...
 *                   [, FOREIGN KEY (col[, col...]) REFERENCES t2(col2[, col2...]) [ON DELETE|UPDATE action]]* [, CHECK (col OP literal)]*)
 *   ALTER TABLE t ADD [COLUMN] col TYPE [NOT NULL] [UNIQUE | PRIMARY KEY] [INDEX] [REFERENCES t2(col2) [ON DELETE|UPDATE action]]
 *   ALTER TABLE t ADD FOREIGN KEY (col[, col...]) REFERENCES t2(col2[, col2...]) [ON DELETE action] [ON UPDATE action]  -- action: RESTRICT | CASCADE | SET NULL; 2+ columns on each side is a composite FK
 *   ALTER TABLE t ADD CHECK (col OP literal)
 *   ALTER TABLE t DROP [COLUMN] col
 *   ALTER TABLE t DROP FOREIGN KEY (col[, col...])
 *   ALTER TABLE t RENAME COLUMN col TO new_col
 *   ALTER TABLE t ALTER [COLUMN] col TYPE newtype | SET NOT NULL | DROP NOT NULL | SET UNIQUE | DROP UNIQUE
 *   ALTER TABLE t RENAME [TO] new_name
 *   DROP TABLE t
 *   CREATE VIEW v AS SELECT ...
 *   DROP VIEW v
 *   CREATE INDEX [name] ON t (col, ...)
 *   DROP INDEX [name] ON t (col, ...)
 *   INSERT INTO t (col, ...) VALUES (val, ...) [, (val, ...)]* [RETURNING * | col, ...]
 *   INSERT INTO t (col, ...) SELECT ... [RETURNING * | col, ...]
 *   INSERT INTO t (col, ...) VALUES (val, ...) ON CONFLICT (col, ...) DO NOTHING | DO UPDATE SET col = val, ... [RETURNING * | col, ...]
 *   [WITH name AS (SELECT ...) [, name2 AS (SELECT ...)]*]
 *   SELECT [DISTINCT] * | item, ... FROM t [[AS] alias]
 *                               [[INNER|LEFT [OUTER]|RIGHT [OUTER]|FULL [OUTER]|CROSS] JOIN t2 [[AS] alias2] [ON a.col OP (b.col|literal) [AND ...]]]*
 *                               [WHERE cond [AND|OR cond ...]]
 *                               [GROUP BY col, ... | ROLLUP(col, ...) | CUBE(col, ...) | GROUPING SETS ((col, ...), ...)]
 *                               [HAVING cond [AND|OR cond ...]]
 *                               [(UNION|INTERSECT|EXCEPT) [ALL] SELECT ...]*
 *                               [ORDER BY col [ASC|DESC], ...]
 *                               [LIMIT n [OFFSET m]]
 *   UPDATE t SET col = val, ... [WHERE cond [AND|OR cond ...]] [RETURNING * | col, ...]
 *   DELETE FROM t [WHERE cond [AND|OR cond ...]] [RETURNING * | col, ...]
 *
 *   BEGIN [TRANSACTION | WORK] | START TRANSACTION
 *   COMMIT [TRANSACTION | WORK]
 *   ROLLBACK [TRANSACTION | WORK]
 *   SAVEPOINT name
 *   RELEASE [SAVEPOINT] name
 *   ROLLBACK TO [SAVEPOINT] name
 *
 * BEGIN opens a kdb_tx_* transaction on the connection (see kumdb.h) --
 * every INSERT/UPDATE/DELETE after that runs through it instead of the
 * plain non-transactional call, until COMMIT or ROLLBACK ends it. No
 * nested transactions (BEGIN while one is already open errors). Schema
 * changes (CREATE/ALTER/DROP) aren't wrapped by kdb_tx_*, so they're
 * rejected while a transaction is open rather than silently running
 * outside it, where ROLLBACK wouldn't actually undo them. SAVEPOINT/
 * RELEASE SAVEPOINT/ROLLBACK TO SAVEPOINT are all supported and need an
 * open transaction; ROLLBACK TO SAVEPOINT undoes everything done since
 * that savepoint (including any savepoints nested inside it, which are
 * discarded too) but leaves the savepoint itself and the transaction
 * open, so it can be rolled back to again. RELEASE SAVEPOINT keeps the
 * changes and just forgets the name (and any names nested inside it). See
 * kdb_tx_savepoint/kdb_tx_rollback_to_savepoint/kdb_tx_release_savepoint
 * in kumdb.h for the underlying semantics. A transaction still open when
 * the connection closes is rolled back automatically.
 *
 * INSERT always needs an explicit column list, never a bare
 * "INSERT INTO t VALUES (...)". VALUES accepts more than one
 * comma-separated tuple in one statement, each inserted as its own row in
 * order. INSERT INTO t (cols) SELECT ... inserts one row per SELECT
 * result row instead, matching to (cols) by position (not name) -- column
 * counts must match, checked before anything is inserted. Either form: a
 * row partway through that fails to insert leaves whatever already
 * succeeded committed rather than rolling back -- no implicit
 * per-statement transaction wrapping here.
 *
 * ON CONFLICT (cols) DO NOTHING / DO UPDATE SET col = val, ... turns a
 * single-row VALUES insert into an upsert (rejected after a multi-row
 * VALUES list). Unlike real SQL's ON CONFLICT (which reacts to an actual
 * constraint-violation error), this checks proactively: before inserting
 * anything, whether a row already matches every named column's value --
 * DO NOTHING leaves things as they are if so (0 affected); DO UPDATE SET
 * updates it. The named columns don't need to be declared UNIQUE/PRIMARY
 * KEY (see CREATE TABLE below, where those ARE really enforced) -- this
 * works as a general existence check on any column combination; if
 * they're not unique, more than one row can match, and every match gets
 * the same SET, same as any other filtered UPDATE. No match either way
 * falls back to a plain insert. Every ON CONFLICT column must be one of
 * the INSERT's own target columns; DO UPDATE SET's values are literals, same
 * as a plain UPDATE's SET (no referencing the row about to be inserted).
 *
 * RETURNING * | col, ... trails INSERT (every form, including multi-row
 * VALUES, INSERT ... SELECT, and ON CONFLICT)/UPDATE/DELETE and hands
 * back the affected rows instead of just a count -- newly inserted
 * row(s) for INSERT, post-update state for UPDATE, the pre-delete image
 * for DELETE. RETURNING * excludes id/created_at/updated_at, same
 * convention plain SELECT * already uses; name one explicitly
 * ("RETURNING id") to get it back, which matters more here than almost
 * anywhere else since recovering a generated id is RETURNING's single
 * most common real use. 0 matching rows on UPDATE/DELETE just means 0
 * returned rows, not an error. No EXCLUDED-style reference to the row
 * that would have been inserted (Postgres's upsert idiom) -- DO UPDATE
 * SET's values are plain literals either way.
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
 * COUNT(*)/COUNT(col)/COUNT(DISTINCT col)/SUM(col)/AVG(col)/MIN(col)/
 * MAX(col)/STRING_AGG(col, 'sep')/GROUP_CONCAT(col, 'sep') (the same
 * function under two names, both requiring an explicit separator
 * argument -- no default-separator or MySQL SEPARATOR-keyword form) -- or
 * a CASE expression (below), optionally renamed with "AS alias" (default
 * alias is e.g. "SUM(amount)", or "case" for an unaliased CASE). Without
 * GROUP BY, one or more aggregate items collapse the whole result into a
 * single summary row. GROUP BY takes one or more comma-separated columns;
 * you get one row per distinct combination of values across all of them.
 * Every other selected item must be an aggregate call (same rule SQL uses
 * -- a plain column that isn't one of the GROUP BY columns is rejected).
 * "SELECT col FROM t GROUP BY col" with no aggregate at all is a valid way
 * to get distinct values. SUM/AVG always come back as FLOAT regardless of
 * the source column's type; STRING_AGG/GROUP_CONCAT comes back NULL for a
 * group with no non-NULL values, not an empty string; STRING_AGG/
 * GROUP_CONCAT only works as a GROUP BY-collapsing aggregate, not as a
 * window function (OVER on it is rejected, unlike the others). HAVING
 * filters the aggregated output (needs GROUP BY or an aggregate item).
 *
 * GROUP BY ROLLUP(col, ...)/CUBE(col, ...)/GROUPING SETS ((col, ...),
 * ...) compute several grouping sets in one query and union the results,
 * same as running one plain GROUP BY per set and UNION ALL-ing them --
 * any column not in a given output row's own set comes back NULL for
 * that row (not distinguished from a real NULL -- no GROUPING() function
 * here). ROLLUP(a, b, c) produces one grouping set per prefix -- (a,b,c),
 * (a,b), (a), () -- a hierarchical subtotal/grand-total pattern. CUBE(a,
 * b) produces every subset -- (a,b), (a), (b), () -- capped at 4 columns
 * (2^n grouping sets stay bounded); use ROLLUP for more columns if a
 * hierarchical breakdown (not every combination) is enough. GROUPING
 * SETS lists the exact sets wanted, nothing implied -- GROUPING SETS
 * ((a,b), (a), ()) gets you a subset of what ROLLUP(a,b) would (skips
 * (a) if you don't list it, or add sets ROLLUP/CUBE wouldn't generate).
 * Each of these must be the entire GROUP BY clause (not mixed with plain
 * columns), and every SELECT item still has to be either an aggregate
 * or one of the columns mentioned somewhere in the clause -- same rule
 * plain GROUP BY uses, just checked against the union of every listed
 * set rather than one fixed list. HAVING filters the unioned result the
 * same as any other GROUP BY.
 *
 * Any GROUP BY-collapsing aggregate call (DISTINCT or not) accepts a
 * trailing FILTER (WHERE cond [AND|OR cond ...]): only rows matching that
 * aggregate's own filter are folded into it, so several differently-
 * filtered aggregates can share one GROUP BY pass -- "SELECT dept,
 * COUNT(*), COUNT(*) FILTER (WHERE status='done') FROM orders GROUP BY
 * dept" -- instead of a CASE WHEN trick inside SUM/COUNT or separate
 * queries. Same "no parens within one condition group" limit CASE's WHEN
 * has. Not supported combined with OVER (...) -- only as a GROUP BY-
 * collapsing aggregate, not a window function.
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
 * Window functions: ROW_NUMBER()/RANK()/DENSE_RANK()/LAG(col[,offset[,
 * default]])/LEAD(col[,offset[,default]])/FIRST_VALUE(col)/LAST_VALUE(col)/
 * NTILE(n) and COUNT/SUM/AVG/MIN/MAX all work with OVER ([PARTITION BY
 * col,...] [ORDER BY col [ASC|DESC],...]) -- unlike GROUP BY, rows aren't
 * collapsed, every row keeps its own computed value. RANK/DENSE_RANK give
 * tied rows (equal ORDER BY values within a partition) the same rank,
 * RANK leaving a gap afterward (1,1,3), DENSE_RANK not (1,1,2). LAG/LEAD
 * return a column's value offset rows before/after the current one (offset
 * defaults to 1) within the same partition, in ORDER BY order; past the
 * partition's edge they return default if given, NULL otherwise.
 * FIRST_VALUE/LAST_VALUE return the partition's first/last row's value
 * (ORDER BY order) -- same on every row, not a running value. NTILE(n)
 * divides each partition's rows into n roughly-equal buckets in ORDER BY
 * order and returns the bucket number (1..n); an uneven split gives
 * earlier buckets the extra row(s); n larger than the partition just
 * leaves some bucket numbers unused, not an error. FIRST_VALUE/LAST_VALUE
 * always cover the whole partition, and LAG/LEAD look at a fixed-offset
 * neighbor within it -- neither takes a frame clause (a parse error, not
 * a silent no-op, if you write one). COUNT/SUM/AVG/MIN/MAX default to
 * the whole partition too, but accept an explicit ROWS/RANGE BETWEEN
 * <start> AND <end> frame clause -- requires ORDER BY in the same OVER
 * (...) -- to narrow that to a running/moving window instead: UNBOUNDED
 * PRECEDING, n PRECEDING, CURRENT ROW, n FOLLOWING, and UNBOUNDED
 * FOLLOWING are all valid bounds under ROWS (row offsets from the current
 * row, clipped to the partition). RANGE only supports UNBOUNDED PRECEDING
 * as the start bound and CURRENT ROW/UNBOUNDED FOLLOWING as the end bound
 * (peer-group based -- ties on ORDER BY all see the same result rather
 * than being split mid-tie); numeric RANGE offsets (n PRECEDING/
 * FOLLOWING) aren't supported, use ROWS for those. `ROWS <bound>`/`RANGE
 * <bound>` with no BETWEEN is shorthand for `BETWEEN <bound> AND CURRENT
 * ROW`. E.g. `SUM(amount) OVER (PARTITION BY region ORDER BY d ROWS
 * BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)` is a running total;
 * `ROWS BETWEEN 1 PRECEDING AND CURRENT ROW` is a 2-row moving sum.
 * PARTITION BY/ORDER BY are both optional. ROW_NUMBER/RANK/DENSE_RANK/
 * LAG/LEAD/FIRST_VALUE/LAST_VALUE/NTILE always need OVER; COUNT/SUM/AVG/
 * MIN/MAX use OVER to switch from their GROUP BY-collapsing form to this
 * one -- a SELECT can't mix a window function with GROUP BY or a plain
 * aggregate. Works after a JOIN, on qualified columns, same as anything
 * else there.
 *
 * JOIN (INNER, LEFT [OUTER], RIGHT [OUTER], FULL [OUTER], or CROSS)
 * matches rows via a conjunction of comparisons in ON -- =, !=, >, >=, <,
 * <=, each between two columns or a column and a literal (number/string/
 * true/false), a real theta join rather than just equi-join. Still no OR
 * in ON -- that's what WHERE, applied after the join, is for. INNER (the
 * default) drops unmatched rows; LEFT keeps
 * every row accumulated so far, padding an unmatched one with NULL for
 * the new table's columns; RIGHT is the mirror (keeps every row of the
 * new table, padding NULL for everything accumulated so far); FULL does
 * both directions; CROSS takes no ON at all (rejected if given one) and
 * is just the cartesian product of both sides. Chain as many JOIN clauses
 * as you want, mixing kinds freely -- "FROM t1 JOIN t2 ON ... RIGHT JOIN
 * t3 ON ..." -- each one matches against everything accumulated so far,
 * so a later ON can reference any earlier alias in the chain, not just
 * the table immediately before it. Every column reference anywhere after
 * a JOIN -- SELECT list, ON, WHERE, ORDER BY -- must be table-qualified
 * ("alias.col" or "table.col"), including the synthetic
 * "alias.id"/"alias.created_at"/"alias.updated_at" pseudo-columns each
 * table gets for joining against (id/created_at/updated_at aren't
 * ordinarily part of a row's field list, but they're common join keys,
 * e.g. "ON orders.user_id = users.id") -- NULL padding includes these
 * pseudo-columns too. GROUP BY/aggregates work fine after a JOIN too --
 * group/aggregate on qualified columns same as any other post-JOIN
 * reference. An alias doesn't need AS --
 * "FROM t alias"/"JOIN t2 alias2" work the same as with it -- except for a
 * short list of words (WHERE/GROUP/HAVING/ORDER/LIMIT/UNION/INTERSECT/
 * EXCEPT/JOIN/INNER/LEFT/RIGHT/FULL/CROSS/OUTER/ON/AS) that always mean
 * the keyword, never a bare alias. A non-JOIN query can also qualify its
 * own columns with its own alias if one's in scope ("FROM users u WHERE
 * u.name=..." works same as unqualified); mostly useful for EXISTS's
 * inner query (below).
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
 * A bare literal (number/string/TRUE/FALSE/NULL) also works as a SELECT
 * item -- same value on every row, FROM still mandatory (this engine's
 * one global SELECT rule; no bare "SELECT 1" with nothing to select it
 * from). Unaliased, defaults to "?column?". Not combined with GROUP BY/
 * aggregates, same limit CASE has.
 *
 * Arithmetic expressions (+, -, *, /, %) also work as a SELECT item:
 * "price * qty AS total", chaining column and/or number-literal terms at
 * real operator precedence (* / % bind tighter than + -, left to right
 * within a level). Always computed in double precision and returned as
 * FLOAT regardless of operand types (same simplification SUM/AVG already
 * make). A missing/NULL/non-numeric column, or division/modulo by zero,
 * makes the expression NULL for that row rather than erroring the query.
 * Up to 6 terms (a term is one column or number, optional leading unary
 * '-'); no parens for grouping within one expression -- "(a+b)*c" isn't
 * parseable, only a flat chain at the precedence above. Unaliased,
 * defaults to "?column?". Not combined with GROUP BY/aggregates. Also
 * works as a WHERE/HAVING condition's left-hand side ("WHERE price * qty
 * > 100"), evaluated per row -- but there, only against a plain numeric
 * literal via the six comparisons (=, !=, >, >=, <, <=), no BETWEEN/IN/
 * LIKE/etc, which don't have a sensible meaning against a computed value.
 * A WHERE/HAVING expression condition always evaluates in memory (every
 * row fetched first, then filtered), never through the indexed storage-
 * level filter path, same as EXISTS/a correlated subquery already forces.
 * Still not usable in JOIN ON or as a function argument (those still take
 * a plain column or literal only).
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
 * aggregates all work on top of a view same as a real table, and a view
 * can be JOINed too (either as the FROM target or a JOIN target, mixed
 * freely with real tables). LEFT/RIGHT/FULL padding against a view needs
 * to know its column names, and a view has no fixed schema of its own the
 * way a real table does -- only whatever rows its query happens to
 * return -- so those three kinds need the view to return at least one row
 * on that particular run; a zero-row view works fine as an INNER/CROSS
 * target (no padding ever needed there) but errors clearly if LEFT/RIGHT/
 * FULL needed to pad against it and had nothing to derive names from.
 * Views are stored as rows in a reserved internal table,
 * "__kumdb_views__" -- it'll show up in kdb_list_tables()/the CLI's
 * "tables" command like any other table (querying it directly works
 * fine, it's just not hidden), so don't name a real table that.
 *
 * WITH name AS (SELECT ...) SELECT ... FROM name (a CTE) is a view scoped
 * to one statement instead of persisted -- same validate-immediately,
 * re-run-fresh, and JOIN-target behavior as CREATE VIEW, gone the moment
 * the statement finishes either way. Chain more with a comma; a later one
 * can reference an earlier one (each validated and made visible in
 * declaration order), never the reverse and never itself -- this plain
 * (non-RECURSIVE) form. Only ever precedes a SELECT, not UPDATE/DELETE/
 * INSERT.
 *
 * WITH RECURSIVE name AS (base_select UNION [ALL] recursive_select)
 * SELECT ... FROM name does self-reference: recursive_select's own "FROM
 * name" sees just the *previous round's new rows* each time (standard
 * semi-naive recursive-CTE evaluation, not the whole running total),
 * repeating until a round produces zero new rows or 10000 rounds pass (a
 * non-converging recursive term errors instead of hanging). UNION (not
 * UNION ALL) drops rows already produced by an earlier round before
 * checking for new rows -- what makes a cycle in the underlying data
 * terminate on its own. Scoped to exactly one CTE (no mixing with other
 * CTEs in the same WITH) with exactly the standard base-UNION-recursive
 * shape (two arms). Base case must return >= 1 row (no separate
 * resultset schema to materialize zero rows against); recursive term's
 * columns must match the base case's exactly (same names, same order --
 * a missing "AS alias" on a computed column is a common way to trip
 * this). Implemented via a real but fully temporary table named name,
 * dropped before returning either way -- invisible outside the
 * statement, safe inside a transaction despite DDL normally being
 * rejected mid-transaction (this one's fully self-contained within the
 * one kdb_exec_sql call).
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
 * NOT NULL and UNIQUE/PRIMARY KEY on a column definition are both really
 * enforced (INSERT/UPDATE reject a violation), not just recorded metadata
 * -- a NULL value never conflicts with another NULL for UNIQUE, same
 * convention real SQL uses. PRIMARY KEY implies both. A plain INDEX/
 * INDEXED/KEY modifier is still just a lookup hint, never enforced --
 * real SQL's own distinction between an index and a uniqueness
 * constraint. UNIQUE/PRIMARY KEY always gets a real index to check
 * against, whether or not INDEX is also given.
 *
 * ALTER TABLE ADD only changes the schema -- existing rows don't get the
 * new column's value until you UPDATE them, same as any other missing
 * field. ALTER TABLE DROP rewrites the whole table file to strip that
 * field from every row, same cost as kdb_compact().
 *
 * CREATE INDEX [name] ON t (cols)/DROP INDEX [name] ON t (cols) index or
 * un-index a column (or columns) *after* the table already exists --
 * unlike INDEX/UNIQUE/PRIMARY KEY on a column definition, which only ever
 * takes effect at that column's creation moment. CREATE INDEX rebuilds
 * from every existing row; DROP INDEX just removes it, leaving the
 * columns and their data untouched. The name, if given, is accepted and
 * ignored -- KumDB finds an index again by its column set, not a name.
 * One column creates an ordinary single-column index; two or more create
 * one real composite (multi-column) index -- a single column-value tuple
 * hashed together, not that many independent single-column indexes --
 * capped at KDB_MAX_COMPOSITE_COLS columns and KDB_MAX_COMPOSITE_INDEXES
 * per table. A query naming every column of a composite index with =/AND
 * (any order) can use it instead of a full scan, same as a single-column
 * index does for one column. Indexing an already-indexed column (or an
 * identical composite column set), or dropping an index that isn't
 * there, both error. A composite index doesn't imply or enforce
 * uniqueness across the combined columns -- that's UNIQUE's job, and the
 * two features are independent of each other.
 *
 * ALTER TABLE t RENAME COLUMN col TO new_col renames a column -- schema,
 * its index if any, and every existing row's field, a full table
 * rewrite same cost as ALTER TABLE ... DROP COLUMN. ALTER TABLE t RENAME
 * [TO] new_name renames the table itself (the file on disk too, not just
 * in-memory state) -- TO is optional. ALTER TABLE t ALTER [COLUMN] col
 * SET NOT NULL / DROP NOT NULL / SET UNIQUE / DROP UNIQUE toggle the
 * nullable/unique flags -- both really enforced from that point on
 * (INSERT/UPDATE reject a violation, same as NOT NULL/UNIQUE on a CREATE
 * TABLE column definition), but only for rows written after the ALTER;
 * existing rows that already violate the new rule aren't retroactively
 * checked or rewritten. SET UNIQUE also indexes the column if it wasn't
 * already. ALTER TABLE t ALTER [COLUMN] col TYPE newtype is a real data
 * migration, not just a metadata flip: every existing row's value is
 * converted to newtype using the same coercions CAST(x AS type) uses
 * (NULL stays NULL; INT/FLOAT/BOOL/STRING otherwise), and any index on
 * the column is rebuilt afterward (its old hash buckets no longer match
 * the new-typed values). If even one existing value can't convert (e.g.
 * a STRING column holding "abc" changed to INT), the whole change is
 * rejected and the table -- schema and every row -- is left completely
 * untouched, never half-migrated. A no-op if newtype is already the
 * column's current type. Renaming a column or table to a name that's
 * already taken errors rather than colliding silently.
 *
 * REFERENCES t2(col2) [ON DELETE action] [ON UPDATE action], as a column
 * modifier (col TYPE ... REFERENCES t2(col2)) or as its own table-level
 * item (FOREIGN KEY (col) REFERENCES t2(col2)), declares a foreign key --
 * t2 and col2 must already exist (checked immediately, not deferred).
 * From then on, col must be NULL or equal to some existing t2.col2
 * value: INSERT/UPDATE giving col a non-NULL value with no match in
 * t2.col2 is rejected. action is RESTRICT (the default if the clause is
 * omitted), CASCADE, or SET NULL, and ON DELETE/ON UPDATE are
 * independent of each other (a delete follows ON DELETE's action, an
 * update that changes the referenced value follows ON UPDATE's):
 * RESTRICT rejects deleting/updating away a t2 row that col still points
 * to; CASCADE deletes the referencing rows too (or propagates the new
 * value, for an update); SET NULL sets col to NULL on them instead (col
 * must be nullable -- rejected at FK-creation time otherwise). CASCADE/
 * SET NULL chain across multiple tables (A references B references C),
 * capped at a fixed depth to catch a cycle rather than recurse forever.
 * col2 can be a real column or one of the id/created_at/updated_at
 * pseudo-columns (referencing a table's own auto id is the most common
 * case, even though it's not a "real" schema column). One FK per column.
 * ALTER TABLE t ADD FOREIGN KEY (col) REFERENCES t2(col2) [ON DELETE ...]
 * [ON UPDATE ...] adds one after the fact (same validation and
 * enforcement); ALTER TABLE t DROP FOREIGN KEY (col) removes it. Dropping
 * col itself drops its FK along with it.
 *
 * FOREIGN KEY (col1, col2, ...) REFERENCES t2(c1, c2, ...) with 2 or more
 * columns on each side (only as a table-level item -- not as a column
 * modifier, which only ever names one column) is a composite (multi-
 * column) foreign key: the two sides must list the same number of
 * columns, matched positionally (col1 corresponds to c1, col2 to c2, and
 * so on). Unlike a single-column FK, c1/c2/... must all be real columns
 * on t2 -- a composite key can't reference the id/created_at/updated_at
 * pseudo-columns. The whole tuple (col1, col2, ...) is checked together:
 * if EVERY one of them is non-NULL, some row on t2 must match all of them
 * at once (MATCH SIMPLE semantics -- any NULL/missing component, on
 * either side, skips the check entirely for that row). Same ON DELETE/ON
 * UPDATE actions as single-column FK (RESTRICT/CASCADE/SET NULL,
 * independent of each other, chainable across tables up to the same
 * cascade depth cap), applied to the whole key at once: a CASCADE update
 * propagates every changed component together in one patch; SET NULL
 * requires every column of the key to be nullable, not just some of
 * them. ALTER TABLE t ADD FOREIGN KEY (col1, col2, ...) REFERENCES
 * t2(c1, c2, ...) [ON DELETE ...] [ON UPDATE ...] adds one after the
 * fact; ALTER TABLE t DROP FOREIGN KEY (col1, col2, ...) removes it
 * (matching the column set, any order).
 *
 * CHECK (col OP literal), as its own table-level item in CREATE TABLE or
 * via ALTER TABLE t ADD CHECK (col OP literal), restricts col's values --
 * OP is one of =, !=, >, >=, <, <= (no BETWEEN/IN/LIKE/etc), literal an
 * INT/FLOAT/BOOL/STRING (not NULL -- a NULL value never violates a CHECK,
 * same as real SQL, so comparing against NULL isn't a meaningful check
 * and is rejected at parse time). Enforced the same way NOT NULL/UNIQUE
 * are -- INSERT/UPDATE reject a violation from here on, existing rows
 * aren't retroactively checked. Dropping col drops any CHECK on it too,
 * rather than leaving a dangling reference to a nonexistent column.
 *
 * WHERE conditions: col = val, != / <>, >, >=, <, <=, BETWEEN a AND b,
 * IN (a, b, c), IS NULL, IS NOT NULL, LIKE 'pat' (standard SQL wildcards --
 * '%' any run of characters including none, '_' exactly one -- anywhere in
 * the pattern; no ESCAPE clause, no way to match a literal '%'/'_'),
 * ILIKE 'pat' (same wildcards, case-insensitive), REGEXP 'pat' (small
 * hand-rolled matcher -- see kdb_regex_match in types.h for the supported
 * subset; not POSIX <regex.h>, which mingw-w64 doesn't provide).
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

/*
 * Same as kdb_exec_sql, but sql may contain bound-parameter placeholders:
 * '?' (positional -- the first '?' takes params[0], the second params[1],
 * and so on) and/or '$1', '$2', ... (explicit 1-based index into params,
 * so a param can be reused or referenced out of order). Both forms can be
 * mixed in one statement. A placeholder inside a string literal or a
 * comment is just text, not a placeholder.
 *
 * Each placeholder is replaced with a properly-escaped SQL literal
 * rendering of the corresponding param before the statement is parsed --
 * safe against injection through the bound values themselves (a string
 * param containing a quote doesn't need any caller-side escaping), but
 * this is substitution-then-parse, not a real prepared-statement/bytecode
 * cache -- the same one-shot "build query text, run it" cost as
 * kdb_exec_sql, just with the substitution done for you instead of by
 * hand with snprintf. params[i].name is ignored (params are positional
 * only). A param's type follows the same rule as an INSERT/UPDATE
 * literal: INT/FLOAT/BOOL/STRING/NULL all render directly; BLOB/ARRAY/
 * OBJECT have no SQL literal syntax (same as kdb_exec_sql's own limit) and
 * fail with an error naming which param.
 *
 *   int64_t min_age = 21;
 *   const char *dept = "eng";
 *   KdbField params[] = { kdb_field_int(NULL, min_age), kdb_field_string(NULL, dept) };
 *   kdb_exec_sql_params(db, "SELECT * FROM employees WHERE age >= ? AND dept = ?",
 *                       params, 2, &rows, NULL);
 */
KdbStatus kdb_exec_sql_params(KumDB *db, const char *sql,
                              const KdbField *params, size_t nparams,
                              KdbRows **rows_out, size_t *affected_out);

#ifdef __cplusplus
}
#endif

#endif
