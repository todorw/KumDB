# KumDB Documentation

Full reference for the C API, the NoSQL filter syntax, the SQL dialect, and
the CLI. For a quick pitch and build instructions, see
[`README.md`](README.md).

## Contents

- [Core concepts](#core-concepts)
- [C API](#c-api)
- [NoSQL filter syntax](#nosql-filter-syntax)
- [Nested values](#nested-values)
- [Transactions](#transactions)
- [SQL](#sql)
- [CLI](#cli)
- [Tools](#tools)
- [KumDB Studio](#kumdb-studio)
- [Installers](#installers)

## Core concepts

A KumDB database is a directory (`data_dir`). Each table is one file in
that directory (`<table>.kdb`), append-only on disk, with deleted rows
marked rather than removed until you `compact`. Tables don't need a schema
up front — the first `kdb_add()` into a new table infers one from whatever
fields you pass — but you can also declare one explicitly with
`kdb_create_table()`, including which columns are indexed.

Every row has three fields the engine manages for you and that you never
set directly: `id` (auto-incrementing, unique per table), `created_at`, and
`updated_at` (both Unix timestamps). All three are filterable like any
other column (see below) — `id=5`, `created_at__gt=...`, etc.

Field values are typed: `INT` (int64), `FLOAT` (double), `BOOL`, `STRING`,
`BLOB` (raw bytes), or a nested `ARRAY`/`OBJECT` (see
[Nested values](#nested-values)). Passing a string through the NoSQL
filter API or the CLI infers its type automatically (`"30"` → int, `"true"`
→ bool, `"1"`/`"0"` → also treated numerically wherever it matters, so
`age=1` matches an int column with value `1`, not just a bool).

## C API

Include `kumdb.h`, link `libkumdb.a`. Every function that can fail sets a
thread-local error you can read with `kdb_last_error()` / `kdb_last_status()`.

### Opening and closing

```c
KumDB *db = kdb_open("./mydata");           // creates the directory if needed
KumDB *db = kdb_open_readonly("./mydata");  // fails if it doesn't exist
kdb_close(db);
```

### Inserting

```c
KdbField fields[] = {
    kdb_field_string("name",  "Alice"),
    kdb_field_int   ("age",   25),
    kdb_field_float ("score", 9.5),
    kdb_field_bool  ("vip",   1),
    kdb_field_null  ("notes"),
    kdb_field_end   ()
};
kdb_add(db, "users", fields);
```

- `kdb_add_validated(db, table, fields, validator_fn, user_data)` — same as
  `kdb_add`, but runs `validator_fn(row, user_data)` first and rejects the
  insert if it returns non-`KDB_OK`.
- `kdb_batch_import(db, table, rows, count, &inserted_out)` — inserts many
  rows (each a `const KdbField *`) under a single lock acquisition.

Blob values: `kdb_field_blob(name, data, len)`. Nested values:
`kdb_field_array(name, items, count)` / `kdb_field_object(name, fields)` —
see [Nested values](#nested-values).

### Querying

```c
const char *filters[] = { "age__gt=21", "name__contains=Ali", NULL };
KdbRows *rows = kdb_find(db, "users", filters);   // NULL filters = all rows
kdb_rows_free(rows);

KdbRow *row = kdb_find_one(db, "users", filters);
kdb_row_free(row);

KdbRow *row = kdb_find_by_id(db, "users", 5);

int64_t n = kdb_count(db, "users", filters);
```

`kdb_find_ex()` adds sorting and pagination on top of `kdb_find()`:

```c
KdbFindOpts opts = { .order_by = "age", .ascending = 0, .limit = 10, .offset = 0 };
KdbRows *rows = kdb_find_ex(db, "users", filters, &opts);
```

### Reading a row's fields

```c
const KdbField *f = kdb_row_get(row, "age");   // generic, check f->type yourself

int64_t     age;    kdb_row_get_int   (row, "age",   &age);
double      score;  kdb_row_get_float (row, "score", &score);
int         vip;    kdb_row_get_bool  (row, "vip",   &vip);
const char *name;   kdb_row_get_string(row, "name",  &name);
const void *data; size_t len;
                     kdb_row_get_blob  (row, "avatar", &data, &len);
```

All the typed getters return `KDB_ERR_NOT_FOUND` if the column doesn't
exist on that row and `KDB_ERR_BAD_TYPE` if it exists but isn't that type.

### Updating and deleting

```c
const char *where[] = { "name=Alice", NULL };
KdbField patch[] = { kdb_field_int("age", 26), kdb_field_end() };
size_t updated = 0;
kdb_update(db, "users", where, patch, &updated);

size_t deleted = 0;
kdb_delete(db, "users", where, &deleted);
```

### Schema management

```c
KdbColumnDef cols[] = {
    { "name", KDB_TYPE_STRING, /*nullable*/ 0, /*indexed*/ 0 },
    { "age",  KDB_TYPE_INT,    1,              1 },
};
kdb_create_table(db, "users", cols, 2);

kdb_add_column (db, "users", "vip", KDB_TYPE_BOOL, /*nullable*/ 1, /*indexed*/ 0);
kdb_drop_column(db, "users", "vip");   // rewrites the whole table file

KdbColumnInfo schema[16]; uint32_t n = 0;
kdb_get_schema(db, "users", schema, 16, &n);

kdb_compact(db, "users");             // strip soft-deleted rows from disk
kdb_drop_table(db, "users");
int exists = kdb_table_exists(db, "users");

const char *names[64]; size_t count = 0;
kdb_list_tables(db, names, 64, &count);   // names point into TLS, valid until the next call
```

`kdb_add_column`/`kdb_add()` on a table with no explicit schema yet both
work — an explicit schema is for when you want to nail down types and
indexes up front, not a requirement.

### Errors, printing, misc

```c
kdb_last_error();    // human-readable string
kdb_last_status();    // KdbStatus enum
kdb_clear_error();

kdb_row_print (row,  stdout);
kdb_rows_print(rows, stdout);
kdb_print_schema(db, "users", stdout);
kdb_version();
```

## NoSQL filter syntax

Filters are plain strings: `"<column>__<operator>=<value>"`. No operator
suffix means equals.

| Operator | Example | Meaning |
|---|---|---|
| *(none)* | `age=30` | equals |
| `__eq` | `age__eq=30` | equals |
| `__neq` | `age__neq=30` | not equals |
| `__gt` | `age__gt=21` | greater than |
| `__gte` | `age__gte=21` | greater than or equal |
| `__lt` | `age__lt=65` | less than |
| `__lte` | `age__lte=65` | less than or equal |
| `__between` | `age__between=18,30` | inclusive range |
| `__in` | `age__in=18,21,30` | matches any value in the list |
| `__contains` | `name__contains=ali` | substring match |
| `__startswith` | `name__startswith=al` | prefix match |
| `__endswith` | `name__endswith=ice` | suffix match |
| `__like` | `name__like=a%_ce` | SQL-style wildcard match (`%` = any run, `_` = one char) |
| `__isnull` | `notes__isnull` | field is null (or missing) |
| `__isnotnull` | `notes__isnotnull` | field is present and not null |

Multiple filters in one call = AND logic. For OR, prefix a filter with
`"OR:"` to start a new AND-group that gets OR'd against everything before
it — same precedence as SQL (AND binds tighter than OR, no parens/nesting):

```c
// age < 18 OR age > 65
const char *filters[] = { "age__lt=18", "OR:age__gt=65", NULL };

// (active=true AND score__gt=90) OR (vip=true)
const char *filters2[] = { "active=true", "score__gt=90", "OR:vip=true", NULL };
```

`id`, `created_at`, and `updated_at` work as filter columns even though
they're not stored as regular fields.

## Nested values

A field can be an array or a nested object instead of a scalar:

```c
KdbField tags[]    = { kdb_field_string(NULL, "vip"), kdb_field_string(NULL, "new") };
KdbField address[] = { kdb_field_string("city", "NYC"), kdb_field_int("zip", 10001), kdb_field_end() };

KdbField fields[] = {
    kdb_field_string("name",    "Alice"),
    kdb_field_array ("tags",    tags, 2),      // array elements: unnamed, count-based
    kdb_field_object("address", address),      // object fields: named, kdb_field_end()-terminated
    kdb_field_end()
};
kdb_add(db, "users", fields);

const char *filters[] = { "name=Alice", NULL };
KdbRow *row = kdb_find_one(db, "users", filters);

const KdbField *items = NULL; size_t count = 0;
kdb_row_get_array(row, "tags", &items, &count);

const KdbField *obj = NULL;
kdb_row_get_object(row, "address", &obj);   // NULL-name-terminated, walk it like any field list
```

Arrays are count-based (no NULL-terminator convention, since a real element
can itself be an unnamed `NULL`-type value and that would collide with a
sentinel). Objects are NULL-name-terminated, same convention as every other
field list in this API, since object keys are never legitimately `NULL`.

Nesting can go arbitrarily deep in principle (arrays of objects of
arrays...), capped at 16 levels and 64 elements per level — generous limits
that exist to stop a corrupt file from making the engine allocate or
recurse without bound, not limits you'll hit in normal use.

`EQ`/`NEQ` compare arrays/objects by deep value equality. Fields inside a
nested `OBJECT` are reachable by dot-path, both as a NoSQL filter
(`"address.city=NYC"`, any operator — `"address.zip__gt=50000"` works
too) and in SQL `WHERE` (`WHERE address.city = 'NYC'`), arbitrarily deep
(`a.b.c`). A path that doesn't resolve — the field's missing, or isn't an
`OBJECT` at some point along the way — just doesn't match, same as a
missing top-level field; it's never an error. No dot-path *into* an
`ARRAY`, only through `OBJECT`s — array elements are unnamed, there's no
name to path through; pull the array out and inspect it yourself for
that. There's no SQL literal syntax for constructing arrays/objects
either (no sane one-line syntax for it); build nested values through the
C API. SQL can still `SELECT`, project, and aggregate the resulting
columns fine, it just can't construct new ones from a literal.

Old data files (format 1.0, written before nested values existed) open
exactly as before — nested values only appear starting at format 1.1, and
none of the existing type encodings changed, so there's nothing to migrate.

## Transactions

`kdb_tx_*` groups `kdb_add`/`kdb_update`/`kdb_delete` calls across one or
more tables into a single all-or-nothing unit:

```c
KdbTx *tx = kdb_tx_begin(db);

KdbField f1[] = { kdb_field_string("from", "alice"), kdb_field_int("amount", 50), kdb_field_end() };
kdb_tx_add(tx, "transfers", f1);

const char *where[] = { "name=alice", NULL };
KdbField patch[] = { kdb_field_int("balance", 50), kdb_field_end() };
size_t updated = 0;
kdb_tx_update(tx, "accounts", where, patch, &updated);

if (kdb_last_status() == KDB_OK)
    kdb_tx_commit(tx);
else
    kdb_tx_rollback(tx);
```

`kdb_tx_add`/`kdb_tx_update`/`kdb_tx_delete` are the same operations as
their non-tx counterparts, just tracked by `tx`. If any of them fails, the
transaction is marked failed and `kdb_tx_commit()` will refuse — call
`kdb_tx_rollback()` instead. Either call ends the transaction and frees
`tx`; don't reuse it afterward.

This is scoped to a single writer, same as the rest of KumDB — it's not a
multi-writer isolation mechanism. What it does give you:

- **Rollback**: `kdb_tx_rollback()` undoes every change made across
  however many tables the transaction touched (including dropping a table
  that the transaction itself created).
- **Crash safety**: if the process dies anywhere between `kdb_tx_begin()`
  and a completed `kdb_tx_commit()`, the next `kdb_open()` automatically
  finishes the job on its own — rolling back an interrupted transaction, or
  finishing cleanup of one that had already committed. A table is never
  left half-migrated. This has been verified with real crash simulations
  (killing a process mid-transaction and mid-commit), not just unit tests.

Mechanically: the first time a transaction touches a table, it backs up
that table's file (or, if the table is new, just remembers to drop it on
rollback). This means a transaction costs a full copy of each table it
touches on first touch — fine for coordinating a handful of tables, not
meant for wrapping bulk operations on a huge one.

## SQL

`kdb_exec_sql(db, sql, &rows_out, &affected_out)` runs one statement and
hits the exact same storage/query engine the NoSQL API uses.

```sql
CREATE TABLE t (col TYPE [NOT NULL] [INDEX], ...)
ALTER TABLE t ADD [COLUMN] col TYPE [NOT NULL] [INDEX]
ALTER TABLE t DROP [COLUMN] col
DROP TABLE t

CREATE VIEW v AS SELECT ...
DROP VIEW v

INSERT INTO t (col, ...) VALUES (val, ...)

[WITH name AS (SELECT ...) [, name2 AS (SELECT ...)]*]
SELECT [DISTINCT] * | item, ... FROM t | (SELECT ...) [[AS] alias]
    [[INNER|LEFT [OUTER]] JOIN t2 [[AS] alias2] ON a.col = b.col [AND ...]]*
    [WHERE cond [AND|OR cond ...]]
    [GROUP BY col, ...]
    [HAVING cond [AND|OR cond ...]]
    [(UNION|INTERSECT|EXCEPT) [ALL] SELECT ...]*
    [ORDER BY col [ASC|DESC], ...]
    [LIMIT n [OFFSET m]]

UPDATE t SET col = val, ... [WHERE cond [AND|OR cond ...]]
DELETE FROM t [WHERE cond [AND|OR cond ...]]
```

**Comments:** `-- to end of line` and `/* block, can span lines */` are
both stripped like whitespace, anywhere a token could otherwise start. An
unterminated `/*` just eats to the end of the string rather than erroring
on the comment itself — you'll get whatever "ran out of input mid-
statement" error the missing content after it would've caused anyway.

**`ORDER BY`** (the top-level clause, after `GROUP BY`/`UNION`/etc, not
`OVER`'s own `ORDER BY`) takes one or more comma-separated columns, each
with its own optional `ASC`/`DESC` — `ORDER BY region ASC, amount DESC`
sorts by `region` first, breaking ties with `amount`, same as real SQL.
Capped at 4 columns. Applies once, after everything else (`GROUP BY`,
`HAVING`, `UNION`) — sorting a `UNION`'s arm individually isn't possible,
same as before.

**Types:** `INT`/`INTEGER`, `FLOAT`/`REAL`/`DOUBLE`, `BOOL`/`BOOLEAN`,
`TEXT`/`STRING`/`VARCHAR`/`CHAR` (length specs like `VARCHAR(50)` are
accepted and ignored — KumDB strings aren't fixed-width), `BLOB`.

**Reserved names:** `id`, `created_at`, `updated_at` are managed by the
engine. Declaring them in `CREATE TABLE`/`ALTER TABLE ADD` is silently
skipped (or rejected for `ADD`) rather than erroring, so a copy-pasted
`id INTEGER PRIMARY KEY` doesn't blow up on you.

**`WHERE`** supports `=`, `!=`/`<>`, `>`, `>=`, `<`, `<=`,
`BETWEEN a AND b`, `IN (a, b, c)`, `IS [NOT] NULL`, `LIKE 'pat'`
(standard SQL wildcards: `%` matches any run of characters including
none, `_` matches exactly one, anywhere in the pattern — no `ESCAPE`
clause, so there's no way to match a literal `%`/`_`), and
`AND`/`OR` at standard SQL precedence (`AND` binds tighter than `OR`:
`a=1 AND b=2 OR c=3` means `(a=1 AND b=2) OR (c=3)`) — parenthesize to
override that, nested as deep as you like (up to 16 levels):
`(a=1 OR b=2) AND c=3`. Same syntax works in `UPDATE`/`DELETE`'s `WHERE`
too. Parens are free on a `SELECT`'s `WHERE`/`HAVING` (evaluated in
memory, same as any post-`JOIN`/view filter); on `UPDATE`/`DELETE` a
parenthesized `WHERE` costs a full table scan to resolve which rows match
before touching them, since the storage engine's own filter pushdown only
understands flat `OR`'d `AND`-groups — fine for the row counts this engine
targets, not something to reach for on a huge table repeatedly.

**Subqueries**: `=`/`!=`/`>`/`>=`/`<`/`<=` accept `(SELECT ...)` as a
scalar right-hand side (must return exactly one row, one column), and `IN`
accepts `(SELECT col FROM ...)` in place of a literal list (any number of
rows, still exactly one column).

```sql
SELECT name FROM employees WHERE salary = (SELECT MAX(salary) FROM employees)
SELECT name FROM employees WHERE dept IN (SELECT dept FROM managers)
```

These can also be **correlated** — the inner query can reference the outer
row, qualified with the outer query's own alias (explicit `AS`, a bare
alias, or just the table name), same as `EXISTS` below:

```sql
SELECT name FROM employees e
    WHERE salary = (SELECT MAX(salary) FROM employees e2 WHERE e2.dept = e.dept)

SELECT name FROM employees e
    WHERE name IN (SELECT emp_name FROM orders o WHERE o.emp_name = e.name)
```

Whether a subquery is correlated is decided per-subquery, automatically,
by whether its text actually references the outer alias — nothing new to
write. A non-correlated one (like the first two examples above) keeps
running once, up front, exactly as before; a correlated one re-runs once
per outer row, the same real cost `EXISTS` already accepts (fine at this
engine's target row counts, not something to reach for in a tight loop
over a huge table). Correlated scalar/`IN` subqueries compose with
`AND`/`OR`, nest in parens, and work in `UPDATE`/`DELETE`'s `WHERE` too,
but — like `EXISTS` — aren't supported in `HAVING` (no real outer row
there to correlate against) or inside a `CASE WHEN` condition.

**`EXISTS`/`NOT EXISTS`** accept a correlated `(SELECT ...)` in `WHERE` —
unlike the scalar/`IN` subqueries above, the inner query *can* reference
the outer row, qualified with the outer query's own alias (explicit
`AS`, a bare alias, or just the table name if neither was given). Only
row existence matters, not what's projected, so `SELECT *` is the usual
choice:

```sql
SELECT name FROM users u WHERE EXISTS (SELECT * FROM orders o WHERE o.user_id = u.id)
SELECT name FROM users WHERE NOT EXISTS (SELECT * FROM orders o WHERE o.user_id = users.id)
```

Mechanically, the inner query is re-run once per outer row (real
correlated-subquery cost — fine at this engine's target row counts, not
something to reach for in a tight loop over a huge table): the outer
row's alias-qualified references get substituted with that row's actual
values, then the now-fully-self-contained `SELECT` runs fresh, same as
any other. `EXISTS`/`NOT EXISTS` can combine with `AND`/`OR` and nest in
parens like any other condition, and works in `UPDATE`/`DELETE`'s `WHERE`
too (via the same full-scan `id` targeting a parenthesized `WHERE` there
uses). Not supported in `HAVING` — it filters aggregated aliases like
`total` from `SUM(amount) AS total`, not a real row to correlate against.

**`JOIN`** (`INNER`/`LEFT`) matches rows via `ON`, a conjunction of
`col = col` equalities — no `OR`, no comparing to a literal in `ON`
(that's what `WHERE`, applied after the join, is for). Chain as many
`JOIN` clauses as you want; each one matches against everything
accumulated so far, so a later `ON` can reference any earlier alias in
the chain, not just the table right before it:

```sql
SELECT u.name, o.item
    FROM users AS u
    JOIN orders AS o ON u.id = o.user_id
    WHERE o.item = 'widget'

SELECT u.name, o.item FROM users AS u LEFT JOIN orders AS o ON u.id = o.user_id

SELECT u.name, o.item, r.stars
    FROM users AS u
    JOIN orders AS o ON u.id = o.user_id
    LEFT JOIN reviews AS r ON o.item = r.order_item
```

Every column reference anywhere after a `JOIN` — the `SELECT` list, `ON`,
`WHERE`, `ORDER BY` — must be table-qualified: `alias.col` (or
`table.col` if you didn't give it an alias). There's no unqualified
fallback, on purpose: guessing which side a bare column name meant when
more than one table could have one is exactly the kind of silent
ambiguity worth refusing outright. This includes three synthetic columns
every table gets for joining against — `alias.id`, `alias.created_at`,
`alias.updated_at` — since those normally aren't part of a row's field
list but are common join keys (`ON orders.user_id = users.id`).
`SELECT *` after a `JOIN` shows every qualified column from every table,
including those three.

Aliasing itself doesn't need `AS` — `FROM orders o` works the same as
`FROM orders AS o` (same for a `JOIN` target), except for a handful of
words (`WHERE`, `GROUP`, `HAVING`, `ORDER`, `LIMIT`, `UNION`, `JOIN`,
`INNER`, `LEFT`, `ON`, `AS`) that always mean the keyword, never a bare
alias — `FROM t WHERE ...` parses `WHERE` as the clause, not as `t`'s
alias, exactly like real SQL's reserved-word handling.

A query with no `JOIN` at all can still qualify its own columns with its
own alias if one's in scope (explicit, bare, or just the table name) —
`SELECT * FROM users u WHERE u.name = 'alice'` works the same as the
unqualified `WHERE name = 'alice'`. Mostly useful for `EXISTS`'s inner
query, where qualifying is the natural way to write a self-reference
alongside a correlated one to the outer row (see below).

`LEFT JOIN` keeps every row accumulated so far; one with no `ON` match
against that step's table gets every column from that table set to
`NULL` (including `alias.id`) instead of being dropped, same as real
`LEFT JOIN`. `INNER JOIN` (or just `JOIN`) drops unmatched rows.

`GROUP BY`/aggregate functions work fine after a `JOIN` — group and
aggregate on qualified columns same as any other post-`JOIN` reference:

```sql
SELECT u.name, COUNT(*) AS order_count
    FROM users AS u
    JOIN orders AS o ON u.id = o.user_id
    GROUP BY u.name
```

Every table is always fetched in full — there's no filter pushed into the
join itself, `WHERE` runs afterward over the combined rows. Fine for the
row counts this engine targets, not something to reach for on huge tables.

**`SELECT` items** can be a plain column, `*`, an aggregate call —
`COUNT(*)`, `COUNT(col)`, `SUM(col)`, `AVG(col)`, `MIN(col)`, `MAX(col)` —
or a `CASE` expression (below), optionally renamed with `AS alias` (default
alias is e.g. `SUM(amount)`, or `case` for an unaliased `CASE`).
Without `GROUP BY`, one or more aggregate items collapse the result into a
single summary row. `GROUP BY` takes one or more comma-separated columns —
`GROUP BY region` or `GROUP BY region, product` — and you get one row per
distinct combination of values across all of them; every other selected
item must be an aggregate call — same rule real SQL uses. `SELECT col FROM
t GROUP BY col` with no aggregate at all is a valid way to get distinct
values. `SUM`/`AVG` always come back as `FLOAT` regardless of the source
column's type.

**Scalar functions** can also be a `SELECT` item, freely alongside plain
columns/`CASE`/window functions (but not combined with `GROUP BY` or a
plain aggregate in the same `SELECT`, same restriction `CASE` and window
functions have):

```sql
SELECT UPPER(name) AS n, ROUND(price, 2) AS p, CONCAT(city, ', ', country) AS loc
    FROM t
```

| Function | |
|---|---|
| `UPPER(x)`, `LOWER(x)` | case-fold a string |
| `LENGTH(x)` | string length in bytes, as `INT` |
| `TRIM(x)` | strip leading/trailing whitespace |
| `SUBSTR(x, start[, len])` / `SUBSTRING(...)` | substring, 1-based `start` like standard SQL |
| `CONCAT(a, b, ...)` | join as strings (2-4 args) |
| `ROUND(x[, ndigits])` | always comes back `FLOAT`, `ndigits` defaults to 0 |
| `ABS(x)` | preserves `INT` vs `FLOAT` |
| `CEIL(x)` / `CEILING(x)`, `FLOOR(x)` | always `INT` |
| `MOD(a, b)` | `INT` if both args are `INT`, `FLOAT` otherwise |
| `COALESCE(a, b, ...)` | first non-`NULL` argument (2-4 args) |
| `NULLIF(a, b)` | `NULL` if `a` equals `b`, else `a` |
| `CAST(x AS type)` | `INT`/`FLOAT`/`BOOL`/`TEXT` (same type names as `CREATE TABLE`) |
| `NOW()` | current time, epoch seconds as `INT` |

Every argument is a plain column reference or a literal — never another
function call, an aggregate, or a `CASE` (no arbitrary expression
nesting, same "keep it flat" scope every other multi-part construct in
this dialect already uses). A type-mismatched argument (`UPPER()` on a
non-string column, say) produces `NULL` for that row rather than erroring
the whole query — same latitude a bad comparison in `WHERE` already has.
`CAST` similarly fails soft: a string that doesn't look like a number
casts to `0`/`0.0` rather than erroring. `NULL` in, `NULL` out for every
function except `COALESCE` (that's the point of it) and `NULLIF`.

**Window functions**: `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, and
`COUNT`/`SUM`/`AVG`/`MIN`/`MAX` all work as window functions with
`OVER ([PARTITION BY col, ...] [ORDER BY col [ASC|DESC], ...])` — unlike
`GROUP BY`, rows aren't collapsed; every row keeps its own value alongside
whatever else is selected:

```sql
SELECT region, rep, amount,
       RANK() OVER (PARTITION BY region ORDER BY amount DESC) AS rank_in_region,
       SUM(amount) OVER (PARTITION BY region) AS region_total
    FROM sales
```

`RANK`/`DENSE_RANK` give tied rows (equal `ORDER BY` values within a
partition) the same rank; `RANK` leaves a gap afterward (`1, 1, 3`),
`DENSE_RANK` doesn't (`1, 1, 2`). A windowed aggregate covers the whole
partition, not a running/cumulative total — no `ROWS`/`RANGE BETWEEN`
frame clause. `PARTITION BY`/`ORDER BY` are both optional (omit
`PARTITION BY` and the whole result set is one partition); omitting
`ORDER BY` inside `OVER` for `RANK`/`DENSE_RANK` means everything ties
(rank 1 for every row in the partition) — same as real SQL. `ROW_NUMBER`/
`RANK`/`DENSE_RANK` always need `OVER` (there's no non-window use of
them); `COUNT`/`SUM`/`AVG`/`MIN`/`MAX` use `OVER` to switch from their
`GROUP BY`-collapsing form to this one — a `SELECT` can't mix a window
function with `GROUP BY` or a plain (non-window) aggregate. Works fine
after a `JOIN`, on qualified columns, same as anything else there.

**`CASE`** as a `SELECT` item: `CASE WHEN cond THEN val [WHEN cond THEN
val ...] [ELSE val] END`. First matching `WHEN` wins; with no `ELSE` and
no match, the value is `NULL`. Each `WHEN` condition is one or more
`WHERE`-style conditions (same operators: `=`, `BETWEEN`, `IN`, `LIKE`,
`IS NULL`, etc) combined with `AND`/`OR` — same precedence as `WHERE`
(`AND` binds tighter), but no parens within one `WHEN` and no more than 3
conditions per `WHEN`; no more than 4 `WHEN` branches either. `THEN`/`ELSE`
values are literals (number, string, `true`/`false`, `null`) evaluated
once at parse time, not column references. Works fine after a `JOIN`
(conditions can reference qualified columns like any other `WHERE`-style
condition), but not combined with `GROUP BY` or aggregate functions:

```sql
SELECT name, CASE WHEN age < 18 THEN 'minor' WHEN age < 65 THEN 'adult' ELSE 'senior' END AS category
    FROM people

SELECT name, CASE WHEN age >= 18 AND age < 65 THEN 'adult' ELSE 'other' END AS category
    FROM people
```

**`HAVING`** filters the aggregated/grouped output (same condition syntax
as `WHERE`, evaluated against the SELECT list's aliases -- `HAVING total >
90` refers to `SUM(amount) AS total`, not the raw `amount` column). Only
valid with `GROUP BY` or an aggregate function; requiring it without either
is a syntax error, same as real SQL. Runs after grouping and before
`ORDER BY`/`LIMIT`, so `HAVING ... ORDER BY ... LIMIT ...` limits the
already-`HAVING`-filtered set.

**`UNION`/`UNION ALL`** chains multiple `SELECT`s into one result: `UNION`
dedupes across all arms (same rules as `DISTINCT`), `UNION ALL` keeps every
row including duplicates. Every arm must select the same number of
columns; output column names come from the first arm that returned at
least one row. `ORDER BY`/`LIMIT` can only appear once, after the last
arm, and apply to the combined result, not to any one arm. Mixing `UNION`
and `UNION ALL` in the same chain isn't supported -- pick one for the
whole statement (which one binds first is a real ambiguity without
parenthesized subqueries, which KumDB's SQL doesn't have).

**`INTERSECT`/`INTERSECT ALL`/`EXCEPT`/`EXCEPT ALL`** work the same way,
with set semantics instead of concatenation: `INTERSECT` keeps only
distinct rows present in *every* arm, `EXCEPT` keeps distinct rows from
the first arm that don't appear in any later one. The `ALL` variants use
multiset semantics instead of deduping -- a run of duplicate rows on one
side survives up to however many matching rows the other side actually
has (`INTERSECT ALL`: the smaller of the two counts; `EXCEPT ALL`: the
left's count minus the right's, floored at zero):

```sql
SELECT dept FROM current_employees INTERSECT SELECT dept FROM managers
SELECT id FROM orders EXCEPT SELECT id FROM refunded_orders
```

Same column-count/output-naming rules as `UNION`, and same restriction on
mixing -- a chain uses exactly one operator (`UNION`/`INTERSECT`/`EXCEPT`)
and one `ALL`-ness throughout; real SQL gives `INTERSECT` higher
precedence than `UNION`/`EXCEPT` when they're mixed in one statement, but
that's not implemented here (mixing errors out instead of silently
picking a precedence you didn't ask for).

**`SELECT DISTINCT`** dedupes the result by the exact set of selected
columns (after projection, so `SELECT DISTINCT col` dedupes on `col`
alone, not the whole row). `id`/`created_at`/`updated_at` aren't part of
what `*` projects (same as plain `SELECT *`), so `SELECT DISTINCT *` on a
table with repeated column values still collapses them. Works with `GROUP
BY` too (dedupes the aggregated output, rarely useful but not rejected).

**`CREATE VIEW v AS SELECT ...`** stores a named query; `SELECT ... FROM
v` re-runs it fresh every time (not materialized/cached). `CREATE VIEW`
validates the underlying query immediately -- a typo or a reference to a
table that doesn't exist yet fails at creation, not at first use.
`WHERE`/`ORDER BY`/`LIMIT`/`DISTINCT`/aggregates all work on top of a view
same as a real table:

```sql
CREATE VIEW eng_staff AS SELECT name, salary FROM employees WHERE dept = 'eng'
SELECT name FROM eng_staff WHERE salary > 130000 ORDER BY name
SELECT COUNT(*), SUM(salary) FROM eng_staff
DROP VIEW eng_staff
```

A view can't be `JOIN`ed (either as the left side or a `JOIN` target) --
not supported yet. Views are stored as rows in a reserved internal table,
`__kumdb_views__`; it'll show up in `kdb_list_tables()`/the CLI's `tables`
command like any other table (querying it directly works fine, it's just
not hidden), so don't name a real table that.

**`WITH name AS (SELECT ...) SELECT ... FROM name`** (a CTE) is a view
scoped to one statement instead of persisted -- same validate-immediately
and re-run-fresh-every-time behavior as `CREATE VIEW`, same `JOIN`
restriction, gone the moment the statement finishes (success or error):

```sql
WITH big_orders AS (SELECT * FROM orders WHERE amount > 1000)
SELECT customer, amount FROM big_orders ORDER BY amount DESC
```

Chain more than one with a comma; a later one can reference an earlier
one (each is validated and made visible in declaration order), but not
the other way around, and not itself -- no `RECURSIVE`:

```sql
WITH regional AS (SELECT region, amount FROM sales WHERE region = 'east'),
     totals   AS (SELECT region, SUM(amount) AS total FROM regional GROUP BY region)
SELECT region FROM totals WHERE total > 10000
```

`WITH` only ever precedes a `SELECT` -- not `UPDATE`/`DELETE`/`INSERT`.

**`FROM (SELECT ...) AS alias`** (a derived table) works the same way --
a subquery standing in for a real table, needing its own alias (there's
no table name to fall back on) since that's the only thing identifying
it:

```sql
SELECT region FROM (SELECT region, SUM(amount) AS total FROM sales GROUP BY region) AS totals
    WHERE total > 10000
```

`WHERE`/`ORDER BY`/`LIMIT`/`DISTINCT`/aggregates all work on top of a
derived table same as a real table, same as `CREATE VIEW`. Only valid as
the primary `FROM` target, not a `JOIN` target -- same restriction a real
view has. Unlike a `WITH` CTE, nothing gets registered anywhere even
temporarily; it's parsed and re-run inline every time, which also means
(unlike `CREATE VIEW`) it isn't validated until the moment it actually
runs, not a moment earlier.

```sql
CREATE TABLE sales (region TEXT, amount FLOAT)
INSERT INTO sales (region, amount) VALUES ('east', 100.0)
SELECT region, COUNT(*), SUM(amount) AS total
    FROM sales
    GROUP BY region
    ORDER BY total DESC
```

Also callable from C directly via `kdb_exec_sql()` (`sql.h`).

## CLI

```bash
./build/bin/kumdb_cli ./mydata
```

### NoSQL commands

```
open <dir>                          open a database
close                               close the current database
tables                              list tables
schema <table>                      show schema
add <table> <k=v> [k=v ...]         insert a record (value can be @path for a blob)
find <table> [filter ...] [order_by=col] [order=asc|desc] [limit=N] [offset=N]
findbyid <table> <id>               find a single record by id
count <table> [filter ...]          count records
update <table> where <k=v> set <k=v>  update records
import <table> <file>               bulk-insert k=v lines from a file
delete <table> <filter> [...]       delete records
compact <table>                     compact table file
drop <table>                        drop table
version                             show CLI/engine version
help                                show command help
quit / exit                         exit
```

Filters/`where` clauses are AND by default; prefix a filter with `OR:` to
start a new OR'd group (same convention as the C API).

### SQL commands

Prefix any line with `sql` to run it as one SQL statement:

```
kumdb> sql CREATE TABLE users (name TEXT NOT NULL, age INT INDEX)
kumdb> sql INSERT INTO users (name, age) VALUES ('Alice', 30)
kumdb> sql SELECT * FROM users WHERE age > 21 ORDER BY age DESC LIMIT 10
kumdb> sql SELECT region, COUNT(*), SUM(amount) AS total FROM sales GROUP BY region ORDER BY total DESC
kumdb> sql ALTER TABLE users ADD COLUMN vip BOOL
kumdb> sql UPDATE users SET age = 31 WHERE name = 'Alice'
kumdb> sql DELETE FROM users WHERE age < 18
kumdb> sql ALTER TABLE users DROP COLUMN vip
kumdb> sql DROP TABLE users
```

## Tools

```bash
# Benchmark (default 10k rows)
./build/bin/bench 50000 ./mydata

# Dump table contents
./build/bin/dump ./mydata users
./build/bin/dump ./mydata users --csv
./build/bin/dump ./mydata users --json --limit 50
```

`dump --json` serializes nested array/object fields as real JSON, not a
placeholder.

## KumDB Studio

A Qt desktop app (Windows/Mac/Linux): table browser, editable datasheet
grid, and a query console that toggles between NoSQL filters and SQL.
Array/object columns show as read-only text in the grid (editing a
nested-value cell in place isn't supported — it would just overwrite the
structured value with plain text). See [`app/README.md`](app/README.md)
for build instructions.

## Installers

`scripts/package.sh` builds Studio in release mode and packages it into two
tiers with CPack:

- **easy** -- just `kumdb_studio`, for someone who wants the desktop app
  and nothing else.
- **full** -- easy + `kumdb_cli`, `kumdb_dump`, and the C headers/static
  lib, for anyone who also wants the command line tools or to embed KumDB
  in their own program.

```bash
./scripts/package.sh
# app/build/KumDBStudio-<version>-<platform>-easy.*
# app/build/KumDBStudio-<version>-<platform>-full.*
```

Run it natively on each target OS -- it doesn't cross-compile Qt. What you
get per platform:

- **Linux**: a `.tar.gz` and a `.deb` for each tier.
- **macOS**: a `.dmg` (CPack's DragNDrop generator) for each tier. Run
  `macdeployqt` on the built `.app` bundle before packaging if you need the
  Qt frameworks bundled in (otherwise the `.dmg` assumes Qt is already
  installed on the target machine).
- **Windows**: an NSIS installer `.exe` and a `.zip` for each tier. Run
  `windeployqt` on the built `kumdb_studio.exe` before packaging for the
  same reason -- CPack doesn't chase down and bundle DLL dependencies for
  you, `windeployqt` does.

The engine itself (`src/*.c`) compiles clean on Windows too now --
`include/platform.h` swaps the POSIX bits (fcntl locking, fsync, mkdir)
for their Windows equivalents (LockFileEx, \_commit, \_mkdir) under
`#ifdef _WIN32`, verified by cross-compiling the whole library and the CLI
tools with mingw-w64. Locking semantics match (one exclusive whole-file
holder either way); the one thing that's a deliberate no-op on Windows is
the post-rename parent-directory fsync durability trick, which is a
POSIX-only idiom with no equivalent (NTFS handles this differently).
