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
`kdb_create_table()`, including which columns are indexed, `NOT NULL`, or
`UNIQUE` (both actually enforced -- see Schema management below).

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

### Aggregation pipeline

`kdb_aggregate()` runs a sequence of stages over a table's rows, each
stage's output feeding the next — the NoSQL C API's counterpart to SQL's
`WHERE`/`GROUP BY`/`ORDER BY`/`LIMIT` (see the SQL section below), built
for direct C use with no SQL text involved:

```c
const char *filters[] = { "region=east", NULL };
KdbAccumulator accs[] = {
    { "total",      KDB_ACC_SUM,   "amount" },
    { "avg_amount", KDB_ACC_AVG,   "amount" },
    { "n",          KDB_ACC_COUNT, NULL     },
};
const char *group_by[] = { "region", NULL };

KdbStage stages[] = {
    { .type = KDB_STAGE_MATCH, .as = { .match_filters = filters } },
    { .type = KDB_STAGE_GROUP, .as = { .group = { .group_by = group_by,
                                                   .accumulators = accs,
                                                   .n_accumulators = 3 } } },
    { .type = KDB_STAGE_SORT,  .as = { .sort = { .fields = {"total"}, .ascending = {0}, .n_fields = 1 } } },
    { .type = KDB_STAGE_LIMIT, .as = { .limit = 10 } },
};

KdbRows *rows = NULL;
kdb_aggregate(db, "sales", stages, 4, &rows);
```

Six stage types, run in whatever order the array lists them (a pipeline
can use the same stage type more than once — two `$match`es, `$sort`
after `$group`, etc.):

- **`KDB_STAGE_MATCH`** — keeps only rows matching `filters` (same raw
  filter-string format `kdb_find` takes, including `OR:`-prefixed groups).
- **`KDB_STAGE_GROUP`** — groups by `group_by` (a `NULL`-terminated field
  list; `NULL`/empty collapses everything into one group, same as a
  `GROUP BY`-less SQL aggregate), reducing each group to one output row.
  `accumulators` computes `KDB_ACC_SUM`/`AVG`/`MIN`/`MAX` (always `FLOAT`
  for `SUM`/`AVG`) or `KDB_ACC_COUNT` (rows, or a field's non-`NULL`
  values if `source_field` is set) per group, alongside the group-by
  fields themselves. Up to `KDB_AGG_MAX_GROUP_KEYS` (4) group-by fields
  and `KDB_AGG_MAX_ACCUMULATORS` (8) accumulators.
- **`KDB_STAGE_SORT`** — multi-key sort (up to `KDB_AGG_MAX_SORT_KEYS`,
  4), `ascending[i]` `1`/`0` per field.
- **`KDB_STAGE_LIMIT`** / **`KDB_STAGE_SKIP`** — cap the row count / drop
  the first `n` rows, same as `KdbFindOpts`.
- **`KDB_STAGE_PROJECT`** — keeps only the named fields (a `NULL`-
  terminated list), in that order; a missing field is silently skipped.

`$group`'s `group_by`/accumulator `source_field`s and `$sort`'s fields
must be real stored fields, not the `id`/`created_at`/`updated_at`
pseudo-columns (`$match`'s filters and `$project`'s fields work fine with
them, same as `kdb_find`/`kdb_find_ex` always have). Every stage runs in
memory over the whole table (`kdb_aggregate` fetches it all up front,
same as `sql__exec_select_core` does under the hood for SQL) — fine at
the row counts this engine targets, not optimized for huge tables.

### Full-text search

`kdb_text_search()` tokenizes a query into lowercased, alphanumeric words
(no stemming, no stop-word removal, no quoted-phrase search — every
space-separated word is its own independent term) and scans a table's
`STRING` fields, ranking rows by how many times each term occurs as a
whole word (not a substring):

```c
KdbRows *rows = NULL;
kdb_text_search(db, "articles", "fox dog", NULL, &rows);
```

With no `opts`, every `STRING` field on the table is searched and a row
is kept only if **every** query term appears somewhere in it
(`KDB_TEXT_MATCH_ALL`, the default — `mode`'s zero value). Pass
`KDB_TEXT_MATCH_ANY` to keep a row if **any** term matches instead, name
specific fields via `opts->fields` (a `NULL`-terminated list) instead of
searching every `STRING` field, and/or cap the result count via
`opts->limit`:

```c
const char *title_only[] = { "title", NULL };
KdbTextSearchOpts opts = { .fields = title_only, .mode = KDB_TEXT_MATCH_ANY, .limit = 10 };
kdb_text_search(db, "articles", "fox dog", &opts, &rows);
```

Results come back sorted by relevance (total term occurrences across the
searched fields, descending; ties broken by `id` ascending), each row
carrying its score in an extra `"_score"` `FLOAT` field. Matching is
case-insensitive and whole-word — searching `"fox"` doesn't match a field
containing only `"foxes"`. There's no persistent index behind this: every
call re-tokenizes the whole table in memory, the same scope `kdb_
aggregate` already accepts. `KDB_ERR_BAD_ARG` if the table has no
`STRING` field to search (name one explicitly via `opts->fields` if it
does have one, just none picked up by the default "every `STRING`
field" scan — which shouldn't normally happen, but a column added since
the table was created some other way is worth knowing about).

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
    { "email", KDB_TYPE_STRING, /*nullable*/ 1, /*indexed*/ 0, /*unique*/ 1 },
    { "age",   KDB_TYPE_INT,    1,              1,             0 },
};
kdb_create_table(db, "users", cols, 2);

