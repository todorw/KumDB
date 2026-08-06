# KumDB Studio

A desktop front end for KumDB — browse tables like a spreadsheet, edit cells
in place, and run queries either the NoSQL way (filter strings) or in SQL,
both against the exact same database file. Built with Qt Widgets + CMake so
it builds the same way on Windows, macOS, and Linux.

It is **not** a clone of Microsoft Access's form/report designer — there's
no drag-and-drop form builder here, just a table browser, an editable
datasheet grid, and a query console. That covers the "view and edit my
data, run some queries" use case Access gets used for most; it does not
cover custom forms or printed reports.

## What it does

- **Datasheet view**: pick a table on the left, see/edit its rows in a grid.
  Editing a cell writes through immediately (same as Access's datasheet view
  — there's no separate save step). Add a row either via the toolbar dialog
  (several fields at once) or Access-style: just start typing into the `*`
  row at the bottom of the grid. A record navigator (First/Prev/Next/Last +
  row count) and an instant search box sit below/above the grid; export the
  whole thing to CSV.
- **Schema view**: a read-only breakdown of a table's columns — type,
  nullable, indexed, unique — for whatever the datasheet grid doesn't have
  room to show inline.
- **Query console**: toggle between NoSQL (pick a table, type filter lines
  like `age__gt=21`) and SQL (type a whole statement, including JOINs,
  subqueries, CTEs, window functions, and transactions). Same engine either
  way — see [DOCUMENTATION.md](../DOCUMENTATION.md) for the full syntax.
  Recent queries (either mode) are kept in a history dropdown; results can
  be exported to CSV too.
- **New Table dialog**: define a table's columns (name, type, NOT NULL,
  indexed, unique) before inserting anything, same as `CREATE TABLE` in SQL.
- Dark theme, applied app-wide via a Qt stylesheet (`resources/style.qss`).

## Building

Requires Qt 6 (or Qt 5) with the Widgets module, and CMake >= 3.16.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This also builds the KumDB C library directly from `../src` — you don't
need to run the top-level `make` first, `cmake --build` builds everything.

Run it:

```bash
./build/kumdb_studio [optional/path/to/a/database/folder]
```

### Windows / macOS notes

- Windows: MinGW-w64 only, not MSVC — the C engine leans on mingw's POSIX
  compat shims (a real `unistd.h`, etc.), and MSVC's CRT doesn't provide
  them. Easiest path is MSYS2's MINGW64 environment (`pacman -S
  mingw-w64-x86_64-{toolchain,cmake,ninja,qt6-base}`), then the same
  `cmake`/`cmake --build` commands from an MSYS2 MINGW64 shell. Package with
  `windeployqt build\kumdb_studio.exe`.
- macOS: `brew install qt cmake`, then the same `cmake`/`cmake --build`
  commands. Package with `macdeployqt build/kumdb_studio.app -dmg`.

## Tests

`kumdb_studio_handle_test` exercises the C++/C interop layer (`KumDbHandle`)
directly — open/create/insert/find/update/delete/SQL, no GUI needed:

```bash
ctest --test-dir build --output-on-failure
```
