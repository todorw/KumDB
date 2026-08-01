# **KumDB** 🔥
### *"The Database That Doesn't Waste Your F*cking Time"*

<p align="center">
<img src="logo.png" alt="KumDB Logo" width="200"/>
</p>

---

## **⚠️ WARNING**

This is **NOT** for:
- ORM enjoyers ❌
- People who enjoy writing `JOIN` statements ❌
- People who need multi-table anything ❌

This **IS** for:
- Developers who want **dead-simple** data persistence ✅
- Embedded projects where SQLite is overkill ✅
- KumOS and anything that runs C ✅
- When you need to **GET SHIT DONE** ✅

---

## **💥 What Is It**

KumDB is a lightweight embedded database engine written in pure C11. No dependencies. No bullshit. A clean key-value-style API on top of a fast binary file format with atomic writes, type inference, and savage error messages -- and if you *want* SQL, it speaks that too, same engine underneath, your call. Still no `JOIN` statements though. We meant that part.

```c
// NoSQL: THIS simple
KdbField fields[] = {
    kdb_field_string("name",   "John"),
    kdb_field_int   ("age",    30),
    kdb_field_bool  ("admin",  1),
    kdb_field_end   ()
};
kdb_add(db, "users", fields);

const char *filters[] = { "name=John", NULL };
KdbRow *user = kdb_find_one(db, "users", filters);

// Or SQL, if that's more your thing -- same table, same data, same engine
kdb_exec_sql(db, "SELECT * FROM users WHERE name = 'John'", &rows, NULL);
```

---

## **⚡ Quick Start**

**Build:**
```bash
make
```

**Use it in your project:**
```bash
# Include the header
#include "kumdb.h"

# Link against the static lib
gcc myapp.c build/libkumdb.a -lm -o myapp
```

**Or just use the CLI:**
```bash
./build/bin/kumdb_cli ./mydata
```

---

## **💻 API Cheat Sheet**

```c
// Open / close
KumDB *db = kdb_open("./mydata");
kdb_close(db);

// Insert
KdbField fields[] = {
    kdb_field_string("name",  "Alice"),
    kdb_field_int   ("age",   25),
    kdb_field_float ("score", 9.5),
    kdb_field_bool  ("vip",   1),
    kdb_field_end   ()
};
kdb_add(db, "users", fields);

// Find (NULL filters = all rows)
const char *filters[] = { "age__gt=21", "name__contains=Ali", NULL };
KdbRows *rows = kdb_find(db, "users", filters);
kdb_rows_free(rows);

// Find one
KdbRow *row = kdb_find_one(db, "users", filters);
kdb_row_free(row);

// Count
int64_t n = kdb_count(db, "users", NULL);

// Update
const char *where[] = { "name=Alice", NULL };
KdbField patch[] = { kdb_field_int("age", 26), kdb_field_end() };
kdb_update(db, "users", where, patch, &updated);

// Delete
kdb_delete(db, "users", where, &deleted);

// Compact (remove soft-deleted rows from disk)
kdb_compact(db, "users");

// Drop table
kdb_drop_table(db, "users");
```

---

## **🔍 Filter Operators**

| Operator | Example | Meaning |
|----------|---------|---------|
| *(none)* | `"age=30"` | equals |
| `__eq` | `"age__eq=30"` | equals |
| `__neq` | `"age__neq=30"` | not equals |
| `__gt` | `"age__gt=21"` | greater than |
| `__gte` | `"age__gte=21"` | greater than or equal |
| `__lt` | `"age__lt=65"` | less than |
| `__lte` | `"age__lte=65"` | less than or equal |
| `__between` | `"age__between=18,30"` | inclusive range |
| `__in` | `"age__in=18,21,30"` | matches any value in the list |
| `__contains` | `"name__contains=ali"` | substring match |
| `__startswith` | `"name__startswith=al"` | prefix match |
| `__endswith` | `"name__endswith=ice"` | suffix match |
| `__isnull` | `"notes__isnull"` | field is null |
| `__isnotnull` | `"notes__isnotnull"` | field is not null |

Multiple filters = AND logic. For OR, prefix a filter with `"OR:"` to start a new
AND-group that gets OR'd against everything before it -- same precedence as SQL
(AND binds tighter than OR, no parens/nesting):