kdb_add_column (db, "users", "vip", KDB_TYPE_BOOL, /*nullable*/ 1, /*indexed*/ 0, /*unique*/ 0);
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
work — an explicit schema is for when you want to nail down types,
indexes, and constraints up front, not a requirement.

`nullable=0` and `unique=1` are both really enforced (not just recorded
metadata): `kdb_add`/`kdb_update` reject a NULL/missing value for a
NOT-NULL column, and reject a value that already exists elsewhere in a
unique column (a `NULL` value never conflicts with another `NULL`, same
convention real SQL `UNIQUE` constraints use). `unique` implies
`indexed` — a unique column always gets a real index to check against.
Turning either flag on later (`kdb_alter_column_nullable`/
`kdb_alter_column_unique`, or SQL's `ALTER COLUMN SET/DROP NOT NULL`/
`UNIQUE` below) only affects rows written from that point on — existing
rows that already violate the new rule aren't retroactively checked or
rewritten.

`kdb_add_foreign_key(db, table, col, ref_table, ref_col)` and
`kdb_add_check_constraint(db, table, col, op, literal)` add real,
enforced constraints the same way from the C API directly (`kdb_drop_
foreign_key` removes one) — see `ALTER TABLE`'s `REFERENCES`/`FOREIGN
KEY`/`CHECK` forms below for the full semantics (RESTRICT-only foreign
keys, the six-comparison-operator CHECK, both enforced by `kdb_add`/
`kdb_update`/`kdb_delete` regardless of whether they were declared from
SQL or here). `kdb_batch_import` bypasses both, same as it bypasses NOT
NULL/UNIQUE.

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

**Savepoints**: `kdb_tx_savepoint(tx, name)` establishes a named
checkpoint inside an open transaction, up to `KDB_TX_MAX_SAVEPOINTS` (8)
deep. `kdb_tx_rollback_to_savepoint(tx, name)` undoes every change made
since that savepoint (including anything done under savepoints nested
inside it, which are discarded too), but leaves the transaction — and the
named savepoint itself — open, so it can be rolled back to again.
`kdb_tx_release_savepoint(tx, name)` keeps the changes and just forgets
the name (and any names nested inside it). Mechanically this works the
same way as the whole-transaction backup, just scoped: the first time a
table is touched since a given savepoint was created, that savepoint gets
its own backup of the table, taken independently of the transaction's own
outer backup and of any other active savepoint's. Crash recovery on the
next `kdb_open()` cleans up any leftover savepoint backup files the same
way it cleans up interrupted whole-transaction ones.

**From SQL**: `BEGIN` (`TRANSACTION`/`WORK` optional, and `START
TRANSACTION` works too) opens a `kdb_tx_*` transaction on the connection;
every `INSERT`/`UPDATE`/`DELETE` after that runs through it instead of the
plain non-transactional call, until `COMMIT` or `ROLLBACK` ends it —
same semantics as the C API above, just without threading a `KdbTx*`
through by hand:

```sql
BEGIN
INSERT INTO accounts (name, balance) VALUES ('alice', 50)
UPDATE accounts SET balance = balance - 10 WHERE name = 'alice'
COMMIT
```

No nested transactions (`BEGIN` while one is already open errors —
`COMMIT`/`ROLLBACK` it first), and `COMMIT`/`ROLLBACK` with no open
transaction errors too. Schema changes (`CREATE`/`ALTER`/`DROP`) aren't
wrapped by `kdb_tx_*`, so they're rejected while a transaction is open
rather than silently running outside it, where `ROLLBACK` wouldn't
actually undo them. A transaction still open when the connection is
closed is rolled back automatically (same outcome a crash mid-transaction
already gets from the next `kdb_open()`, just immediate instead of
deferred to next open).

`SAVEPOINT name` / `RELEASE [SAVEPOINT] name` / `ROLLBACK TO [SAVEPOINT]
name` are all supported, and each needs an open transaction (`BEGIN`
first):

```sql
BEGIN
INSERT INTO accounts (name, balance) VALUES ('alice', 50)
SAVEPOINT before_bonus
UPDATE accounts SET balance = balance + 1000000 WHERE name = 'alice'
ROLLBACK TO SAVEPOINT before_bonus
COMMIT
```

`ROLLBACK TO SAVEPOINT` undoes everything since that savepoint (including
any savepoints nested inside it, which are discarded too) but leaves the
savepoint itself and the surrounding transaction open, so it can be
rolled back to again. `RELEASE SAVEPOINT` keeps the changes made and just
forgets the name (and any names nested inside it). Same underlying
mechanism as `kdb_tx_savepoint`/`kdb_tx_rollback_to_savepoint`/
`kdb_tx_release_savepoint` above, just driven from SQL text instead of a
`KdbTx*` handle.

## SQL

`kdb_exec_sql(db, sql, &rows_out, &affected_out)` runs one statement and
hits the exact same storage/query engine the NoSQL API uses.

`kdb_exec_sql_params(db, sql, params, nparams, &rows_out, &affected_out)`
is the same thing with bound-parameter placeholders: `?` (positional) and
`$1`/`$2`/... (explicit 1-based index, so a param can be reused or
referenced out of order) can appear anywhere a literal would, and get
substituted with a properly-escaped rendering of the corresponding
`KdbField` before the statement is parsed — a string param containing a
quote needs no caller-side escaping. `params[i].name` is ignored (params
are positional only). This is substitution-then-parse, not a real
prepared-statement cache — same one-shot cost as `kdb_exec_sql`, just
without hand-rolled `snprintf`/escaping on the caller's side:

```c
KdbField params[] = { kdb_field_int(NULL, min_age), kdb_field_string(NULL, "eng") };
kdb_exec_sql_params(db, "SELECT * FROM employees WHERE age >= ? AND dept = ?",
                    params, 2, &rows, NULL);
