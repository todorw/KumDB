# KumDB

*The database that doesn't waste your time.*

## What is it

KumDB is a lightweight embedded database engine written in pure C11. No
dependencies, no server process, no bullshit. A clean key-value-style API on
top of a fast binary file format with atomic writes, type inference, and
error messages that actually tell you what went wrong.

It didn't start out trying to be a SQL database or a NoSQL database — it was
just an embedded engine with its own filter-string query language. SQL
support (and the explicit NoSQL-style operators) got added later because it
turned out useful to speak both, not because the project set out to clone
either one. Both sit on the exact same storage and query engine underneath,
so results are always consistent between them. "No `JOIN` statements, that's
not up for debate" didn't survive contact with actually writing a SQL
dialect — there are real `JOIN`s now (inner/outer/theta/`CROSS`), plus
subqueries, CTEs, window functions, transactions, and real constraints. See
[`DOCUMENTATION.md`](DOCUMENTATION.md) for the full list.

```c
KdbField fields[] = {
    kdb_field_string("name",  "John"),
    kdb_field_int   ("age",   30),
    kdb_field_bool  ("admin", 1),
    kdb_field_end   ()
};
kdb_add(db, "users", fields);

const char *filters[] = { "name=John", NULL };
KdbRow *user = kdb_find_one(db, "users", filters);

// or, if SQL is more your thing -- same table, same data, same engine
kdb_exec_sql(db, "SELECT * FROM users WHERE name = 'John'", &rows, NULL);
```

This is **not** for people who need multi-writer concurrency, a
client/server protocol, an ORM, or user accounts/permissions — it's a
single-process embedded library, not a database server. It **is** for
dead-simple embedded persistence with real query power when you want it —
projects where SQLite feels like overkill, or where you just want to get
something working in C without any ceremony.

## Quick start

Build the library and tools:

```bash
make
```

Use it in your project:

```c
#include "kumdb.h"
```
```bash
gcc myapp.c build/libkumdb.a -lm -o myapp
```

Or just use the CLI:

```bash
./build/bin/kumdb_cli ./mydata
```

Full command/API reference, including the NoSQL filter syntax, the SQL
grammar, nested values, and every CLI command, lives in
[`DOCUMENTATION.md`](DOCUMENTATION.md).

## KumDB Studio

A desktop app (Windows/Mac/Linux, Qt) for people who'd rather click than
type: table browser, editable datasheet grid, and a query console that
toggles between NoSQL filters and SQL. Not an Access form/report designer
clone — just the "view and edit my data, run some queries" part, which is
most of what Access gets used for anyway. See
[`app/README.md`](app/README.md) for build instructions.

## Build targets

```bash
make              # release build -> build/libkumdb.a + build/bin/*
make debug        # ASan + UBSan + debug symbols
make check        # run all tests (use after make debug)
make distclean    # remove build/
```

## Performance

| Operation | Speed |
|---|---|
| Insert | ~50K ops/sec |
| Find (full scan) | ~200K rows/sec |
| Count | ~500K rows/sec |
| Update (batch) | ~40K ops/sec |

Numbers from `bench` on a mid-range machine — your mileage will vary. Run
`./build/bin/bench` to find out on yours.

## Comparison

| Feature | KumDB | SQLite | MongoDB |
|---|---|---|---|
| Learning curve | 5 minutes | 5 weeks | 5 years |
| Setup | drop in 2 files | configure & build | install a daemon |
| Dependencies | none | none | entire ecosystem |
| Query language | filter strings or SQL, your pick | SQL | BSON query objects |
| Debugging | readable errors | "syntax error near..." | "BSON serialization..." |

## FAQ

**Is this production-ready?**
It's running on KumOS. You tell me.

**How do I back up data?**
`cp -r mydata/ backup/` — congrats, you're a DBA now.

**Thread safety?**
File-level `fcntl` locks on writes. Multiple readers are fine. Don't do
concurrent writes from separate processes without knowing what you're doing.

**Can I contribute?**
Submit a PR.