```c
// age < 18 OR age > 65
const char *filters[] = { "age__lt=18", "OR:age__gt=65", NULL };

// (active=true AND score__gt=90) OR (vip=true)
const char *filters2[] = { "active=true", "score__gt=90", "OR:vip=true", NULL };
```

---

## **🖥️ KumDB Studio**

A desktop app (Windows/Mac/Linux, Qt) for people who'd rather click than type:
table browser, editable datasheet grid, and a query console that toggles
between NoSQL filters and SQL. Not an Access form/report designer clone --
just the "view and edit my data, run some queries" part, which is most of
what Access gets used for anyway. See [`app/README.md`](app/README.md) for
build instructions.

---

## **🛠️ Tools**

```bash
# Interactive CLI
./build/bin/kumdb_cli ./mydata

# Benchmark (default 10k rows)
./build/bin/bench 50000 ./mydata

# Dump table contents
./build/bin/dump ./mydata users
./build/bin/dump ./mydata users --csv
./build/bin/dump ./mydata users --json --limit 50
```

**CLI commands (NoSQL):**
```
open <dir>                          open a database
close                               close the current database
tables                              list tables
schema <table>                      show schema
add <table> <k=v> [k=v ...]        insert a record (value can be @path for a blob)
find <table> [filter ...] [order_by=col] [order=asc|desc] [limit=N] [offset=N]
findbyid <table> <id>               find a single record by id
count <table> [filter ...]         count records
update <table> where <k=v> set <k=v>  update records
import <table> <file>               bulk-insert k=v lines from a file
delete <table> <filter> [...]      delete records
compact <table>                     compact table file
drop <table>                        drop table
version                             show CLI/engine version
```

**CLI commands (SQL):** same engine, different syntax --- one statement per line:
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
No `JOIN`, no subqueries. `WHERE` supports `=`, `!=`/`<>`, `>`, `>=`, `<`, `<=`,
`BETWEEN a AND b`, `IN (a, b, c)`, `IS [NOT] NULL`, `LIKE 'pat'` (leading/trailing `%`
only), and `AND`/`OR` at standard SQL precedence (`AND` binds tighter than `OR`, no
parens/nesting -- `a=1 AND b=2 OR c=3` means `(a=1 AND b=2) OR (c=3)`). `SELECT` items
can be `COUNT(*)`/`COUNT(col)`/`SUM(col)`/`AVG(col)`/`MIN(col)`/`MAX(col)`, optionally
`AS alias`'d, with an optional `GROUP BY col` (one column, no `HAVING`) -- same rule as
real SQL: a plain column in a `GROUP BY` query has to either be the grouping column or
be wrapped in an aggregate. Also callable directly from C via `kdb_exec_sql()` in
`sql.h` --- both syntaxes hit the exact same
storage and query code, so results are always consistent between them.

---

## **⚙️ Build Targets**

```bash
make              # release build → build/libkumdb.a + build/bin/*
make debug        # ASAN + UBSan + debug symbols
make check        # run all tests (use after make debug)
make distclean    # nuke build/
```

---

## **🔫 Performance**

| Operation | Speed |
|-----------|-------|
| Insert | ~50K ops/sec |
| Find (full scan) | ~200K rows/sec |
| Count | ~500K rows/sec |
| Update (batch) | ~40K ops/sec |

Numbers from `bench` on a mid-range machine. Your mileage may vary. Run `./build/bin/bench` to find out.

---

## **🤬 Comparison**

| Feature | KumDB | SQLite | MongoDB |
|---------|-------|--------|---------|
| Learning curve | 5 mins | 5 weeks | 5 years |
| Setup | drop in 2 files | configure & build | install daemon |
| Dependencies | none | none | entire ecosystem |
| Query language | filter strings *or* SQL, your pick | SQL | BSON query objects |
| Debugging | readable errors | "syntax error near..." | "BSON serialization..." |
| Street cred | 💯 | ❌ | 🤮 |

---

## **🚨 FAQ**

**Q: Is this production-ready?**
A: It's running on KumOS. You tell me.

**Q: How do I backup data?**
A: `cp -r mydata/ backup/` — congrats, you're a DBA now.

**Q: Thread safety?**
A: File-level `fcntl` locks on writes. Multiple readers fine. Don't do concurrent writes from separate processes without knowing what you're doing.

**Q: Can I contribute?**
A: Submit a PR or GTFO.

---

**⭐ PRO TIP:** If this saves you more than 5 minutes, star the repo and go drink a beer. You've earned it.