```

```sql
CREATE TABLE t (col TYPE [NOT NULL] [UNIQUE | PRIMARY KEY] [INDEX] [REFERENCES t2(col2)], ...
                [, FOREIGN KEY (col) REFERENCES t2(col2)]* [, CHECK (col op literal)]*)
ALTER TABLE t ADD [COLUMN] col TYPE [NOT NULL] [UNIQUE | PRIMARY KEY] [INDEX] [REFERENCES t2(col2)]
ALTER TABLE t ADD FOREIGN KEY (col) REFERENCES t2(col2)
ALTER TABLE t ADD CHECK (col op literal)
ALTER TABLE t DROP [COLUMN] col
ALTER TABLE t DROP FOREIGN KEY (col)
ALTER TABLE t RENAME COLUMN col TO new_col
ALTER TABLE t ALTER [COLUMN] col TYPE newtype | SET NOT NULL | DROP NOT NULL | SET UNIQUE | DROP UNIQUE
ALTER TABLE t RENAME [TO] new_name
DROP TABLE t

CREATE VIEW v AS SELECT ...
DROP VIEW v

CREATE INDEX [name] ON t (col, ...)
DROP INDEX [name] ON t (col, ...)

INSERT INTO t (col, ...) VALUES (val, ...) [, (val, ...)]* [RETURNING * | col, ...]
INSERT INTO t (col, ...) SELECT ... [RETURNING * | col, ...]
INSERT INTO t (col, ...) VALUES (val, ...) ON CONFLICT (col, ...) DO NOTHING | DO UPDATE SET col = val, ... [RETURNING * | col, ...]

[WITH name AS (SELECT ...) [, name2 AS (SELECT ...)]*]
[WITH RECURSIVE name AS (SELECT ... UNION [ALL] SELECT ...)]
SELECT [DISTINCT] * | item, ... FROM t | (SELECT ...) [[AS] alias]
    [[INNER|LEFT [OUTER]|RIGHT [OUTER]|FULL [OUTER]|CROSS] JOIN t2 [[AS] alias2] [ON a.col OP (b.col|literal) [AND ...]]]*
    [WHERE cond [AND|OR cond ...]]
    [GROUP BY col, ... | ROLLUP(col, ...) | CUBE(col, ...) | GROUPING SETS ((col, ...), ...)]
    [HAVING cond [AND|OR cond ...]]
    [(UNION|INTERSECT|EXCEPT) [ALL] SELECT ...]*
    [ORDER BY col [ASC|DESC], ...]
    [LIMIT n [OFFSET m]]

UPDATE t SET col = val, ... [WHERE cond [AND|OR cond ...]] [RETURNING * | col, ...]
DELETE FROM t [WHERE cond [AND|OR cond ...]] [RETURNING * | col, ...]
```

**Comments:** `-- to end of line` and `/* block, can span lines */` are
both stripped like whitespace, anywhere a token could otherwise start. An
unterminated `/*` just eats to the end of the string rather than erroring
on the comment itself — you'll get whatever "ran out of input mid-
statement" error the missing content after it would've caused anyway.

**`INSERT`** always needs an explicit column list — `INSERT INTO t (a, b)
VALUES (...)`, never a bare `INSERT INTO t VALUES (...)`. `VALUES` accepts
more than one comma-separated tuple in one statement — `VALUES (1, 'a'),
(2, 'b'), (3, 'c')` — each inserted as its own row, in order; if one
partway through fails, whatever came before it stays inserted rather than
rolling back the whole statement, since there's no implicit per-statement
transaction wrapping here (wrap it in an explicit transaction yourself if
that matters). `INSERT INTO t (a, b) SELECT ...` inserts one row per row
the `SELECT` returns instead, matching its projected columns to `(a, b)`
by position, not by name — the column counts must match, checked before
anything is inserted. Same no-rollback behavior if a row partway through
the `SELECT`'s results fails to insert.

**`ON CONFLICT (col, ...) DO NOTHING`/`DO UPDATE SET ...`** turns a
single-row `VALUES` insert into an upsert — only single-row, an `ON
CONFLICT` after a multi-row `VALUES` list is rejected. Unlike real SQL's
`ON CONFLICT`, which reacts to an actual constraint-violation error,
this checks proactively: before inserting anything, it checks whether a
row already matches every named column's value. If one does, `DO
NOTHING` leaves things exactly as they are (0 rows affected) and `DO
UPDATE SET ...` updates it. The named columns don't have to be declared
`UNIQUE`/`PRIMARY KEY` at all — `ON CONFLICT` works as a general
"does a matching row already exist" check on any column combination; if
they *aren't* unique, more than one row can match, and every match gets
the same `SET`, same as any other filtered `UPDATE`. (If the named
columns *are* declared `UNIQUE`/`PRIMARY KEY`, real enforcement already
guarantees at most one row can ever match, on top of whichever branch
`ON CONFLICT` takes.) If none matches, both forms fall back to a plain
insert:

```sql
INSERT INTO users (email, name, visits) VALUES ('a@x.com', 'Alice', 1)
    ON CONFLICT (email) DO UPDATE SET name = 'Alice', visits = 1

INSERT INTO users (email, name) VALUES ('b@x.com', 'Bob')
    ON CONFLICT (email) DO NOTHING
```

Every `ON CONFLICT` column must actually be one of the `INSERT`'s own
target columns (its value comes from that same `VALUES` tuple) — more
than one column is fine, checked together as a conjunction, same
"multi-column key" idea a real composite unique index would give you.
`DO UPDATE SET`'s values are literals, same as a plain `UPDATE`'s `SET`
(no referencing the row that was about to be inserted).

```sql
INSERT INTO sales (region, amount) VALUES ('east', 100.0), ('west', 50.0)
INSERT INTO archive (name, total) SELECT name, SUM(amount) FROM sales GROUP BY name
```

**`RETURNING * | col, ...`** trails `INSERT`/`UPDATE`/`DELETE` (every form
of `INSERT`, including multi-row `VALUES`, `INSERT ... SELECT`, and the
upsert forms above) and hands back the affected rows instead of just a
count — the newly inserted row(s) for `INSERT`, the post-update state for
`UPDATE`, the row(s) as they were right before deletion for `DELETE`.
`RETURNING *` excludes the `id`/`created_at`/`updated_at` pseudo-columns,
same convention plain `SELECT *` already uses elsewhere in this dialect —
name one explicitly (`RETURNING id, name`) to get it back, which matters
here more than almost anywhere else, since recovering a generated `id` is
`RETURNING`'s single most common real use:

```sql
INSERT INTO users (name, email) VALUES ('Alice', 'a@x.com') RETURNING id
UPDATE users SET status = 'active' WHERE id = 5 RETURNING id, status
DELETE FROM users WHERE status = 'banned' RETURNING id, email
```

An `UPDATE`/`DELETE` `RETURNING` with no matching rows just comes back
with 0 rows, not an error, same as an unmatched `ON CONFLICT ... DO
NOTHING`. There's no `EXCLUDED`-style reference to the row that would
have been inserted (Postgres's upsert idiom) — `DO UPDATE SET`'s values
are plain literals either way, `RETURNING` or not.

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

**`CREATE INDEX [name] ON t (col, ...)`**/**`DROP INDEX [name] ON t (col,
...)`** index (or un-index) a column *after* the table already exists —
`INDEX`/`UNIQUE`/`PRIMARY KEY` on a `CREATE TABLE`/`ALTER TABLE ADD
COLUMN` column definition only ever takes effect at that column's
creation moment; these two work on a column that's already there,
rebuilding the index from every existing row (`CREATE INDEX`) or just
removing it (`DROP INDEX`, leaving the column and its data untouched).
The index name is accepted and ignored if you give one — KumDB finds an
index again by its column set, not a name. One column creates an
ordinary single-column index; two or more create one **real composite
(multi-column) index** — a single column-value tuple hashed together,
not that many independent single-column indexes — capped at 4 columns
per composite index and 4 composite indexes per table:

```sql
CREATE INDEX ON employees (department)
CREATE INDEX ON employees (department, role)  -- one composite index over both columns together
DROP INDEX ON employees (department, role)     -- column order doesn't matter for finding it again
```

A query naming every column of a composite index with `=`/`AND` (in any
order) can use it to skip a full table scan, same as a single-column
index does for one column. Indexing an already-indexed column (or an
identical composite column set), or dropping an index that isn't there,
both error rather than silently no-opping. A composite index is purely
a lookup accelerator, same as a single-column one — it doesn't imply or
enforce uniqueness across the combined columns (that's what a `UNIQUE`
column, or several, is for; the two features are independent of each
other).

**`ALTER TABLE t RENAME COLUMN col TO new_col`** renames a column —
schema, its index if it has one, and every existing row's field, so it's
a full table rewrite, same cost as `ALTER TABLE ... DROP COLUMN`.
**`ALTER TABLE t RENAME [TO] new_name`** renames the table itself (the
file on disk, not just in-memory state) — `TO` is optional, both spellings
work. **`ALTER TABLE t ALTER [COLUMN] col SET NOT NULL`**/**`DROP NOT
NULL`**/**`SET UNIQUE`**/**`DROP UNIQUE`** toggle a column's declared
nullable/unique flags — both really enforced from that point on
(`INSERT`/`UPDATE` reject a violation, same as `NOT NULL`/`UNIQUE` on a
`CREATE TABLE` column definition), but only for rows written *after* the
`ALTER`; existing rows that already violate the new rule aren't
retroactively checked or rewritten. `SET UNIQUE` also indexes the column
if it wasn't already (a unique column always needs a real index to check
against).

**`ALTER TABLE t ALTER [COLUMN] col TYPE newtype`** is a real data
migration, not just a metadata flip: every existing row's value for
`col` is converted to `newtype` using the same coercions `CAST(x AS
type)` uses in `SELECT` (`NULL` stays `NULL`; otherwise the usual INT/
FLOAT/BOOL/STRING conversions), and any index on the column is rebuilt
afterward (its old hash buckets were built from the old-typed values and
no longer match). If even one existing value doesn't convert — a STRING
column holding `"abc"` changed to `INT`, say — the whole change is
rejected and the table (schema and every row) is left completely
untouched, never half-migrated. A no-op if `newtype` is already the
column's current type.

```sql
ALTER TABLE employees RENAME COLUMN dept TO department
ALTER TABLE employees RENAME TO staff
ALTER TABLE staff ALTER COLUMN email SET NOT NULL
ALTER TABLE staff ALTER COLUMN zip_code TYPE INT
```

Renaming a column or table that another one already has that name
errors rather than silently colliding.

**Foreign keys** — `REFERENCES t2(col2)` as a column modifier, or
`FOREIGN KEY (col) REFERENCES t2(col2)` as its own table-level item —
declare that `col` must always be `NULL` or equal to some existing
`t2.col2` value. `t2`/`col2` must already exist (checked immediately, not
deferred):

```sql
CREATE TABLE customers (name TEXT UNIQUE)
CREATE TABLE orders (customer_id INT REFERENCES customers(id), amount FLOAT)
```

From then on, `INSERT`/`UPDATE` giving `customer_id` a non-`NULL` value
with no matching `customers.id` is rejected, and deleting or updating
away a `customers` row that some `orders` row still points to is
rejected too — `RESTRICT` semantics, no `ON DELETE`/`ON UPDATE`
`CASCADE`/`SET NULL`, the write is simply refused. `col2` can be a real
column or one of the `id`/`created_at`/`updated_at` pseudo-columns —
referencing a table's own auto `id` (as above) is the most common case,
even though `id` isn't a "real" schema column (`kdb_table_has_column` is
false for it — a plain `SELECT id` doesn't project it back out either,
a separate, pre-existing gap; use `RETURNING id` to get it after an
`INSERT`). One foreign key per column, no composite (multi-column)
foreign keys. `ALTER TABLE t ADD FOREIGN KEY (col) REFERENCES
t2(col2)` adds one after the fact, same validation and enforcement;
`ALTER TABLE t DROP FOREIGN KEY (col)` removes it. Dropping `col` itself
takes its foreign key with it.

**`CHECK (col op literal)`** — as its own table-level item in
`CREATE TABLE`, or via `ALTER TABLE t ADD CHECK (col op literal)` —
restricts `col`'s values:

```sql
CREATE TABLE orders (customer_id INT REFERENCES customers(id), amount FLOAT,
                      CHECK (amount > 0))
```

`op` is one of `=`, `!=`, `>`, `>=`, `<`, `<=` (no `BETWEEN`/`IN`/`LIKE`/
etc — those would need more than one literal or a list); the literal is
an `INT`/`FLOAT`/`BOOL`/`STRING` value, never `NULL` (a `NULL` value
never violates a `CHECK`, same as real SQL, so comparing against `NULL`
isn't a meaningful constraint and is rejected at parse time). Enforced
the same way `NOT NULL`/`UNIQUE` are — `INSERT`/`UPDATE` reject a
violation from here on, existing rows aren't retroactively checked.
Dropping `col` drops any `CHECK` on it too, rather than leaving a
dangling reference to a nonexistent column.

Both features needed real fields on `KdbColumn`/`KdbTableHeader` (a
foreign key alone needs a whole table-name-sized string) rather than
being squeezed into the small reserved padding every earlier extension
this year carved from instead — the on-disk file format bumped to 2.0
over it, which is a real, intentional break from 1.x (KumDB refuses to
open a 1.x file with a clear "versions are incompatible" error, same as
it already did for any major-version mismatch, rather than misreading
old data).

**`WHERE`** supports `=`, `!=`/`<>`, `>`, `>=`, `<`, `<=`,
`BETWEEN a AND b`, `IN (a, b, c)`, `IS [NOT] NULL`, `LIKE 'pat'`
(standard SQL wildcards: `%` matches any run of characters including
none, `_` matches exactly one, anywhere in the pattern — no `ESCAPE`
clause, so there's no way to match a literal `%`/`_`), `ILIKE 'pat'`
(same wildcards, case-insensitive), `REGEXP 'pat'`, and
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

**`ILIKE`** is `LIKE` with the case sensitivity removed — same `%`/`_`
wildcards, no `ESCAPE` clause, just an ASCII-case-insensitive comparison:
`name ILIKE 'a%'` matches `"alice"` and `"Alice"` alike.

**`REGEXP`** matches against a small, hand-rolled regex engine, not
POSIX `<regex.h>` — that header isn't available when cross-compiling for
Windows via mingw-w64, which this project targets alongside Linux. It
supports literals, `.` (any character), `*`/`+`/`?` quantifiers,
`[...]`/`[^...]` character classes (with `a-z`-style ranges), `\`
escapes, and optional `^`/`$` anchors (unanchored otherwise — the pattern
matches anywhere in the string, same as a substring search would). It
does **not** support alternation (`|`), groups/capturing, backreferences,
or bounded repetition (`{n,m}`) — those characters are treated as
ordinary literals if you use them, not an error.

```sql
SELECT name FROM users WHERE name ILIKE 'a%'
SELECT name FROM users WHERE email REGEXP '^[a-z]+@[a-z]+\.[a-z]+$'
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

**`JOIN`** (`INNER`/`LEFT [OUTER]`/`RIGHT [OUTER]`/`FULL [OUTER]`/`CROSS`)
matches rows via `ON`, a conjunction of comparisons — `=`, `!=`, `>`,
`>=`, `<`, `<=` — between two columns, or a column and a literal
(number/string/`true`/`false`): a real theta join, not just an equi-join.
Still no `OR` in `ON` (that's what `WHERE`, applied after the join, is
for). `INNER` (the default) drops unmatched rows; `LEFT` keeps
every row from everything accumulated so far, padding an unmatched one
with `NULL` for the new table's columns; `RIGHT` is the mirror — keeps
every row of the new table, padding `NULL` for every column accumulated
so far when nothing matches; `FULL` does both directions at once. `CROSS`
is the odd one out: no `ON` at all (rejected if you write one), just
every combination of both sides — the cartesian product. Chain as many
`JOIN` clauses as you want, mixing kinds freely; each one matches against
everything accumulated so far, so a later `ON` can reference any earlier
alias in the chain, not just the table right before it:

```sql
SELECT u.name, o.item
    FROM users AS u
    JOIN orders AS o ON u.id = o.user_id
    WHERE o.item = 'widget'

SELECT u.name, o.item FROM users AS u LEFT JOIN orders AS o ON u.id = o.user_id
SELECT u.name, o.item FROM users AS u RIGHT JOIN orders AS o ON u.id = o.user_id
SELECT u.name, o.item FROM users AS u FULL JOIN orders AS o ON u.id = o.user_id
SELECT u.name, o.item FROM users AS u CROSS JOIN orders AS o

SELECT u.name, o.item, r.stars
    FROM users AS u
    JOIN orders AS o ON u.id = o.user_id
    LEFT JOIN reviews AS r ON o.item = r.order_item

-- theta join: band membership by range, plus a literal comparison
SELECT e.name, b.label
    FROM emp AS e
    JOIN band AS b ON e.salary >= b.lo AND e.salary < b.hi AND e.active = true
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
words (`WHERE`, `GROUP`, `HAVING`, `ORDER`, `LIMIT`, `UNION`, `INTERSECT`,
`EXCEPT`, `JOIN`, `INNER`, `LEFT`, `RIGHT`, `FULL`, `CROSS`, `OUTER`,
`ON`, `AS`) that always mean the keyword, never a bare alias — `FROM t
WHERE ...` parses `WHERE` as the clause, not as `t`'s alias, exactly like
real SQL's reserved-word handling.

A query with no `JOIN` at all can still qualify its own columns with its
own alias if one's in scope (explicit, bare, or just the table name) —
`SELECT * FROM users u WHERE u.name = 'alice'` works the same as the
unqualified `WHERE name = 'alice'`. Mostly useful for `EXISTS`'s inner
query, where qualifying is the natural way to write a self-reference
alongside a correlated one to the outer row (see below).

(`LEFT`/`RIGHT`/`FULL`'s NULL-padding, described above, always includes
the `alias.id`/`alias.created_at`/`alias.updated_at` pseudo-columns too,
same as a real matched row would have them.)

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
`COUNT(*)`, `COUNT(col)`, `COUNT(DISTINCT col)`, `SUM(col)`, `AVG(col)`,
`MIN(col)`, `MAX(col)`, `STRING_AGG(col, 'sep')`/`GROUP_CONCAT(col, 'sep')`
(the same function under two names, both requiring an explicit separator
— no default-separator or MySQL `SEPARATOR`-keyword form) — or a `CASE`
expression (below), optionally renamed with `AS alias` (default alias is
e.g. `SUM(amount)`, or `case` for an unaliased `CASE`). Without
`GROUP BY`, one or more aggregate items collapse the result into a single
summary row. `GROUP BY` takes one or more comma-separated columns —
`GROUP BY region` or `GROUP BY region, product` — and you get one row per
distinct combination of values across all of them; every other selected
item must be an aggregate call — same rule real SQL uses. `SELECT col FROM
t GROUP BY col` with no aggregate at all is a valid way to get distinct
values. `SUM`/`AVG` always come back as `FLOAT` regardless of the source
column's type; `STRING_AGG`/`GROUP_CONCAT` comes back `NULL` for a group
with no non-`NULL` values to concatenate, not an empty string.

```sql
SELECT region, COUNT(DISTINCT rep) AS n_reps, STRING_AGG(rep, ', ') AS reps
    FROM sales GROUP BY region
```

`STRING_AGG`/`GROUP_CONCAT` only works as a `GROUP BY`-collapsing
aggregate, not as a window function (`OVER (...)` on it is rejected) —
the other aggregates can be either, `OVER`'s presence is what decides.

**`GROUP BY ROLLUP(...)`/`CUBE(...)`/`GROUPING SETS (...)`** compute
several grouping sets in one query and union the results together — the
same rows you'd get running one plain `GROUP BY` per set and `UNION
ALL`-ing them by hand. A column not in a given output row's own set comes
back `NULL` for that row (this engine has no `GROUPING()` function, so
that `NULL` isn't distinguished from a real stored `NULL`):

```sql
SELECT region, rep, SUM(amount) AS total FROM sales GROUP BY ROLLUP(region, rep)
```

`ROLLUP(a, b, c)` produces one grouping set per prefix — `(a,b,c)`,
`(a,b)`, `(a)`, `()` — a hierarchical subtotal-then-grand-total pattern
(the query above gets a subtotal per region, a subtotal per rep within
each region, and a grand total). `CUBE(a, b)` produces every subset —
`(a,b)`, `(a)`, `(b)`, `()` — capped at 4 columns to keep its 2^n
grouping sets bounded; use `ROLLUP` instead for more columns if a
hierarchical breakdown (not every combination) is enough. `GROUPING
SETS ((a,b), (a), ())` lists the exact sets wanted and nothing more —
`GROUPING SETS ((region), ())` gets region subtotals plus a grand total
but skips the full `(region, rep)` breakdown `ROLLUP(region, rep)` would
include. Each of `ROLLUP`/`CUBE`/`GROUPING SETS` must be the entire
`GROUP BY` clause (not mixed with plain columns — `GROUP BY region,
ROLLUP(rep)` isn't supported), and every `SELECT` item still needs to be
an aggregate or one of the columns mentioned somewhere in the clause,
same rule plain `GROUP BY` uses — just checked against the union of
every listed set rather than one fixed column list. `HAVING` filters the
unioned result the same as any other `GROUP BY`.

Any `GROUP BY`-collapsing aggregate — `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`/
`STRING_AGG`/`GROUP_CONCAT`, `DISTINCT` or not — accepts a trailing
`FILTER (WHERE cond [AND|OR cond ...])`: only rows matching that
aggregate's own filter are folded into it, so several differently-
filtered aggregates can share one `GROUP BY` pass instead of needing a
`CASE WHEN ... END` trick inside `SUM`/`COUNT` or several separate
queries. No parens within one `FILTER`'s conditions (same limit `CASE`'s
`WHEN` has). Not supported combined with `OVER (...)` — only as a
`GROUP BY`-collapsing aggregate, not a window function.

```sql
SELECT dept,
       COUNT(*) AS total,
       COUNT(*) FILTER (WHERE status = 'done') AS done_count,
       SUM(amount) FILTER (WHERE status = 'done') AS done_sum
    FROM orders GROUP BY dept
```

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

**Window functions**: `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`,
`LAG(col[, offset[, default]])`, `LEAD(col[, offset[, default]])`,
`FIRST_VALUE(col)`, `LAST_VALUE(col)`, `NTILE(n)`, and
`COUNT`/`SUM`/`AVG`/`MIN`/`MAX` all work as window functions with
`OVER ([PARTITION BY col, ...] [ORDER BY col [ASC|DESC], ...])` — unlike
`GROUP BY`, rows aren't collapsed; every row keeps its own value alongside
whatever else is selected:

```sql
SELECT region, rep, amount,
       RANK() OVER (PARTITION BY region ORDER BY amount DESC) AS rank_in_region,
       SUM(amount) OVER (PARTITION BY region) AS region_total,
       LAG(amount) OVER (PARTITION BY region ORDER BY amount ASC) AS prev_amount
    FROM sales
```

`RANK`/`DENSE_RANK` give tied rows (equal `ORDER BY` values within a
partition) the same rank; `RANK` leaves a gap afterward (`1, 1, 3`),
`DENSE_RANK` doesn't (`1, 1, 2`).

`LAG`/`LEAD` return the value of a column `offset` rows before/after the
current one, in `ORDER BY` order, within the same partition (`offset`
defaults to `1`); past the partition's edge they return `default` if
given, `NULL` otherwise. `FIRST_VALUE`/`LAST_VALUE` return the first/last
row's value in the partition (in `ORDER BY` order) — same value on every
row of it, not a running value. `NTILE(n)` divides each partition's rows
into `n` roughly-equal buckets in `ORDER BY` order and returns the bucket
number (`1..n`); an uneven split gives the earlier buckets the extra
row(s) (5 rows into 2 buckets: 3, 2); `n` larger than the partition just
means some rows each get their own bucket and the rest of the numbers
(up to `n`) go unused, not an error.

`FIRST_VALUE`/`LAST_VALUE` always cover the whole partition, and `LAG`/
`LEAD` look at a fixed-offset neighbor within it — neither takes a frame
clause (using one is a parse error, not a silent no-op). `COUNT`/`SUM`/
`AVG`/`MIN`/`MAX` default to the whole partition too, but accept an
explicit `ROWS`/`RANGE BETWEEN <start> AND <end>` frame clause — this
needs `ORDER BY` in the same `OVER (...)` — to narrow that to a running
or moving window instead:

```sql
SELECT date, amount,
       SUM(amount) OVER (PARTITION BY region ORDER BY date
                          ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS running_total,
       AVG(amount) OVER (PARTITION BY region ORDER BY date
                          ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS moving_avg_3
    FROM sales
```

`ROWS` accepts the full bound vocabulary, each relative to the current
row and clipped to the partition: `UNBOUNDED PRECEDING`, `n PRECEDING`,
`CURRENT ROW`, `n FOLLOWING`, `UNBOUNDED FOLLOWING`. `RANGE` only supports
`UNBOUNDED PRECEDING` as the start bound and `CURRENT ROW`/`UNBOUNDED
FOLLOWING` as the end bound — resolved via peer groups, so rows tied on
`ORDER BY` all see the same result rather than being split mid-tie;
numeric `RANGE` offsets (`n PRECEDING`/`FOLLOWING`) aren't supported, use
`ROWS` for those. `ROWS <bound>`/`RANGE <bound>` with no `BETWEEN` is
shorthand for `BETWEEN <bound> AND CURRENT ROW` (so `ROWS UNBOUNDED
PRECEDING` alone is a running total, same as spelling out the `BETWEEN`
above). `PARTITION BY`/`ORDER BY` are both optional (omit `PARTITION BY`
and the whole result set is one partition); omitting `ORDER BY` inside
`OVER` for `RANK`/`DENSE_RANK` means everything ties (rank 1 for every
row in the partition) — same as real SQL. `ROW_NUMBER`/`RANK`/
`DENSE_RANK`/`LAG`/`LEAD`/`FIRST_VALUE`/`LAST_VALUE`/`NTILE` always need
`OVER` (there's no non-window use of them); `COUNT`/`SUM`/`AVG`/`MIN`/
`MAX` use `OVER` to switch from their `GROUP BY`-collapsing form to this
one — a `SELECT` can't mix a window function with `GROUP BY` or a plain
(non-window) aggregate. Works fine after a `JOIN`, on qualified columns,
same as anything else there.

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

**A bare literal** (number, string, `TRUE`/`FALSE`, `NULL`) also works as
a `SELECT` item — same value on every row, `FROM` is still mandatory
(this engine's one global `SELECT` rule; there's no `SELECT 1` with
nothing to select it *from*, same limitation Oracle's famous `SELECT 1
FROM DUAL` works around):

```sql
SELECT name, 'active' AS status FROM users
```

Unaliased, it defaults to `?column?`, same as real SQL. Not combined
with `GROUP BY`/aggregate functions, same limit `CASE` has above.

**Arithmetic expressions** (`+`, `-`, `*`, `/`, `%`) work as a `SELECT`
item too, chaining a column and/or number literal terms at real operator
precedence (`*`/`/`/`%` bind tighter than `+`/`-`, left to right within
the same precedence level):

```sql
SELECT name, price * qty AS total FROM orders
SELECT price + qty * 2 AS r FROM orders
```

Always computed in double precision and returned as `FLOAT`, regardless
of the operand types — same simplification `SUM`/`AVG` already make
("always `FLOAT` regardless of the source column's type"), so `int /
int` doesn't truncate the way it does in some real engines. A missing/
`NULL`/non-numeric column, or a division/modulo by zero, makes the whole
expression `NULL` for that row rather than erroring the query. Up to 6
terms per expression (a term is one column or number, with an optional
leading unary `-`); no parentheses for grouping within one expression —
`(a + b) * c` isn't parseable, only a flat left-to-right chain at the
precedence above. Unaliased, defaults to `?column?`, same as a bare
literal. Not combined with `GROUP BY`/aggregate functions, and — for now
— only usable as a `SELECT` item, not inside `WHERE`/`HAVING`/`ON` or as
a function argument (`ROUND(price * 1.1, 2)` isn't parseable either);
those all still take a plain column or literal only.

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

A view can be `JOIN`ed too, either as the `FROM` target or a `JOIN`
target, mixed freely with real tables in the same chain:

```sql
SELECT au.name, o.item FROM active_users AS au JOIN orders AS o ON au.id = o.user_id
```

`LEFT`/`RIGHT`/`FULL` padding against a view needs to know its column
names, and a view has no fixed schema the way a real table does -- only
whatever rows its query happens to return -- so those three kinds need
the view to return at least one row on that particular run; a view that
returns zero rows works fine as an `INNER`/`CROSS` target (no padding
ever needed there) but errors clearly if `LEFT`/`RIGHT`/`FULL` needed to
pad against it and came up with nothing to derive column names from.
Views are stored as rows in a reserved internal table, `__kumdb_views__`;
it'll show up in `kdb_list_tables()`/the CLI's `tables` command like any
other table (querying it directly works fine, it's just not hidden), so
don't name a real table that.

**`WITH name AS (SELECT ...) SELECT ... FROM name`** (a CTE) is a view
scoped to one statement instead of persisted -- same validate-immediately,
re-run-fresh-every-time, and `JOIN`-target behavior as `CREATE VIEW`,
gone the moment the statement finishes (success or error):

```sql
WITH big_orders AS (SELECT * FROM orders WHERE amount > 1000)
SELECT customer, amount FROM big_orders ORDER BY amount DESC
```

Chain more than one with a comma; a later one can reference an earlier
one (each is validated and made visible in declaration order), but not
the other way around, and not itself -- this plain (non-`RECURSIVE`)
form:

```sql
WITH regional AS (SELECT region, amount FROM sales WHERE region = 'east'),
     totals   AS (SELECT region, SUM(amount) AS total FROM regional GROUP BY region)
SELECT region FROM totals WHERE total > 10000
```

`WITH` only ever precedes a `SELECT` -- not `UPDATE`/`DELETE`/`INSERT`.

**`WITH RECURSIVE name AS (base_select UNION [ALL] recursive_select) SELECT
... FROM name`** does self-reference: `recursive_select` reads `name` too,
seeing just the *previous round's new rows* each time (standard
"semi-naive" recursive-CTE evaluation, not the whole running total —
matters for a recursive term whose own logic assumes that, e.g. graph
traversal via a JOIN), repeating until a round produces zero new rows
(fixpoint) or `10000` rounds pass (a recursive term that never converges
errors instead of hanging). `UNION` (not `UNION ALL`) drops any row
already produced by an earlier round before checking whether the round
produced anything new — this is what makes a cycle in the underlying
data (a graph edge back to an already-visited node, say) terminate on
its own instead of looping forever:

```sql
WITH RECURSIVE org AS (
    SELECT name, manager FROM employees WHERE name = 'vp_eng'
    UNION
    SELECT e.name, e.manager FROM employees AS e JOIN org ON e.manager = org.name
)
SELECT name FROM org
```

Scoped to exactly one CTE (no mixing with other CTEs in the same `WITH`,
recursive or not — issue those as a separate statement) with exactly the
standard base-`UNION`-recursive shape (two arms, not more). The base
case must return at least one row (there's no separate resultset schema
to materialize zero rows against — see literal `SELECT` items below for
the same underlying reason), and the recursive term's columns must match
the base case's exactly (same names, same order — a missing `AS alias`
on a computed column is a common way to trip this). Under the hood, a
real (but fully temporary) table named `name` gets created to run all
of this — dropped again before returning, success or error, so it's
invisible outside the statement and safe to use inside a `BEGIN`/`COMMIT`
transaction despite schema changes normally being rejected mid-transaction
(see Transactions above) — this one's fully self-contained.

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
