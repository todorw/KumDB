#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../include/kumdb.h"
#include "../include/sql.h"

static int passed = 0;
static int failed = 0;

#define ASSERT(expr) do { \
    if (expr) { passed++; } \
    else { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #expr); \
        failed++; \
    } \
} while (0)

#define ASSERT_EQ(a, b)  ASSERT((a) == (b))
#define ASSERT_STR(a, b) ASSERT(strcmp((a), (b)) == 0)
#define ASSERT_OK(st)    ASSERT((st) == KDB_OK)
#define ASSERT_ERR(st)   ASSERT((st) != KDB_OK)

#define TEST_DIR "/tmp/kumdb_test_sql"

static void setup(KumDB **db_out) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);
    KumDB *db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);
    *db_out = db;
}

static void teardown(KumDB *db) {
    kdb_close(db);
    system("rm -rf " TEST_DIR);
}

static KdbStatus sql(KumDB *db, const char *stmt) {
    return kdb_exec_sql(db, stmt, NULL, NULL);
}

static void test_create_insert_select(void) {
    KumDB *db;
    setup(&db);

    ASSERT_OK(sql(db, "CREATE TABLE students (name TEXT NOT NULL, age INT INDEX, gpa FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO students (name, age, gpa) VALUES ('Alice', 20, 3.9)"));
    ASSERT_OK(sql(db, "INSERT INTO students (name, age, gpa) VALUES ('Bob', 22, 3.1)"));
    ASSERT_OK(sql(db, "INSERT INTO students (name, age, gpa) VALUES ('Carol', 19, 3.5)"));

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM students WHERE age >= 20 ORDER BY age DESC", &rows, NULL));
    ASSERT(rows != NULL);
    if (rows) {
        ASSERT_EQ(rows->count, 2u);
        if (rows->count == 2) {
            int64_t age0 = 0, age1 = 0;
            ASSERT_OK(kdb_row_get_int(&rows->rows[0], "age", &age0));
            ASSERT_OK(kdb_row_get_int(&rows->rows[1], "age", &age1));
            ASSERT_EQ(age0, 22); /* DESC */
            ASSERT_EQ(age1, 20);
        }
        kdb_rows_free(rows);
    }

    teardown(db);
}

static void test_multi_row_insert_and_insert_select(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, age INT)"));

    KdbRows *rows = NULL;
    size_t affected = 0;

    /* multi-row VALUES */
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO t (name, age) VALUES ('alice', 30), ('bob', 25), ('carol', 40)",
        NULL, &affected));
    ASSERT_EQ(affected, 3u);
    ASSERT_EQ(kdb_count(db, "t", NULL), 3);

    ASSERT_OK(kdb_exec_sql(db, "SELECT name, age FROM t ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *n0 = NULL, *n1 = NULL, *n2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "name", &n2));
        ASSERT_STR(n0, "alice");
        ASSERT_STR(n1, "bob");
        ASSERT_STR(n2, "carol");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a mismatched tuple in a multi-row VALUES list is rejected */
    ASSERT_ERR(sql(db, "INSERT INTO t (name, age) VALUES ('dan', 22), ('eve')"));

    /* INSERT ... SELECT, filtered */
    ASSERT_OK(sql(db, "CREATE TABLE t2 (n TEXT, a INT)"));
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO t2 (n, a) SELECT name, age FROM t WHERE age > 26", NULL, &affected));
    ASSERT_EQ(affected, 2u); /* alice (30), carol (40) -- not bob (25) */

    ASSERT_OK(kdb_exec_sql(db, "SELECT n, a FROM t2 ORDER BY n ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL;
        int64_t a0 = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "n", &n0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "a", &a0));
        ASSERT_STR(n0, "alice");
        ASSERT_EQ(a0, 30);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* INSERT ... SELECT with an aggregate source */
    ASSERT_OK(sql(db, "CREATE TABLE counts (label TEXT, cnt INT)"));
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO counts (label, cnt) SELECT name, age FROM t WHERE name = 'alice'", NULL, &affected));
    ASSERT_EQ(affected, 1u);

    /* column count mismatch between the target list and the SELECT's
     * projected columns is rejected, nothing inserted */
    ASSERT_ERR(sql(db, "INSERT INTO t2 (n, a) SELECT name FROM t"));
    ASSERT_EQ(kdb_count(db, "t2", NULL), 2); /* unchanged from before */

    /* zero matching rows -- 0 affected, not an error */
    affected = 999;
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO t2 (n, a) SELECT name, age FROM t WHERE age > 1000", NULL, &affected));
    ASSERT_EQ(affected, 0u);

    /* single-row INSERT still works unchanged */
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO t (name, age) VALUES ('zed', 99)", NULL, &affected));
    ASSERT_EQ(affected, 1u);

    teardown(db);
}

static void test_upsert(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE users (email TEXT, name TEXT, visits INT)"));
    ASSERT_OK(sql(db, "INSERT INTO users (email, name, visits) VALUES ('a@x.com', 'Alice', 1)"));

    KdbRows *rows = NULL;
    size_t affected = 0;

    /* ON CONFLICT DO UPDATE against an existing row */
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO users (email, name, visits) VALUES ('a@x.com', 'Alice2', 2) "
        "ON CONFLICT (email) DO UPDATE SET name = 'Alice2', visits = 2",
        NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_EQ(kdb_count(db, "users", NULL), 1); /* still one row -- updated, not inserted */

    ASSERT_OK(kdb_exec_sql(db, "SELECT name, visits FROM users WHERE email = 'a@x.com'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        int64_t visits = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "visits", &visits));
        ASSERT_STR(name, "Alice2");
        ASSERT_EQ(visits, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* ON CONFLICT DO UPDATE with no existing match falls back to insert */
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO users (email, name, visits) VALUES ('b@x.com', 'Bob', 1) "
        "ON CONFLICT (email) DO UPDATE SET name = 'Bob', visits = 1",
        NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_EQ(kdb_count(db, "users", NULL), 2);

    /* ON CONFLICT DO NOTHING against an existing row is a true no-op */
    affected = 999;
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO users (email, name, visits) VALUES ('a@x.com', 'ShouldNotAppear', 99) ON CONFLICT (email) DO NOTHING",
        NULL, &affected));
    ASSERT_EQ(affected, 0u);
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM users WHERE email = 'a@x.com'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "Alice2"); /* unchanged */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* ON CONFLICT DO NOTHING with no existing match still inserts */
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO users (email, name, visits) VALUES ('c@x.com', 'Carol', 1) ON CONFLICT (email) DO NOTHING",
        NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_EQ(kdb_count(db, "users", NULL), 3);

    /* multi-column conflict key */
    ASSERT_OK(sql(db, "CREATE TABLE inv (warehouse TEXT, sku TEXT, qty INT)"));
    ASSERT_OK(sql(db, "INSERT INTO inv (warehouse, sku, qty) VALUES ('w1', 's1', 5)"));
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO inv (warehouse, sku, qty) VALUES ('w1', 's1', 10) "
        "ON CONFLICT (warehouse, sku) DO UPDATE SET qty = 10",
        NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_EQ(kdb_count(db, "inv", NULL), 1);

    /* same warehouse, different sku -- doesn't conflict, a fresh insert */
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO inv (warehouse, sku, qty) VALUES ('w1', 's2', 3) "
        "ON CONFLICT (warehouse, sku) DO UPDATE SET qty = 3",
        NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_EQ(kdb_count(db, "inv", NULL), 2);

    /* ON CONFLICT with a multi-row VALUES list is rejected */
    ASSERT_ERR(sql(db,
        "INSERT INTO users (email, name, visits) VALUES ('d@x.com','D',1),('e@x.com','E',1) ON CONFLICT (email) DO NOTHING"));

    /* an ON CONFLICT column that isn't in the INSERT column list errors */
    ASSERT_ERR(sql(db,
        "INSERT INTO users (email, name, visits) VALUES ('f@x.com','F',1) ON CONFLICT (nonexistent_col) DO NOTHING"));

    /* plain multi-row INSERT (no ON CONFLICT) still works unchanged */
    affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO users (email, name, visits) VALUES ('g@x.com','G',1), ('h@x.com','H',1)",
        NULL, &affected));
    ASSERT_EQ(affected, 2u);

    teardown(db);
}

static void test_returning(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, status TEXT)"));

    KdbRows *rows = NULL;

    /* RETURNING * excludes id/created_at/updated_at, same convention
     * plain SELECT * already uses -- name them explicitly to get them */
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO t (name, status) VALUES ('alice', 'new') RETURNING *", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "alice");
        ASSERT(kdb_row_get(&rows->rows[0], "id") == NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* single-row INSERT RETURNING id explicitly */
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO t (name, status) VALUES ('bob', 'new') RETURNING id, name", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t id = 0;
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "id", &id));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_EQ(id, 2);
        ASSERT_STR(name, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* multi-row INSERT RETURNING, in insertion order */
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO t (name, status) VALUES ('carol','new'), ('dave','new'), ('eve','new') RETURNING id, name",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *n0 = NULL, *n1 = NULL, *n2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "name", &n2));
        ASSERT_STR(n0, "carol");
        ASSERT_STR(n1, "dave");
        ASSERT_STR(n2, "eve");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* INSERT ... SELECT RETURNING */
    ASSERT_OK(sql(db, "CREATE TABLE t2 (name TEXT)"));
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO t2 (name) SELECT name FROM t WHERE status = 'new' RETURNING id, name",
        &rows, NULL));
    ASSERT(rows && rows->count == 5u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* UPDATE RETURNING gives the post-update state */
    ASSERT_OK(kdb_exec_sql(db,
        "UPDATE t SET status = 'active' WHERE name = 'alice' RETURNING id, name, status",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *status = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "status", &status));
        ASSERT_STR(status, "active");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* UPDATE RETURNING when WHERE and SET reference the same column --
     * re-fetching by the original WHERE filter after the update would
     * wrongly find nothing, so this specifically checks the id-based
     * re-targeting path instead */
    ASSERT_OK(kdb_exec_sql(db,
        "UPDATE t SET status = 'done' WHERE status = 'new' RETURNING id, name, status",
        &rows, NULL));
    ASSERT(rows && rows->count == 4u); /* bob, carol, dave, eve */
    if (rows) {
        for (size_t i = 0; i < rows->count; i++) {
            const char *status = NULL;
            ASSERT_OK(kdb_row_get_string(&rows->rows[i], "status", &status));
            ASSERT_STR(status, "done");
        }
        kdb_rows_free(rows); rows = NULL;
    }

    /* DELETE RETURNING gives the pre-delete image */
    ASSERT_OK(kdb_exec_sql(db, "DELETE FROM t WHERE name = 'alice' RETURNING id, name, status", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *status = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "status", &status));
        ASSERT_STR(status, "active");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }
    {
        const char *alice_filter[] = { "name=alice", NULL };
        ASSERT_EQ(kdb_count(db, "t", alice_filter), 0);
    }

    /* DELETE RETURNING with a parenthesized WHERE (the id-list path) */
    ASSERT_OK(kdb_exec_sql(db,
        "DELETE FROM t WHERE (name = 'bob' OR name = 'carol') RETURNING id, name",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* upsert RETURNING -- insert branch, update branch, and DO NOTHING's
     * true no-op (0 rows, not an error) */
    ASSERT_OK(sql(db, "CREATE TABLE u (email TEXT, visits INT)"));
    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO u (email, visits) VALUES ('a@x.com', 1) ON CONFLICT (email) DO UPDATE SET visits = 1 RETURNING id, visits",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    int64_t first_id = 0;
    if (rows && rows->count == 1) ASSERT_OK(kdb_row_get_int(&rows->rows[0], "id", &first_id));
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO u (email, visits) VALUES ('a@x.com', 2) ON CONFLICT (email) DO UPDATE SET visits = 2 RETURNING id, visits",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t id = 0, visits = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "id", &id));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "visits", &visits));
        ASSERT_EQ(id, first_id); /* same row, updated -- not a new insert */
        ASSERT_EQ(visits, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db,
        "INSERT INTO u (email, visits) VALUES ('a@x.com', 99) ON CONFLICT (email) DO NOTHING RETURNING id, visits",
        &rows, NULL));
    ASSERT(rows && rows->count == 0u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* RETURNING still parses fine when the caller passes rows_out=NULL */
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO t (name, status) VALUES ('zed', 'x') RETURNING *", NULL, NULL));

    /* statements without RETURNING still work unchanged */
    size_t affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "INSERT INTO t (name, status) VALUES ('yolanda', 'x')", NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_OK(kdb_exec_sql(db, "UPDATE t SET status = 'y' WHERE name = 'yolanda'", NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_OK(kdb_exec_sql(db, "DELETE FROM t WHERE name = 'yolanda'", NULL, &affected));
    ASSERT_EQ(affected, 1u);

    teardown(db);
}

static int64_t count_all(KumDB *db, const char *table) {
    char q[128];
    snprintf(q, sizeof(q), "SELECT * FROM %s", table);
    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, q, &rows, NULL));
    int64_t n = rows ? (int64_t)rows->count : -1;
    if (rows) kdb_rows_free(rows);
    return n;
}

static void test_transactions(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('alice')"));

    /* COMMIT keeps the writes */
    ASSERT_OK(sql(db, "BEGIN"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('bob')"));
    ASSERT_EQ(count_all(db, "t"), 2);
    ASSERT_OK(sql(db, "COMMIT"));
    ASSERT_EQ(count_all(db, "t"), 2);

    /* ROLLBACK undoes every statement in the transaction, across
     * INSERT/UPDATE/DELETE together */
    ASSERT_OK(sql(db, "BEGIN TRANSACTION"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('carol')"));
    ASSERT_OK(sql(db, "UPDATE t SET name = 'alice2' WHERE name = 'alice'"));
    ASSERT_OK(sql(db, "DELETE FROM t WHERE name = 'bob'"));
    ASSERT_EQ(count_all(db, "t"), 2); /* alice2, carol */
    ASSERT_OK(sql(db, "ROLLBACK"));
    ASSERT_EQ(count_all(db, "t"), 2); /* back to alice, bob */
    {
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE name = 'alice'", &rows, NULL));
        ASSERT(rows && rows->count == 1u);
        if (rows) kdb_rows_free(rows);
    }

    /* START TRANSACTION is accepted as an alias for BEGIN */
    ASSERT_OK(sql(db, "START TRANSACTION"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('dave')"));
    ASSERT_OK(sql(db, "COMMIT"));
    ASSERT_EQ(count_all(db, "t"), 3);

    /* error paths */
    ASSERT_ERR(sql(db, "COMMIT"));                 /* nothing open */
    ASSERT_ERR(sql(db, "ROLLBACK"));                /* nothing open */
    ASSERT_OK(sql(db, "BEGIN"));
    ASSERT_ERR(sql(db, "BEGIN"));                   /* no nested transactions */
    ASSERT_ERR(sql(db, "CREATE TABLE u (x INT)"));  /* DDL isn't transactional here */
    ASSERT_ERR(sql(db, "SAVEPOINT sp1"));           /* not supported, see docs */
    ASSERT_ERR(sql(db, "ROLLBACK TO SAVEPOINT sp1"));
    ASSERT_ERR(sql(db, "RELEASE SAVEPOINT sp1"));
    ASSERT_OK(sql(db, "ROLLBACK")); /* clean up the still-open transaction */

    /* a transaction left open when the handle closes is rolled back, same
     * as a crash mid-transaction would be -- reopen the same directory
     * (not teardown()/setup(), which would wipe it) to check */
    ASSERT_OK(sql(db, "BEGIN"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('ghost')"));
    ASSERT_EQ(count_all(db, "t"), 4);
    kdb_close(db);

    db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);
    ASSERT_EQ(count_all(db, "t"), 3); /* ghost rolled back automatically */

    teardown(db);
}

static void test_projection(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (a INT, b INT, c INT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (a, b, c) VALUES (1, 2, 3)"));

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT a, c FROM t", &rows, NULL));
    ASSERT(rows != NULL);
    if (rows && rows->count == 1) {
        ASSERT_EQ(rows->rows[0].field_count, 2u);
        ASSERT(kdb_row_get(&rows->rows[0], "a") != NULL);
        ASSERT(kdb_row_get(&rows->rows[0], "c") != NULL);
        ASSERT(kdb_row_get(&rows->rows[0], "b") == NULL);
    }
    if (rows) kdb_rows_free(rows);

    /* AS on a plain (non-aggregate) column renames the output field */
    ASSERT_OK(kdb_exec_sql(db, "SELECT a AS first FROM t", &rows, NULL));
    if (rows && rows->count == 1) {
        ASSERT_EQ(rows->rows[0].field_count, 1u);
        ASSERT(kdb_row_get(&rows->rows[0], "first") != NULL);
        ASSERT(kdb_row_get(&rows->rows[0], "a") == NULL);
        int64_t v = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "first", &v));
        ASSERT_EQ(v, 1);
    }
    if (rows) kdb_rows_free(rows);

    teardown(db);
}

static void test_limit_offset(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (n INT)"));
    for (int i = 0; i < 10; i++) {
        char stmt[64];
        snprintf(stmt, sizeof(stmt), "INSERT INTO t (n) VALUES (%d)", i);
        ASSERT_OK(sql(db, stmt));
    }

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t ORDER BY n ASC LIMIT 3 OFFSET 2", &rows, NULL));
    ASSERT(rows != NULL);
    if (rows) {
        ASSERT_EQ(rows->count, 3u);
        if (rows->count == 3) {
            int64_t n = 0;
            ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n));
            ASSERT_EQ(n, 2);
        }
        kdb_rows_free(rows);
    }

    teardown(db);
}

static void test_multi_column_order_by(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (region TEXT, amount INT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (region, amount) VALUES ('east', 100)"));
    ASSERT_OK(sql(db, "INSERT INTO t (region, amount) VALUES ('east', 50)"));
    ASSERT_OK(sql(db, "INSERT INTO t (region, amount) VALUES ('west', 200)"));
    ASSERT_OK(sql(db, "INSERT INTO t (region, amount) VALUES ('west', 10)"));

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT region, amount FROM t ORDER BY region ASC, amount DESC", &rows, NULL));
    ASSERT(rows && rows->count == 4u);
    if (rows && rows->count == 4) {
        int64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        const char *r0 = NULL, *r2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &r0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "amount", &a0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "amount", &a1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "region", &r2));
        ASSERT_OK(kdb_row_get_int(&rows->rows[2], "amount", &a2));
        ASSERT_OK(kdb_row_get_int(&rows->rows[3], "amount", &a3));
        ASSERT_STR(r0, "east"); ASSERT_EQ(a0, 100); /* east, amount desc: 100 then 50 */
        ASSERT_EQ(a1, 50);
        ASSERT_STR(r2, "west"); ASSERT_EQ(a2, 200); /* west, amount desc: 200 then 10 */
        ASSERT_EQ(a3, 10);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* mixed directions per column */
    ASSERT_OK(kdb_exec_sql(db, "SELECT region, amount FROM t ORDER BY region DESC, amount ASC", &rows, NULL));
    ASSERT(rows && rows->count == 4u);
    if (rows && rows->count == 4) {
        const char *r0 = NULL; int64_t a0 = 0, a1 = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &r0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "amount", &a0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "amount", &a1));
        ASSERT_STR(r0, "west"); ASSERT_EQ(a0, 10); /* west, amount asc: 10 then 200 */
        ASSERT_EQ(a1, 200);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* combined with LIMIT */
    ASSERT_OK(kdb_exec_sql(db, "SELECT region, amount FROM t ORDER BY region ASC, amount DESC LIMIT 2", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* too many ORDER BY columns is rejected, not silently truncated */
    ASSERT_ERR(sql(db, "SELECT * FROM t ORDER BY region, amount, region, amount, region"));

    teardown(db);
}

static void test_update_delete(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, score INT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, score) VALUES ('x', 1)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, score) VALUES ('y', 2)"));

    size_t affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "UPDATE t SET score = 99 WHERE name = 'x'", NULL, &affected));
    ASSERT_EQ(affected, 1u);

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE name = 'x'", &rows, NULL));
    if (rows && rows->count == 1) {
        int64_t score = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "score", &score));
        ASSERT_EQ(score, 99);
    }
    if (rows) kdb_rows_free(rows);

    affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "DELETE FROM t WHERE score = 2", NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_EQ(kdb_count(db, "t", NULL), 1);

    teardown(db);
}

static void test_where_operators(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (n INT, s TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (n, s) VALUES (0, 'zero')"));
    ASSERT_OK(sql(db, "INSERT INTO t (n, s) VALUES (1, 'one')"));
    ASSERT_OK(sql(db, "INSERT INTO t (n, s) VALUES (5, 'five')"));

    KdbRows *rows = NULL;

    /* "0"/"1" literal vs bool/int regression, via SQL this time */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n = 0", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n = 1", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n != 1", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s LIKE 'f%'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* mid-pattern % and single-char _ wildcards -- real SQL LIKE, not just
     * the leading/trailing-only version this used to be limited to */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s LIKE 'f%e'", &rows, NULL));
    ASSERT(rows && rows->count == 1u); /* "five" */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s LIKE '_ne'", &rows, NULL));
    ASSERT(rows && rows->count == 1u); /* "one" */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s LIKE 'z_ro'", &rows, NULL));
    ASSERT(rows && rows->count == 1u); /* "zero" */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s LIKE 'nomatch'", &rows, NULL));
    ASSERT(rows && rows->count == 0u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* ILIKE: same wildcards as LIKE, case-insensitive */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s ILIKE 'F%'", &rows, NULL));
    ASSERT(rows && rows->count == 1u); /* "five" */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s LIKE 'F%'", &rows, NULL));
    ASSERT(rows && rows->count == 0u); /* plain LIKE stays case-sensitive */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* REGEXP */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s REGEXP '^(f|z)'", &rows, NULL));
    ASSERT(rows && rows->count == 0u); /* alternation isn't supported -- treated as a literal '(f|z)' */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s REGEXP '^f'", &rows, NULL));
    ASSERT(rows && rows->count == 1u); /* "five" */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s REGEXP 'o.e'", &rows, NULL));
    ASSERT(rows && rows->count == 1u); /* "one" */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n BETWEEN 1 AND 5", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* IN used to have no SQL syntax at all, and the underlying operator was a broken stub */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n IN (0, 5)", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE s IN ('one', 'nope')", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE id = 1", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_regexp_and_ilike(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, email TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, email) VALUES ('Alice', 'alice@example.com')"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, email) VALUES ('bob', 'bob@test.org')"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, email) VALUES ('Carol', 'not-an-email')"));

    KdbRows *rows = NULL;

    /* a reasonably real-world REGEXP: basic email shape */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM t WHERE email REGEXP '^[a-z]+@[a-z]+\\.[a-z]+$' ORDER BY name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL, *n1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_STR(n0, "Alice");
        ASSERT_STR(n1, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* ILIKE matches regardless of case; the parallel LIKE doesn't */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE name ILIKE 'a%'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE name LIKE 'a%'", &rows, NULL));
    ASSERT(rows && rows->count == 0u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* REGEXP/ILIKE both work inside a parenthesized WHERE (the in-memory
     * condition-tree evaluation path, not the flat filter-string
     * pushdown path plain conditions normally take) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM t WHERE (name ILIKE 'a%' OR email REGEXP 'test') ORDER BY name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL, *n1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_STR(n0, "Alice");
        ASSERT_STR(n1, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* only a string literal pattern is accepted, same rule LIKE follows */
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE name REGEXP 5"));
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE name ILIKE 5"));

    teardown(db);
}

static void test_bound_params(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, age INT, active BOOL)"));

    KdbRows *rows = NULL;
    size_t affected = 0;

    /* positional '?' placeholders */
    {
        KdbField params[] = {
            kdb_field_string(NULL, "alice"),
            kdb_field_int(NULL, 30),
            kdb_field_bool(NULL, 1),
        };
        ASSERT_OK(kdb_exec_sql_params(db, "INSERT INTO t (name, age, active) VALUES (?, ?, ?)",
                                       params, 3, NULL, &affected));
        ASSERT_EQ(affected, 1u);
    }

    /* a string param containing a quote needs no caller-side escaping */
    {
        KdbField params[] = {
            kdb_field_string(NULL, "O'Brien"),
            kdb_field_int(NULL, 45),
            kdb_field_bool(NULL, 0),
        };
        ASSERT_OK(kdb_exec_sql_params(db, "INSERT INTO t (name, age, active) VALUES (?, ?, ?)",
                                       params, 3, NULL, NULL));
    }
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE name = 'O''Brien'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a NULL param */
    {
        KdbField params[] = { kdb_field_null(NULL), kdb_field_int(NULL, 5), kdb_field_bool(NULL, 0) };
        ASSERT_OK(kdb_exec_sql_params(db, "INSERT INTO t (name, age, active) VALUES (?, ?, ?)",
                                       params, 3, NULL, NULL));
    }
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE name IS NULL", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* explicit '$N' index, and reusing the same param twice */
    {
        KdbField params[] = { kdb_field_int(NULL, 30) };
        ASSERT_OK(kdb_exec_sql_params(db, "SELECT name FROM t WHERE age = $1 OR age = $1",
                                       params, 1, &rows, NULL));
        ASSERT(rows && rows->count == 1u);
        if (rows) { kdb_rows_free(rows); rows = NULL; }
    }

    /* '?' and '$N' mixed in one statement */
    {
        KdbField params[] = { kdb_field_int(NULL, 45), kdb_field_string(NULL, "O'Brien") };
        ASSERT_OK(kdb_exec_sql_params(db, "SELECT name FROM t WHERE age = ? AND name = $2",
                                       params, 2, &rows, NULL));
        ASSERT(rows && rows->count == 1u);
        if (rows) { kdb_rows_free(rows); rows = NULL; }
    }

    /* a '?'/'$1' that's just text inside a string literal or a comment
     * isn't treated as a placeholder */
    {
        KdbField params[] = { kdb_field_bool(NULL, 1) };
        ASSERT_OK(kdb_exec_sql_params(db,
            "INSERT INTO t (name, age, active) VALUES ('literal ? mark', 1, ?) -- trailing $1 comment",
            params, 1, NULL, NULL));
    }
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE name = 'literal ? mark'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* UPDATE ... RETURNING with bound params */
    {
        KdbField params[] = { kdb_field_int(NULL, 99), kdb_field_string(NULL, "alice") };
        ASSERT_OK(kdb_exec_sql_params(db, "UPDATE t SET age = ? WHERE name = ? RETURNING name, age",
                                       params, 2, &rows, NULL));
        ASSERT(rows && rows->count == 1u);
        if (rows && rows->count == 1) {
            int64_t age = 0;
            ASSERT_OK(kdb_row_get_int(&rows->rows[0], "age", &age));
            ASSERT_EQ(age, 99);
        }
        if (rows) { kdb_rows_free(rows); rows = NULL; }
    }

    /* error paths: too few params, a $0 index, and a BLOB param (no SQL
     * literal form) all fail with a message, not a crash */
    {
        KdbField params[] = { kdb_field_int(NULL, 1) };
        ASSERT_ERR(kdb_exec_sql_params(db, "SELECT * FROM t WHERE age = ? AND active = ?",
                                        params, 1, &rows, NULL));
        ASSERT_ERR(kdb_exec_sql_params(db, "SELECT * FROM t WHERE age = $0", params, 1, &rows, NULL));
    }
    {
        KdbField params[] = { kdb_field_blob(NULL, "xy", 2) };
        ASSERT_ERR(kdb_exec_sql_params(db, "SELECT * FROM t WHERE name = ?", params, 1, &rows, NULL));
    }
    ASSERT_ERR(kdb_exec_sql_params(NULL, "SELECT 1", NULL, 0, NULL, NULL));

    teardown(db);
}

static void test_or(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (n INT, s TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (n, s) VALUES (0, 'zero')"));
    ASSERT_OK(sql(db, "INSERT INTO t (n, s) VALUES (1, 'one')"));
    ASSERT_OK(sql(db, "INSERT INTO t (n, s) VALUES (5, 'five')"));
    ASSERT_OK(sql(db, "INSERT INTO t (n, s) VALUES (10, 'ten')"));

    KdbRows *rows = NULL;

    /* n < 1 OR n > 5 -> 0 and 10 */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n < 1 OR n > 5", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* AND binds tighter than OR: (n = 5 AND s = 'five') OR n = 0 -> two rows */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n = 5 AND s = 'five' OR n = 0", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* same precedence check but the AND-condition shouldn't match on its own terms */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE n = 5 AND s = 'wrong' OR n = 0", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_parenthesized_where(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE people (name TEXT, age INT, dept TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO people (name, age, dept) VALUES ('alice', 17, 'eng')"));
    ASSERT_OK(sql(db, "INSERT INTO people (name, age, dept) VALUES ('bob', 70, 'eng')"));
    ASSERT_OK(sql(db, "INSERT INTO people (name, age, dept) VALUES ('carol', 30, 'sales')"));
    ASSERT_OK(sql(db, "INSERT INTO people (name, age, dept) VALUES ('dave', 40, 'eng')"));

    KdbRows *rows = NULL;

    /* without parens, AND binds tighter: age<18 OR (age>65 AND dept='sales') -> just alice */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM people WHERE age < 18 OR age > 65 AND dept = 'sales'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* parens override precedence: (age<18 OR age>65) AND dept='sales' -> nobody */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM people WHERE (age < 18 OR age > 65) AND dept = 'sales'", &rows, NULL));
    ASSERT(rows && rows->count == 0u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* same, but dept='eng' -> alice and bob */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM people WHERE (age < 18 OR age > 65) AND dept = 'eng'", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* nested parens */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT * FROM people WHERE ((age < 18 OR age > 65) AND dept = 'eng') OR name = 'carol'",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* UPDATE/DELETE with parens fall back to a full scan + id match, but
     * still only touch the rows the tree actually matches. */
    size_t affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "UPDATE people SET age = 99 WHERE (age < 18 OR age > 65) AND dept = 'eng'", NULL, &affected));
    ASSERT_EQ(affected, 2u);
    ASSERT_EQ(kdb_count(db, "people", NULL), 4);

    affected = 0;
    ASSERT_OK(kdb_exec_sql(db, "DELETE FROM people WHERE (age = 99 OR age = 30) AND dept != 'sales'", NULL, &affected));
    ASSERT_EQ(affected, 2u);
    ASSERT_EQ(kdb_count(db, "people", NULL), 2);

    /* parens that match nothing shouldn't touch any row */
    affected = 1;
    ASSERT_OK(kdb_exec_sql(db, "DELETE FROM people WHERE (age = 12345 OR age = 54321)", NULL, &affected));
    ASSERT_EQ(affected, 0u);
    ASSERT_EQ(kdb_count(db, "people", NULL), 2);

    /* mismatched parens are a syntax error */
    ASSERT_ERR(sql(db, "SELECT * FROM people WHERE (age < 18 OR age > 65"));

    teardown(db);
}

static void test_group_by_and_aggregates(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE sales (region TEXT, amount FLOAT, qty INT)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount, qty) VALUES ('east', 100.0, 5)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount, qty) VALUES ('east', 200.0, 3)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount, qty) VALUES ('west', 50.0, 10)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount, qty) VALUES ('west', 75.0, 2)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount, qty) VALUES ('north', 500.0, 1)"));

    KdbRows *rows = NULL;

    /* no GROUP BY: one summary row */
    ASSERT_OK(kdb_exec_sql(db, "SELECT COUNT(*), SUM(amount), AVG(amount), MIN(qty), MAX(qty) FROM sales", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t cnt = 0; double sum = 0, avg = 0, mn = 0, mx = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "COUNT(*)", &cnt));
        ASSERT_EQ(cnt, 5);
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "SUM(amount)", &sum));
        ASSERT(sum > 924.9 && sum < 925.1);
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "AVG(amount)", &avg));
        ASSERT(avg > 184.9 && avg < 185.1);
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "MIN(qty)", &mn));
        ASSERT(mn > 0.9 && mn < 1.1);
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "MAX(qty)", &mx));
        ASSERT(mx > 9.9 && mx < 10.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* GROUP BY with COUNT + aliased SUM */
    ASSERT_OK(kdb_exec_sql(db, "SELECT region, COUNT(*), SUM(amount) AS total FROM sales GROUP BY region ORDER BY total DESC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *region = NULL;
        double total = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &region));
        ASSERT_STR(region, "north"); /* highest total (500), DESC order */
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "total", &total));
        ASSERT(total > 499.9 && total < 500.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* LIMIT on the aggregated/sorted output */
    ASSERT_OK(kdb_exec_sql(db, "SELECT region, SUM(amount) AS total FROM sales GROUP BY region ORDER BY total DESC LIMIT 1", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* GROUP BY on a plain column with no aggregate = distinct-like */
    ASSERT_OK(kdb_exec_sql(db, "SELECT region FROM sales GROUP BY region", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* validation errors */
    ASSERT_ERR(sql(db, "SELECT region, amount FROM sales GROUP BY region")); /* amount not grouped/aggregated */
    ASSERT_ERR(sql(db, "SELECT * FROM sales GROUP BY region"));              /* '*' with GROUP BY */
    ASSERT_ERR(sql(db, "SELECT SUM(*) FROM sales"));                        /* only COUNT(*) is valid */

    teardown(db);
}

static void test_group_by_extensions(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE sales (region TEXT, rep TEXT, amount INT)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('east', 'alice', 100)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('east', 'alice', 50)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('east', 'bob', 30)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('west', 'carol', 70)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('west', 'carol', 70)"));

    KdbRows *rows = NULL;

    /* COUNT(DISTINCT col) per group, alongside a plain COUNT(*) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region, COUNT(DISTINCT rep) AS n_reps, COUNT(*) AS n_sales FROM sales GROUP BY region ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        int64_t n_reps0 = 0, n_sales0 = 0, n_reps1 = 0, n_sales1 = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n_reps", &n_reps0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n_sales", &n_sales0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "n_reps", &n_reps1));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "n_sales", &n_sales1));
        ASSERT_EQ(n_reps0, 2);  /* east: alice, bob */
        ASSERT_EQ(n_sales0, 3);
        ASSERT_EQ(n_reps1, 1);  /* west: carol only */
        ASSERT_EQ(n_sales1, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* COUNT(DISTINCT) with no GROUP BY (one summary row) */
    ASSERT_OK(kdb_exec_sql(db, "SELECT COUNT(DISTINCT region) AS n FROM sales", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t n = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n));
        ASSERT_EQ(n, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* STRING_AGG(col, sep) per group */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region, STRING_AGG(rep, ',') AS reps FROM sales GROUP BY region ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *reps0 = NULL, *reps1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "reps", &reps0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "reps", &reps1));
        ASSERT_STR(reps0, "alice,alice,bob");
        ASSERT_STR(reps1, "carol,carol");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* GROUP_CONCAT is the same function under MySQL's name */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region, GROUP_CONCAT(rep, '|') AS reps FROM sales GROUP BY region ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *reps0 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "reps", &reps0));
        ASSERT_STR(reps0, "alice|alice|bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* STRING_AGG composes with other aggregates in the same SELECT */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region, STRING_AGG(rep, ',') AS reps, SUM(amount) AS total FROM sales GROUP BY region ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        double total0 = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "total", &total0));
        ASSERT(total0 > 179.9 && total0 < 180.1); /* east: 100+50+30 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* STRING_AGG as a window function (OVER) isn't supported */
    ASSERT_ERR(sql(db, "SELECT STRING_AGG(rep, ',') OVER (PARTITION BY region) FROM sales"));

    /* COUNT(DISTINCT) on an empty result set is 0, not an error */
    ASSERT_OK(kdb_exec_sql(db, "SELECT COUNT(DISTINCT rep) AS n FROM sales WHERE region = 'nowhere'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t n = -1;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n));
        ASSERT_EQ(n, 0);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_aggregate_filter(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE orders (dept TEXT, amount FLOAT, status TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO orders (dept, amount, status) VALUES ('eng', 100, 'done')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (dept, amount, status) VALUES ('eng', 50, 'pending')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (dept, amount, status) VALUES ('eng', 200, 'done')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (dept, amount, status) VALUES ('sales', 10, 'done')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (dept, amount, status) VALUES ('sales', 20, 'pending')"));

    KdbRows *rows = NULL;

    /* COUNT(*) FILTER (WHERE ...) alongside a plain COUNT(*), per group */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT dept, COUNT(*) AS total, COUNT(*) FILTER (WHERE status = 'done') AS done_count "
        "FROM orders GROUP BY dept ORDER BY dept ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        int64_t total0 = 0, done0 = 0, total1 = 0, done1 = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "total", &total0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "done_count", &done0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "total", &total1));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "done_count", &done1));
        ASSERT_EQ(total0, 3); ASSERT_EQ(done0, 2); /* eng: 3 rows, 2 done */
        ASSERT_EQ(total1, 2); ASSERT_EQ(done1, 1); /* sales: 2 rows, 1 done */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* SUM(...) FILTER (WHERE ...) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT dept, SUM(amount) FILTER (WHERE status = 'done') AS done_sum "
        "FROM orders GROUP BY dept ORDER BY dept ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        double s0 = 0, s1 = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "done_sum", &s0));
        ASSERT_OK(kdb_row_get_float(&rows->rows[1], "done_sum", &s1));
        ASSERT(s0 > 299.9 && s0 < 300.1); /* eng: 100 + 200 */
        ASSERT(s1 > 9.9 && s1 < 10.1);    /* sales: 10 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* FILTER with AND, no GROUP BY -- single summary row */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT COUNT(*) FILTER (WHERE dept = 'eng' AND status = 'done') AS c FROM orders", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t c = -1;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "c", &c));
        ASSERT_EQ(c, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* COUNT(DISTINCT col) FILTER (WHERE ...) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT COUNT(DISTINCT dept) FILTER (WHERE status = 'done') AS c FROM orders", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t c = -1;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "c", &c));
        ASSERT_EQ(c, 2); /* both eng and sales have at least one done row */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* STRING_AGG(...) FILTER (WHERE ...) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT dept, STRING_AGG(status, ',') FILTER (WHERE amount > 15) AS agg "
        "FROM orders GROUP BY dept ORDER BY dept ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *a1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "agg", &a1));
        ASSERT_STR(a1, "pending"); /* sales: only the amount=20 row clears >15 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* FILTER isn't supported combined with OVER (window functions) */
    ASSERT_ERR(sql(db, "SELECT SUM(amount) FILTER (WHERE status = 'done') OVER (PARTITION BY dept) FROM orders"));

    teardown(db);
}

static void test_multi_column_group_by(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE sales (region TEXT, product TEXT, amount FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, product, amount) VALUES ('east', 'a', 100.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, product, amount) VALUES ('east', 'a', 50.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, product, amount) VALUES ('east', 'b', 30.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, product, amount) VALUES ('west', 'a', 10.0)"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region, product, SUM(amount) AS total FROM sales GROUP BY region, product ORDER BY total DESC",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *region = NULL, *product = NULL;
        double total = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &region));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "product", &product));
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "total", &total));
        ASSERT_STR(region, "east");
        ASSERT_STR(product, "a");
        ASSERT(total > 149.9 && total < 150.1); /* 100 + 50 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* HAVING on top of a multi-column GROUP BY */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region, product, SUM(amount) AS total FROM sales GROUP BY region, product HAVING total > 40",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a selected column that's in neither GROUP BY list nor an aggregate is still rejected */
    ASSERT_ERR(sql(db, "SELECT region, product, amount FROM sales GROUP BY region, product"));

    teardown(db);
}

static void test_having(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE sales (region TEXT, amount FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('east', 100.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('east', 200.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('west', 50.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('north', 500.0)"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region, SUM(amount) AS total FROM sales GROUP BY region HAVING total > 90 ORDER BY total DESC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *r0 = NULL, *r1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &r0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "region", &r1));
        ASSERT_STR(r0, "north");
        ASSERT_STR(r1, "east");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* HAVING with COUNT(*) */
    ASSERT_OK(kdb_exec_sql(db, "SELECT region, COUNT(*) AS n FROM sales GROUP BY region HAVING n >= 2", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *r0 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &r0));
        ASSERT_STR(r0, "east");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* HAVING without GROUP BY/aggregate is rejected */
    ASSERT_ERR(sql(db, "SELECT region FROM sales HAVING region = 'east'"));

    teardown(db);
}

static void test_union(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t1 (name TEXT)"));
    ASSERT_OK(sql(db, "CREATE TABLE t2 (name TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t1 (name) VALUES ('alice')"));
    ASSERT_OK(sql(db, "INSERT INTO t1 (name) VALUES ('bob')"));
    ASSERT_OK(sql(db, "INSERT INTO t2 (name) VALUES ('bob')"));
    ASSERT_OK(sql(db, "INSERT INTO t2 (name) VALUES ('carol')"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t1 UNION SELECT name FROM t2 ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *n0 = NULL, *n1 = NULL, *n2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "name", &n2));
        ASSERT_STR(n0, "alice");
        ASSERT_STR(n1, "bob");
        ASSERT_STR(n2, "carol");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t1 UNION ALL SELECT name FROM t2", &rows, NULL));
    ASSERT(rows && rows->count == 4u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* 3-way chain with ORDER BY + LIMIT applying to the combined result */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM t1 UNION SELECT name FROM t2 UNION SELECT name FROM t1 ORDER BY name DESC LIMIT 1",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *n0 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_STR(n0, "carol");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* column-count mismatch is rejected */
    ASSERT_OK(sql(db, "CREATE TABLE t3 (name TEXT, age INT)"));
    ASSERT_OK(sql(db, "INSERT INTO t3 (name, age) VALUES ('dan', 5)"));
    ASSERT_ERR(sql(db, "SELECT name FROM t1 UNION SELECT name, age FROM t3"));

    /* mixing UNION and UNION ALL in one chain is rejected */
    ASSERT_ERR(sql(db, "SELECT name FROM t1 UNION SELECT name FROM t2 UNION ALL SELECT name FROM t1"));

    teardown(db);
}

static void test_intersect_except(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE a (v INT)"));
    ASSERT_OK(sql(db, "CREATE TABLE b (v INT)"));
    /* a: 1,2,2,3   b: 2,3,3,4 */
    ASSERT_OK(sql(db, "INSERT INTO a (v) VALUES (1)"));
    ASSERT_OK(sql(db, "INSERT INTO a (v) VALUES (2)"));
    ASSERT_OK(sql(db, "INSERT INTO a (v) VALUES (2)"));
    ASSERT_OK(sql(db, "INSERT INTO a (v) VALUES (3)"));
    ASSERT_OK(sql(db, "INSERT INTO b (v) VALUES (2)"));
    ASSERT_OK(sql(db, "INSERT INTO b (v) VALUES (3)"));
    ASSERT_OK(sql(db, "INSERT INTO b (v) VALUES (3)"));
    ASSERT_OK(sql(db, "INSERT INTO b (v) VALUES (4)"));

    KdbRows *rows = NULL;

    /* INTERSECT dedupes -- distinct values present on both sides */
    ASSERT_OK(kdb_exec_sql(db, "SELECT v FROM a INTERSECT SELECT v FROM b ORDER BY v ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* INTERSECT ALL: multiset intersection (min occurrence count each side) */
    ASSERT_OK(kdb_exec_sql(db, "SELECT v FROM a INTERSECT ALL SELECT v FROM b ORDER BY v ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u); /* one 2 (a has 2, b has 1 -> min 1), one 3 (a has 1, b has 2 -> min 1) */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* EXCEPT dedupes -- distinct values only on the left */
    ASSERT_OK(kdb_exec_sql(db, "SELECT v FROM a EXCEPT SELECT v FROM b", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t v = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "v", &v));
        ASSERT_EQ(v, 1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* EXCEPT ALL: multiset difference (a's count minus b's count per value) */
    ASSERT_OK(kdb_exec_sql(db, "SELECT v FROM a EXCEPT ALL SELECT v FROM b ORDER BY v ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u); /* 1 (1-0), 2 (2-1=1 survives), 3 (1-2 -> 0, gone) */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* chains 3 arms deep */
    ASSERT_OK(sql(db, "CREATE TABLE c (v INT)"));
    ASSERT_OK(sql(db, "INSERT INTO c (v) VALUES (3)"));
    ASSERT_OK(sql(db, "INSERT INTO c (v) VALUES (9)"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT v FROM a INTERSECT SELECT v FROM b INTERSECT SELECT v FROM c", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t v = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "v", &v));
        ASSERT_EQ(v, 3);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* ORDER BY/LIMIT apply to the combined result */
    ASSERT_OK(kdb_exec_sql(db, "SELECT v FROM a EXCEPT SELECT v FROM b ORDER BY v ASC LIMIT 5", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* an INTERSECT that empties out stays empty and stable */
    ASSERT_OK(kdb_exec_sql(db, "SELECT v FROM a INTERSECT SELECT v FROM a WHERE v = 999", &rows, NULL));
    ASSERT(rows && rows->count == 0u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* column-count mismatch is rejected */
    ASSERT_ERR(sql(db, "SELECT v FROM a INTERSECT SELECT v, v FROM b"));

    /* mixing different set operators, or ALL and non-ALL, in one chain is rejected */
    ASSERT_ERR(sql(db, "SELECT v FROM a UNION SELECT v FROM b INTERSECT SELECT v FROM a"));
    ASSERT_ERR(sql(db, "SELECT v FROM a INTERSECT SELECT v FROM b INTERSECT ALL SELECT v FROM a"));

    teardown(db);
}

static void test_subqueries(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE employees (name TEXT, dept TEXT, salary FLOAT)"));
    ASSERT_OK(sql(db, "CREATE TABLE managers (dept TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('alice', 'eng', 100.0)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('bob', 'sales', 80.0)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('carol', 'eng', 120.0)"));
    ASSERT_OK(sql(db, "INSERT INTO managers (dept) VALUES ('eng')"));

    KdbRows *rows = NULL;

    /* scalar subquery */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM employees WHERE salary = (SELECT MAX(salary) FROM employees)", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *n0 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_STR(n0, "carol");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* IN subquery */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM employees WHERE dept IN (SELECT dept FROM managers) ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* scalar subquery returning more than one row is rejected */
    ASSERT_ERR(sql(db, "SELECT name FROM employees WHERE dept = (SELECT dept FROM employees)"));

    /* IN subquery with more than one column is rejected */
    ASSERT_ERR(sql(db, "SELECT name FROM employees WHERE dept IN (SELECT dept, salary FROM employees)"));

    /* subqueries work in UPDATE/DELETE WHERE too, since they share sql__parse_where */
    size_t updated = 0;
    ASSERT_OK(kdb_exec_sql(db, "UPDATE employees SET salary = 999 WHERE dept IN (SELECT dept FROM managers)", NULL, &updated));
    ASSERT_EQ(updated, 2u);

    size_t deleted = 0;
    ASSERT_OK(kdb_exec_sql(db, "DELETE FROM employees WHERE dept IN (SELECT dept FROM managers)", NULL, &deleted));
    ASSERT_EQ(deleted, 2u);

    teardown(db);
}

static void test_join(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE users (name TEXT)"));
    ASSERT_OK(sql(db, "CREATE TABLE orders (user_id INT, item TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('alice')"));  /* id 1 */
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('bob')"));    /* id 2, no orders */
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (1, 'widget')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (1, 'gadget')"));

    KdbRows *rows = NULL;

    /* INNER JOIN */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item FROM users AS u JOIN orders AS o ON u.id = o.user_id ORDER BY o.item ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *name = NULL, *item = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "u.name", &name));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "o.item", &item));
        ASSERT_STR(name, "alice");
        ASSERT_STR(item, "gadget");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* LEFT JOIN: unmatched left row (bob) still appears, right side NULL */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item FROM users AS u LEFT JOIN orders AS o ON u.id = o.user_id ORDER BY u.name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "u.name", &name));
        ASSERT_STR(name, "bob");
        const KdbField *item_f = kdb_row_get(&rows->rows[2], "o.item");
        ASSERT(item_f && item_f->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* WHERE applies after the join, over qualified columns */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name FROM users AS u JOIN orders AS o ON u.id = o.user_id WHERE o.item = 'gadget'",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* SELECT * exposes every qualified column from both sides, including
     * the id/created_at/updated_at pseudo-columns */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT * FROM users AS u JOIN orders AS o ON u.id = o.user_id LIMIT 1", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        ASSERT_EQ(rows->rows[0].field_count, 9u); /* u.name + u.{id,created_at,updated_at} + o.{user_id,item} + o.{id,created_at,updated_at} */
        ASSERT(kdb_row_get(&rows->rows[0], "u.id") != NULL);
        ASSERT(kdb_row_get(&rows->rows[0], "o.id") != NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* parenthesized WHERE applies fine after a JOIN too */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name FROM users AS u JOIN orders AS o ON u.id = o.user_id "
        "WHERE (o.item = 'gadget' OR o.item = 'nope') AND u.name = 'alice'",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* GROUP BY/aggregates work fine after a JOIN -- group/aggregate on
     * qualified columns same as any other post-JOIN reference. alice has
     * 2 orders, bob has none (and an INNER JOIN drops him entirely). */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, COUNT(*) AS n FROM users AS u JOIN orders AS o ON u.id = o.user_id GROUP BY u.name",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL; int64_t n = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "u.name", &name));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n));
        ASSERT_STR(name, "alice");
        ASSERT_EQ(n, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* HAVING on top of a JOIN + GROUP BY */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, COUNT(*) AS n FROM users AS u JOIN orders AS o ON u.id = o.user_id "
        "GROUP BY u.name HAVING n >= 2",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* duplicate alias on both sides rejected */
    ASSERT_ERR(sql(db, "SELECT * FROM users AS u JOIN orders AS u ON u.id = u.user_id"));

    teardown(db);
}

static void test_outer_joins(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE users (name TEXT)"));
    ASSERT_OK(sql(db, "CREATE TABLE orders (user_id INT, item TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('alice')"));  /* id 1, has an order */
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('bob')"));    /* id 2, no orders */
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (1, 'widget')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (99, 'orphan')")); /* no matching user */

    KdbRows *rows = NULL;

    /* RIGHT JOIN: keeps every order, padding NULL for unmatched users */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item FROM users u RIGHT JOIN orders o ON u.id = o.user_id ORDER BY o.item ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const KdbField *name_f = kdb_row_get(&rows->rows[0], "u.name");
        ASSERT(name_f && name_f->type == KDB_TYPE_NULL);
        const char *item = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "o.item", &item));
        ASSERT_STR(item, "orphan");
        const char *name1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "u.name", &name1));
        ASSERT_STR(name1, "alice");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* FULL [OUTER] JOIN: both unmatched sides appear */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item FROM users u FULL JOIN orders o ON u.id = o.user_id",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item FROM users u FULL OUTER JOIN orders o ON u.id = o.user_id",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* CROSS JOIN: full cartesian product, no ON */
    ASSERT_OK(kdb_exec_sql(db, "SELECT u.name, o.item FROM users u CROSS JOIN orders o", &rows, NULL));
    ASSERT(rows && rows->count == 4u); /* 2 users x 2 orders */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* CROSS JOIN rejects an ON clause */
    ASSERT_ERR(sql(db, "SELECT u.name FROM users u CROSS JOIN orders o ON u.id = o.user_id"));

    /* RIGHT JOIN against a table with zero rows on the left still pads
     * correctly -- the left side's qualified NULL shape has to come from
     * schema, not from a live row, since there isn't one */
    ASSERT_OK(sql(db, "CREATE TABLE empty_t (x INT)"));
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT e.x, o.item FROM empty_t e RIGHT JOIN orders o ON e.x = o.user_id",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const KdbField *x_f = kdb_row_get(&rows->rows[0], "e.x");
        ASSERT(x_f && x_f->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* 3-table chain mixing LEFT then RIGHT: the RIGHT step pads NULLs for
     * the *entire* accumulated (users LEFT JOIN orders) side */
    ASSERT_OK(sql(db, "CREATE TABLE reviews (item TEXT, stars INT)"));
    ASSERT_OK(sql(db, "INSERT INTO reviews (item, stars) VALUES ('widget', 5)"));
    ASSERT_OK(sql(db, "INSERT INTO reviews (item, stars) VALUES ('gizmo', 3)"));
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item, r.stars FROM users u "
        "LEFT JOIN orders o ON u.id = o.user_id "
        "RIGHT JOIN reviews r ON o.item = r.item",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* WHERE still applies after a RIGHT/FULL join, over the combined rows */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item FROM users u RIGHT JOIN orders o ON u.id = o.user_id WHERE o.item = 'orphan'",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_theta_join(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE emp (name TEXT, salary INT, active BOOL)"));
    ASSERT_OK(sql(db, "CREATE TABLE band (label TEXT, lo INT, hi INT)"));
    ASSERT_OK(sql(db, "INSERT INTO emp (name, salary, active) VALUES ('alice', 50, true)"));
    ASSERT_OK(sql(db, "INSERT INTO emp (name, salary, active) VALUES ('bob', 120, false)"));
    ASSERT_OK(sql(db, "INSERT INTO emp (name, salary, active) VALUES ('carol', 80, true)"));
    ASSERT_OK(sql(db, "INSERT INTO band (label, lo, hi) VALUES ('low', 0, 60)"));
    ASSERT_OK(sql(db, "INSERT INTO band (label, lo, hi) VALUES ('mid', 60, 100)"));
    ASSERT_OK(sql(db, "INSERT INTO band (label, lo, hi) VALUES ('high', 100, 200)"));

    KdbRows *rows = NULL;

    /* range-band theta join: salary >= lo AND salary < hi */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT e.name, b.label FROM emp e JOIN band b ON e.salary >= b.lo AND e.salary < b.hi ORDER BY e.name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *l0 = NULL, *l1 = NULL, *l2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "b.label", &l0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "b.label", &l1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "b.label", &l2));
        ASSERT_STR(l0, "low");   /* alice */
        ASSERT_STR(l1, "high");  /* bob */
        ASSERT_STR(l2, "mid");   /* carol */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* boolean literal comparison in ON */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT e.name FROM emp e JOIN band b ON e.active = true AND e.salary >= b.lo AND e.salary < b.hi",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u); /* alice, carol -- bob is inactive */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* != operator against a string literal */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT e.name FROM emp e JOIN band b ON b.label != 'mid' AND e.salary >= b.lo AND e.salary < b.hi",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u); /* alice(low), bob(high) -- carol's mid band excluded */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* LEFT JOIN with a theta ON that never matches -- every row still
     * appears once, padded */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT e.name, b.label FROM emp e LEFT JOIN band b ON b.lo > 1000",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const KdbField *label_f = kdb_row_get(&rows->rows[0], "b.label");
        ASSERT(label_f && label_f->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* equi-join still works unchanged */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT e.name FROM emp e JOIN band b ON b.label = 'low' AND e.salary >= b.lo AND e.salary < b.hi",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* an unsupported operator in ON (LIKE) errors cleanly */
    ASSERT_ERR(sql(db, "SELECT * FROM emp e JOIN band b ON e.salary LIKE b.lo"));

    teardown(db);
}

static void test_join_chain(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE users (name TEXT)"));
    ASSERT_OK(sql(db, "CREATE TABLE orders (user_id INT, item TEXT)"));
    ASSERT_OK(sql(db, "CREATE TABLE reviews (order_item TEXT, stars INT)"));

    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('alice')"));  /* id 1 */
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('bob')"));    /* id 2 */
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (1, 'widget')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (2, 'gizmo')"));
    ASSERT_OK(sql(db, "INSERT INTO reviews (order_item, stars) VALUES ('widget', 5)"));
    /* gizmo has no review */

    KdbRows *rows = NULL;

    /* 3-way INNER chain */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, o.item, rv.stars FROM users AS u "
        "JOIN orders AS o ON u.id = o.user_id "
        "JOIN reviews AS rv ON o.item = rv.order_item "
        "ORDER BY u.name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        int64_t stars = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "u.name", &name));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "rv.stars", &stars));
        ASSERT_STR(name, "alice");
        ASSERT_EQ(stars, 5);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* LEFT JOIN mid-chain: bob's gizmo has no review, so rv.stars is NULL
     * but the row still appears */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, rv.stars FROM users AS u "
        "JOIN orders AS o ON u.id = o.user_id "
        "LEFT JOIN reviews AS rv ON o.item = rv.order_item "
        "ORDER BY u.name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "u.name", &n1));
        ASSERT_STR(n1, "bob");
        const KdbField *sf = kdb_row_get(&rows->rows[1], "rv.stars");
        ASSERT(sf && sf->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* duplicate alias anywhere in the chain (not just adjacent) is rejected */
    ASSERT_ERR(sql(db,
        "SELECT * FROM users AS u JOIN orders AS o ON u.id = o.user_id "
        "JOIN reviews AS u ON o.item = u.order_item"));

    teardown(db);
}

static void test_bare_alias(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE users (name TEXT)"));
    ASSERT_OK(sql(db, "CREATE TABLE orders (user_id INT, item TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('alice')"));  /* id 1 */
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('bob')"));    /* id 2 */
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (1, 'widget')"));

    KdbRows *rows = NULL;

    /* FROM t alias (no AS) works the same as FROM t AS alias */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name FROM users u JOIN orders o ON u.id = o.user_id", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a bare alias that happens to collide with a real clause keyword is
     * NOT swallowed as an alias -- "FROM users WHERE ..." must still parse
     * WHERE as the WHERE clause */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM users WHERE name = 'alice'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_exists(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE users (name TEXT)"));
    ASSERT_OK(sql(db, "CREATE TABLE orders (user_id INT, item TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('alice')"));  /* id 1, has an order */
    ASSERT_OK(sql(db, "INSERT INTO users (name) VALUES ('bob')"));    /* id 2, no orders */
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (1, 'widget')"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM users u WHERE EXISTS (SELECT * FROM orders o WHERE o.user_id = u.id)",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "alice");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM users u WHERE NOT EXISTS (SELECT * FROM orders o WHERE o.user_id = u.id)",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* correlates against the default (bare, no AS) outer alias too */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM users WHERE EXISTS (SELECT * FROM orders o WHERE o.user_id = users.id)",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* EXISTS composes with AND like any other condition */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM users u WHERE EXISTS (SELECT * FROM orders o WHERE o.user_id = u.id) AND name = 'nobody'",
        &rows, NULL));
    ASSERT(rows && rows->count == 0u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* UPDATE/DELETE with EXISTS in WHERE (the parens-style id__in fallback) */
    size_t affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "UPDATE users SET name = 'has_orders' WHERE EXISTS (SELECT * FROM orders o WHERE o.user_id = users.id)",
        NULL, &affected));
    ASSERT_EQ(affected, 1u);

    affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "DELETE FROM users WHERE NOT EXISTS (SELECT * FROM orders o WHERE o.user_id = users.id)",
        NULL, &affected));
    ASSERT_EQ(affected, 1u);
    ASSERT_EQ(kdb_count(db, "users", NULL), 1);

    /* EXISTS isn't supported in HAVING */
    ASSERT_ERR(sql(db,
        "SELECT name, COUNT(*) AS cnt FROM users u GROUP BY name "
        "HAVING EXISTS (SELECT * FROM orders o WHERE o.user_id = u.id)"));

    /* malformed EXISTS -- unterminated subquery -- errors cleanly */
    ASSERT_ERR(sql(db, "SELECT name FROM users u WHERE EXISTS (SELECT * FROM orders o WHERE o.user_id = u.id"));

    teardown(db);
}

static void test_correlated_subqueries(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE employees (name TEXT, dept TEXT, salary INT)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('alice', 'eng', 100)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('bob', 'eng', 200)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('carol', 'sales', 50)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('dave', 'sales', 80)"));
    ASSERT_OK(sql(db, "CREATE TABLE orders (emp_name TEXT, item TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO orders (emp_name, item) VALUES ('alice', 'pen')"));

    KdbRows *rows = NULL;

    /* correlated scalar subquery: top earner per department */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM employees e WHERE salary = (SELECT MAX(salary) FROM employees e2 WHERE e2.dept = e.dept) ORDER BY name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL, *n1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_STR(n0, "bob");
        ASSERT_STR(n1, "dave");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* correlated scalar subquery with a non-equality operator */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM employees e WHERE salary > (SELECT AVG(salary) FROM employees e2 WHERE e2.dept = e.dept) ORDER BY name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL, *n1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_STR(n0, "bob");
        ASSERT_STR(n1, "dave");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* correlated scalar subquery correlating against the bare (no-AS) outer alias */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM employees WHERE salary = (SELECT MAX(salary) FROM employees e2 WHERE e2.dept = employees.dept) ORDER BY name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* correlated scalar subquery composes with AND like any other condition */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM employees e WHERE salary = (SELECT MAX(salary) FROM employees e2 WHERE e2.dept = e.dept) AND dept = 'sales'",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "dave");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* correlated IN subquery */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name FROM employees e WHERE name IN (SELECT emp_name FROM orders o WHERE o.emp_name = e.name)",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "alice");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* non-correlated scalar/IN subqueries still work (the pre-existing,
     * run-once fast path -- a correlated subquery's raw text just happens
     * not to reference the outer alias here) */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM employees WHERE salary = (SELECT MAX(salary) FROM employees)", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM employees WHERE name IN (SELECT emp_name FROM orders)", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* UPDATE/DELETE with a correlated scalar subquery in WHERE */
    size_t affected = 0;
    ASSERT_OK(kdb_exec_sql(db,
        "UPDATE employees SET dept = 'lead' WHERE salary = (SELECT MAX(salary) FROM employees e2 WHERE e2.dept = employees.dept)",
        NULL, &affected));
    ASSERT_EQ(affected, 2u);

    teardown(db);
}

static void test_case_when(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE people (name TEXT, age INT)"));
    ASSERT_OK(sql(db, "INSERT INTO people (name, age) VALUES ('alice', 10)"));
    ASSERT_OK(sql(db, "INSERT INTO people (name, age) VALUES ('bob', 25)"));
    ASSERT_OK(sql(db, "INSERT INTO people (name, age) VALUES ('carol', 70)"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name, CASE WHEN age < 18 THEN 'minor' WHEN age < 65 THEN 'adult' ELSE 'senior' END AS category "
        "FROM people ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *c0 = NULL, *c1 = NULL, *c2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "category", &c0)); /* alice */
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "category", &c1)); /* bob */
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "category", &c2)); /* carol */
        ASSERT_STR(c0, "minor");
        ASSERT_STR(c1, "adult");
        ASSERT_STR(c2, "senior");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* no ELSE, no branch matches -> NULL, row still appears */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name, CASE WHEN age > 100 THEN 'ancient' END AS category FROM people WHERE name = 'alice'",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const KdbField *cf = kdb_row_get(&rows->rows[0], "category");
        ASSERT(cf && cf->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* CASE works fine after a JOIN too, referencing a qualified column */
    ASSERT_OK(sql(db, "CREATE TABLE depts (person TEXT, dept TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO depts (person, dept) VALUES ('alice', 'eng')"));
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT p.name, CASE WHEN p.age < 18 THEN 'minor' ELSE 'adult' END AS cat "
        "FROM people AS p JOIN depts AS d ON p.name = d.person", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *cat = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "cat", &cat));
        ASSERT_STR(cat, "minor");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* AND within one WHEN */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name, CASE WHEN age >= 18 AND age < 65 THEN 'adult' ELSE 'other' END AS category "
        "FROM people ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *c0 = NULL, *c1 = NULL, *c2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "category", &c0)); /* alice, 10 */
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "category", &c1)); /* bob, 25 */
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "category", &c2)); /* carol, 70 */
        ASSERT_STR(c0, "other");
        ASSERT_STR(c1, "adult");
        ASSERT_STR(c2, "other");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* OR within one WHEN */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name, CASE WHEN age < 18 OR age >= 65 THEN 'edge' ELSE 'middle' END AS category "
        "FROM people ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *c0 = NULL, *c1 = NULL, *c2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "category", &c0)); /* alice, 10 */
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "category", &c1)); /* bob, 25 */
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "category", &c2)); /* carol, 70 */
        ASSERT_STR(c0, "edge");
        ASSERT_STR(c1, "middle");
        ASSERT_STR(c2, "edge");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* mixing AND/OR in one WHEN -- AND binds tighter, same as WHERE */
    ASSERT_OK(sql(db, "INSERT INTO depts (person, dept) VALUES ('bob', 'sales')"));
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT p.name, CASE WHEN d.dept = 'eng' AND p.age > 60 OR d.dept = 'sales' THEN 'match' ELSE 'no' END AS c "
        "FROM people AS p JOIN depts AS d ON p.name = d.person ORDER BY p.name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *c0 = NULL, *c1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "c", &c0)); /* alice/eng, age 10 -> no */
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "c", &c1)); /* bob/sales -> match */
        ASSERT_STR(c0, "no");
        ASSERT_STR(c1, "match");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* too many sub-conditions in one WHEN is rejected, not silently truncated */
    ASSERT_ERR(sql(db, "SELECT CASE WHEN age > 1 AND age > 2 AND age > 3 AND age > 4 THEN 'x' END FROM people"));

    /* CASE combined with GROUP BY/aggregates is rejected */
    ASSERT_ERR(sql(db, "SELECT CASE WHEN age < 18 THEN 'x' END, COUNT(*) FROM people GROUP BY age"));

    teardown(db);
}

static void test_distinct(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, grp INT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, grp) VALUES ('a', 1)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, grp) VALUES ('a', 1)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, grp) VALUES ('b', 2)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, grp) VALUES ('a', 1)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, grp) VALUES ('c', 3)"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db, "SELECT DISTINCT name FROM t ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *n0 = NULL, *n1 = NULL, *n2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "name", &n2));
        ASSERT_STR(n0, "a");
        ASSERT_STR(n1, "b");
        ASSERT_STR(n2, "c");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* SELECT DISTINCT * : id/created_at/updated_at live outside row->fields
     * (they're KdbRow struct members, not projected columns), so '*' here
     * dedupes on (name, grp) same as a plain SELECT * always only exposes
     * the user-defined columns -- 3 unique combos, not 5 rows. */
    ASSERT_OK(kdb_exec_sql(db, "SELECT DISTINCT * FROM t", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* dedupe on the full projected (name, grp) pair */
    ASSERT_OK(kdb_exec_sql(db, "SELECT DISTINCT name, grp FROM t", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* DISTINCT + ORDER BY + LIMIT applies limit to the deduped set */
    ASSERT_OK(kdb_exec_sql(db, "SELECT DISTINCT name FROM t ORDER BY name DESC LIMIT 2", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_STR(n0, "c");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_alter_table(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('Alice')"));

    ASSERT_OK(sql(db, "ALTER TABLE t ADD COLUMN age INT INDEX"));
    ASSERT_OK(sql(db, "UPDATE t SET age = 30 WHERE name = 'Alice'"));

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE age > 20", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* both "ADD COLUMN x" and "ADD x" spellings accepted */
    ASSERT_OK(sql(db, "ALTER TABLE t ADD score FLOAT"));
    ASSERT_OK(sql(db, "UPDATE t SET score = 9.5 WHERE name = 'Alice'"));

    /* duplicate column: real error, not "Table already exists" */
    ASSERT_ERR(sql(db, "ALTER TABLE t ADD age INT"));

    /* reserved names rejected */
    ASSERT_ERR(sql(db, "ALTER TABLE t ADD id INT"));
    ASSERT_ERR(sql(db, "ALTER TABLE t ADD created_at INT"));

    /* nonexistent table */
    ASSERT_ERR(sql(db, "ALTER TABLE nope ADD x INT"));

    ASSERT_OK(sql(db, "ALTER TABLE t DROP COLUMN age"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        ASSERT(kdb_row_get(&rows->rows[0], "age") == NULL);
        ASSERT(kdb_row_get(&rows->rows[0], "score") != NULL);
    }
    if (rows) kdb_rows_free(rows);

    teardown(db);
}

static void test_unique_not_null_constraints(void) {
    KumDB *db;
    setup(&db);

    ASSERT_OK(sql(db, "CREATE TABLE users (email TEXT UNIQUE, name TEXT NOT NULL, age INT)"));
    ASSERT_OK(sql(db, "INSERT INTO users (email, name, age) VALUES ('a@x.com', 'alice', 30)"));

    /* UNIQUE rejects a duplicate */
    ASSERT_ERR(sql(db, "INSERT INTO users (email, name, age) VALUES ('a@x.com', 'bob', 40)"));

    /* NOT NULL rejects a missing/NULL value */
    ASSERT_ERR(sql(db, "INSERT INTO users (email, name, age) VALUES ('b@x.com', NULL, 40)"));

    /* NULLs never conflict with each other for UNIQUE */
    ASSERT_OK(sql(db, "INSERT INTO users (email, name, age) VALUES (NULL, 'carol', 22)"));
    ASSERT_OK(sql(db, "INSERT INTO users (email, name, age) VALUES (NULL, 'dave', 25)"));

    /* UPDATE also enforces it */
    ASSERT_ERR(sql(db, "UPDATE users SET email = 'a@x.com' WHERE name = 'carol'"));
    ASSERT_OK(sql(db, "UPDATE users SET age = 99 WHERE name = 'carol'")); /* untouched column is fine */

    /* two rows in the same UPDATE both landing on the same new value is
     * rejected too, and nothing gets written */
    ASSERT_ERR(sql(db, "UPDATE users SET email = 'dup@x.com' WHERE name = 'dave' OR name = 'carol'"));
    {
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM users WHERE email = 'dup@x.com'", &rows, NULL));
        ASSERT(rows && rows->count == 0u);
        if (rows) kdb_rows_free(rows);
    }

    /* PRIMARY KEY implies both UNIQUE and NOT NULL */
    ASSERT_OK(sql(db, "CREATE TABLE accounts (acct_id INT PRIMARY KEY, balance FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO accounts (acct_id, balance) VALUES (1, 100.0)"));
    ASSERT_ERR(sql(db, "INSERT INTO accounts (acct_id, balance) VALUES (1, 200.0)"));
    ASSERT_ERR(sql(db, "INSERT INTO accounts (acct_id, balance) VALUES (NULL, 300.0)"));

    /* a multi-row VALUES insert catches an intra-batch duplicate; the row
     * before it in the same statement stays committed (no implicit
     * per-statement rollback, same convention as everywhere else here) */
    ASSERT_ERR(sql(db, "INSERT INTO accounts (acct_id, balance) VALUES (2, 1.0), (2, 2.0)"));
    {
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM accounts WHERE acct_id = 2", &rows, NULL));
        ASSERT(rows && rows->count == 1u);
        if (rows) kdb_rows_free(rows);
    }

    /* ALTER TABLE ... ALTER COLUMN SET/DROP UNIQUE */
    ASSERT_OK(sql(db, "CREATE TABLE tags (name TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO tags (name) VALUES ('x')"));
    ASSERT_OK(sql(db, "INSERT INTO tags (name) VALUES ('x')")); /* fine before UNIQUE is set */
    ASSERT_OK(sql(db, "ALTER TABLE tags ALTER COLUMN name SET UNIQUE"));
    ASSERT_ERR(sql(db, "INSERT INTO tags (name) VALUES ('x')")); /* rejected now */
    ASSERT_OK(sql(db, "INSERT INTO tags (name) VALUES ('y')"));
    ASSERT_OK(sql(db, "ALTER TABLE tags ALTER COLUMN name DROP UNIQUE"));
    ASSERT_OK(sql(db, "INSERT INTO tags (name) VALUES ('y')")); /* fine again */

    /* ON CONFLICT (upsert) still works alongside real UNIQUE enforcement */
    ASSERT_OK(sql(db, "INSERT INTO accounts (acct_id, balance) VALUES (1, 999.0) ON CONFLICT (acct_id) DO NOTHING"));
    {
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_exec_sql(db, "SELECT balance FROM accounts WHERE acct_id = 1", &rows, NULL));
        ASSERT(rows && rows->count == 1u);
        if (rows && rows->count == 1) {
            double v = 0;
            ASSERT_OK(kdb_row_get_float(&rows->rows[0], "balance", &v));
            ASSERT(v > 99.9 && v < 100.1); /* unchanged */
        }
        if (rows) kdb_rows_free(rows);
    }

    /* ADD COLUMN ... UNIQUE on an existing table */
    ASSERT_OK(sql(db, "ALTER TABLE accounts ADD COLUMN nickname TEXT UNIQUE"));
    ASSERT_OK(sql(db, "INSERT INTO accounts (acct_id, balance, nickname) VALUES (3, 1.0, 'foo')"));
    ASSERT_ERR(sql(db, "INSERT INTO accounts (acct_id, balance, nickname) VALUES (4, 1.0, 'foo')"));

    teardown(db);
}

static void test_create_drop_index(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, dept TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, dept) VALUES ('alice', 'eng')"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, dept) VALUES ('bob', 'sales')"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, dept) VALUES ('carol', 'eng')"));

    KdbRows *rows = NULL;

    /* CREATE INDEX on an existing column with data already present --
     * rebuilds from the existing rows, not just future ones */
    ASSERT_OK(sql(db, "CREATE INDEX ON t (dept)"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE dept = 'eng' ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL, *n1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_STR(n0, "alice");
        ASSERT_STR(n1, "carol");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* indexing an already-indexed column is rejected */
    ASSERT_ERR(sql(db, "CREATE INDEX ON t (dept)"));

    /* an index name is accepted and ignored -- KumDB's indexes aren't named */
    ASSERT_OK(sql(db, "CREATE INDEX idx_name ON t (name)"));

    /* a multi-column CREATE INDEX creates independent single-column
     * indexes, not one combined-key composite index */
    ASSERT_OK(sql(db, "CREATE TABLE t2 (a TEXT, b TEXT)"));
    ASSERT_OK(sql(db, "CREATE INDEX ON t2 (a, b)"));
    ASSERT_OK(sql(db, "INSERT INTO t2 (a, b) VALUES ('x', 'y')"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT a FROM t2 WHERE a = 'x'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }
    ASSERT_OK(kdb_exec_sql(db, "SELECT b FROM t2 WHERE b = 'y'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* nonexistent column/table both error cleanly */
    ASSERT_ERR(sql(db, "CREATE INDEX ON t (nonexistent)"));
    ASSERT_ERR(sql(db, "CREATE INDEX ON nonexistent_table (x)"));

    /* DROP INDEX removes it; filtering still works correctly (unindexed
     * full scan), just without the index */
    ASSERT_OK(sql(db, "DROP INDEX ON t (dept)"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE dept = 'eng' ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* dropping an index that isn't there is rejected */
    ASSERT_ERR(sql(db, "DROP INDEX ON t (dept)"));

    teardown(db);
}

static void test_alter_table_rename(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, dept TEXT INDEX)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, dept) VALUES ('alice', 'eng')"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, dept) VALUES ('bob', 'sales')"));

    KdbRows *rows = NULL;

    /* RENAME COLUMN -- data and the index both follow the new name */
    ASSERT_OK(sql(db, "ALTER TABLE t RENAME COLUMN dept TO department"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE department = 'eng'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "alice");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* renaming to an already-existing column name is rejected */
    ASSERT_ERR(sql(db, "ALTER TABLE t RENAME COLUMN department TO name"));

    /* ALTER COLUMN SET/DROP NOT NULL -- metadata only, always succeeds */
    ASSERT_OK(sql(db, "ALTER TABLE t ALTER COLUMN name SET NOT NULL"));
    ASSERT_OK(sql(db, "ALTER TABLE t ALTER COLUMN name DROP NOT NULL"));

    /* changing a column's type isn't supported */
    ASSERT_ERR(sql(db, "ALTER TABLE t ALTER COLUMN name TYPE INT"));

    /* RENAME TO -- the table itself, data and index intact */
    ASSERT_OK(sql(db, "ALTER TABLE t RENAME TO people"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM people WHERE department = 'sales'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }
    ASSERT_ERR(sql(db, "SELECT * FROM t")); /* old name is gone */

    /* RENAME without TO (MySQL-style) is also accepted */
    ASSERT_OK(sql(db, "ALTER TABLE people RENAME humans"));
    ASSERT_EQ(kdb_count(db, "humans", NULL), 2);

    /* renaming to an already-existing table name is rejected */
    ASSERT_OK(sql(db, "CREATE TABLE other (x INT)"));
    ASSERT_ERR(sql(db, "ALTER TABLE humans RENAME TO other"));

    /* the index (now on 'department') still works for rows inserted
     * after the rename, not just the ones that existed before it */
    ASSERT_OK(sql(db, "INSERT INTO humans (name, department) VALUES ('carol', 'eng')"));
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM humans WHERE department = 'eng' ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_nested_values_through_sql(void) {
    /* no SQL literal syntax for arrays/objects -- build via the C API,
       then confirm SQL's SELECT/projection/GROUP BY-MIN paths carry them
       through correctly instead of silently zeroing them out. */
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT)"));

    KdbField addr1[] = { kdb_field_string("city", "NYC"), kdb_field_end() };
    KdbField f1[] = { kdb_field_string("name", "Alice"), kdb_field_object("address", addr1), kdb_field_end() };
    ASSERT_OK(kdb_add(db, "t", f1));

    KdbField addr2[] = { kdb_field_string("city", "LA"), kdb_field_end() };
    KdbField f2[] = { kdb_field_string("name", "Bob"), kdb_field_object("address", addr2), kdb_field_end() };
    ASSERT_OK(kdb_add(db, "t", f2));

    KdbRows *rows = NULL;

    /* SELECT * carries the object through untouched */
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t WHERE name = 'Alice'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const KdbField *addr = kdb_row_get(&rows->rows[0], "address");
        ASSERT(addr != NULL && addr->type == KDB_TYPE_OBJECT);
        if (addr) {
            ASSERT(addr->v.as_object != NULL);
            ASSERT_STR(addr->v.as_object[0].name, "city");
            ASSERT_STR(addr->v.as_object[0].v.as_string, "NYC");
        }
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* explicit projection must also deep-copy the object, not zero it */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name, address FROM t WHERE name = 'Alice'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const KdbField *addr = kdb_row_get(&rows->rows[0], "address");
        ASSERT(addr != NULL && addr->type == KDB_TYPE_OBJECT && addr->v.as_object != NULL);
        if (addr && addr->v.as_object) ASSERT_STR(addr->v.as_object[0].v.as_string, "NYC");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* GROUP BY MIN() on the object column exercises the aggregate copy path */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name, MIN(address) FROM t GROUP BY name", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) kdb_rows_free(rows);

    /* dot-path WHERE into the nested object -- "address.city" */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM t WHERE address.city = 'NYC'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &name));
        ASSERT_STR(name, "Alice");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_reserved_columns_skipped(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (id INT PRIMARY KEY, created_at INT, name TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('only real column')"));

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) {
        ASSERT_EQ(rows->rows[0].field_count, 1u); /* just "name", id/created_at werent duplicated */
        ASSERT(rows->rows[0].id != 0u);            /* the real engine-managed id still works */
        kdb_rows_free(rows);
    }

    teardown(db);
}

static void test_drop_table(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (n INT)"));
    ASSERT(kdb_table_exists(db, "t"));
    ASSERT_OK(sql(db, "DROP TABLE t"));
    ASSERT(!kdb_table_exists(db, "t"));
    teardown(db);
}

static void test_views(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE employees (name TEXT, dept TEXT, salary FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('alice', 'eng', 120.0)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('bob', 'sales', 80.0)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, dept, salary) VALUES ('carol', 'eng', 150.0)"));

    ASSERT_OK(sql(db, "CREATE VIEW eng_staff AS SELECT name, salary FROM employees WHERE dept = 'eng'"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM eng_staff ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *n0 = NULL, *n1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "name", &n1));
        ASSERT_STR(n0, "alice");
        ASSERT_STR(n1, "carol");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* WHERE applies on top of the view's own filter */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name FROM eng_staff WHERE salary > 130", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* aggregates work over a view */
    ASSERT_OK(kdb_exec_sql(db, "SELECT COUNT(*) AS n, SUM(salary) AS total FROM eng_staff", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t cnt = 0;
        double total = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &cnt));
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "total", &total));
        ASSERT_EQ(cnt, 2);
        ASSERT(total > 269.9 && total < 270.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* DROP VIEW removes it */
    ASSERT_OK(sql(db, "DROP VIEW eng_staff"));
    ASSERT_ERR(sql(db, "SELECT * FROM eng_staff"));
    ASSERT_ERR(sql(db, "DROP VIEW eng_staff")); /* already gone */

    /* a view can be JOINed on either side -- see test_view_cte_join_targets
     * for the detailed coverage */
    ASSERT_OK(sql(db, "CREATE VIEW v2 AS SELECT name FROM employees"));
    ASSERT_OK(sql(db, "CREATE TABLE t2 (x TEXT)"));
    ASSERT_OK(sql(db, "SELECT * FROM v2 JOIN t2 AS t ON v2.name = t.x"));
    ASSERT_OK(sql(db, "SELECT * FROM t2 AS t JOIN v2 ON t.x = v2.name"));

    /* CREATE VIEW validates its query immediately */
    ASSERT_ERR(sql(db, "CREATE VIEW bad_view AS SELECT * FROM nonexistent_table"));

    /* can't shadow an existing table, or redefine an existing view */
    ASSERT_ERR(sql(db, "CREATE VIEW employees AS SELECT name FROM employees"));
    ASSERT_ERR(sql(db, "CREATE VIEW v2 AS SELECT name FROM employees"));

    teardown(db);
}

static void test_view_cte_join_targets(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE users (name TEXT, active BOOL)"));
    ASSERT_OK(sql(db, "CREATE TABLE orders (user_id INT, item TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO users (name, active) VALUES ('alice', true)"));  /* id 1 */
    ASSERT_OK(sql(db, "INSERT INTO users (name, active) VALUES ('bob', false)"));   /* id 2 */
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (1, 'widget')"));
    ASSERT_OK(sql(db, "INSERT INTO orders (user_id, item) VALUES (2, 'gizmo')"));

    ASSERT_OK(sql(db, "CREATE VIEW active_users AS SELECT * FROM users WHERE active = true"));

    KdbRows *rows = NULL;

    /* view as a JOIN target (not table1) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT au.name, o.item FROM active_users au JOIN orders o ON au.id = o.user_id",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "au.name", &name));
        ASSERT_STR(name, "alice");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* view as table1 (the FROM target) with a JOIN on top -- already
     * filtered by the view's own WHERE before the join runs at all */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT au.name, o.item FROM active_users au LEFT JOIN orders o ON au.id = o.user_id",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* RIGHT JOIN with a view as table1 -- left_null_template has to be
     * derived from the view's own returned rows, not a real schema */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT au.name, o.item FROM active_users au RIGHT JOIN orders o ON au.id = o.user_id ORDER BY o.item ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const KdbField *name_f = kdb_row_get(&rows->rows[0], "au.name");
        ASSERT(name_f && name_f->type == KDB_TYPE_NULL); /* gizmo's user (bob) isn't in the view */
        const char *name1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "au.name", &name1));
        ASSERT_STR(name1, "alice");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* CTE as a JOIN target */
    ASSERT_OK(kdb_exec_sql(db,
        "WITH au AS (SELECT * FROM users WHERE active = true) "
        "SELECT au.name, o.item FROM orders o JOIN au ON o.user_id = au.id",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* LEFT/RIGHT/FULL JOIN against a view that returns zero rows on this
     * particular run fails clearly -- there's no schema to derive NULL
     * column names from, only whatever rows it happened to return */
    ASSERT_OK(sql(db, "CREATE VIEW nobody AS SELECT * FROM users WHERE name = 'zzz_nomatch'"));
    ASSERT_ERR(sql(db, "SELECT u.name, n.name FROM users u LEFT JOIN nobody n ON u.name = n.name"));

    /* but INNER/CROSS against that same empty view works fine -- no
     * padding is ever needed for those kinds */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT u.name, n.name FROM users u JOIN nobody n ON u.name = n.name",
        &rows, NULL));
    ASSERT(rows && rows->count == 0u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_ctes(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE sales (region TEXT, amount FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('east', 100.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('east', 50.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('west', 30.0)"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db,
        "WITH big AS (SELECT * FROM sales WHERE amount > 40) SELECT region FROM big ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* CTE over an aggregate */
    ASSERT_OK(kdb_exec_sql(db,
        "WITH totals AS (SELECT region, SUM(amount) AS total FROM sales GROUP BY region) "
        "SELECT region FROM totals WHERE total > 60",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *region = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &region));
        ASSERT_STR(region, "east");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a later CTE can reference an earlier one */
    ASSERT_OK(kdb_exec_sql(db,
        "WITH t1 AS (SELECT * FROM sales WHERE region = 'east'), "
        "t2 AS (SELECT region FROM t1 WHERE amount > 60) "
        "SELECT * FROM t2",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a CTE is scoped to its own statement -- gone afterward */
    ASSERT_ERR(sql(db, "SELECT * FROM big"));

    /* a CTE name colliding with a real table is rejected */
    ASSERT_ERR(sql(db, "WITH sales AS (SELECT * FROM sales) SELECT * FROM sales"));

    /* an error inside a CTE's body still cleans up (no leaked temp view) */
    ASSERT_ERR(sql(db, "WITH bad AS (SELECT * FROM nosuchtable) SELECT * FROM bad"));
    ASSERT_ERR(sql(db, "SELECT * FROM bad"));

    /* forward reference (t2 declared before t1 it depends on) is rejected,
     * not recursive WITH -- only prior CTEs are visible to a later one */
    ASSERT_ERR(sql(db, "WITH t2 AS (SELECT * FROM t1), t1 AS (SELECT * FROM sales) SELECT * FROM t2"));

    /* same statement runs cleanly a second time -- no leftover state from
     * the first run's cleanup */
    ASSERT_OK(kdb_exec_sql(db,
        "WITH big AS (SELECT * FROM sales WHERE amount > 40) SELECT region FROM big ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    teardown(db);
}

static void test_recursive_ctes(void) {
    KumDB *db;
    setup(&db);

    /* a one-row helper table to select a literal seed FROM -- FROM is
     * mandatory for every SELECT here, recursive CTEs included */
    ASSERT_OK(sql(db, "CREATE TABLE dual (x INT)"));
    ASSERT_OK(sql(db, "INSERT INTO dual (x) VALUES (1)"));

    KdbRows *rows = NULL;

    /* growing a string one character at a time -- this engine has no
     * arithmetic expressions (col + 1) or function calls in WHERE, so a
     * numeric counter isn't expressible, but CONCAT()/a plain column
     * comparison is */
    ASSERT_OK(kdb_exec_sql(db,
        "WITH RECURSIVE growing AS ("
        "  SELECT 'a' AS s FROM dual"
        "  UNION ALL"
        "  SELECT CONCAT(s, 'a') AS s FROM growing WHERE s != 'aaaaa'"
        ") SELECT s FROM growing ORDER BY s ASC", &rows, NULL));
    ASSERT(rows && rows->count == 5u);
    if (rows && rows->count == 5) {
        const char *s0 = NULL, *s4 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "s", &s0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[4], "s", &s4));
        ASSERT_STR(s0, "a");
        ASSERT_STR(s4, "aaaaa");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* hierarchy traversal via a self-JOIN on the recursive term -- the
     * canonical recursive CTE use case */
    ASSERT_OK(sql(db, "CREATE TABLE employees (name TEXT, manager TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, manager) VALUES ('ceo', 'none')"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, manager) VALUES ('vp_eng', 'ceo')"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, manager) VALUES ('vp_sales', 'ceo')"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, manager) VALUES ('eng1', 'vp_eng')"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, manager) VALUES ('eng2', 'vp_eng')"));
    ASSERT_OK(sql(db, "INSERT INTO employees (name, manager) VALUES ('sales1', 'vp_sales')"));

    ASSERT_OK(kdb_exec_sql(db,
        "WITH RECURSIVE org AS ("
        "  SELECT name, manager FROM employees WHERE name = 'vp_eng'"
        "  UNION"
        "  SELECT e.name AS name, e.manager AS manager FROM employees AS e JOIN org ON e.manager = org.name"
        ") SELECT name FROM org ORDER BY name ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u); /* vp_eng, eng1, eng2 */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a cycle in the underlying data doesn't hang -- UNION (not ALL)
     * dedupes against the full accumulated history every round, so a
     * graph edge back to an already-visited node stops on its own */
    ASSERT_OK(sql(db, "CREATE TABLE edges (src TEXT, dst TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO edges (src, dst) VALUES ('a', 'b')"));
    ASSERT_OK(sql(db, "INSERT INTO edges (src, dst) VALUES ('b', 'c')"));
    ASSERT_OK(sql(db, "INSERT INTO edges (src, dst) VALUES ('c', 'a')")); /* cycles back */
    ASSERT_OK(kdb_exec_sql(db,
        "WITH RECURSIVE reach AS ("
        "  SELECT 'a' AS node FROM dual"
        "  UNION"
        "  SELECT edges.dst AS node FROM edges JOIN reach ON edges.src = reach.node"
        ") SELECT node FROM reach ORDER BY node ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u); /* a, b, c */
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* the CTE's real backing table doesn't survive past the statement */
    ASSERT_ERR(kdb_exec_sql(db, "SELECT * FROM growing", NULL, NULL));

    /* works inside a SQL transaction too -- the CTE's temp table is
     * created and dropped entirely within this one statement, so it
     * never interacts with the surrounding BEGIN/COMMIT bookkeeping */
    ASSERT_OK(sql(db, "BEGIN"));
    ASSERT_OK(kdb_exec_sql(db,
        "WITH RECURSIVE growing2 AS ("
        "  SELECT 'a' AS s FROM dual"
        "  UNION ALL"
        "  SELECT CONCAT(s, 'a') AS s FROM growing2 WHERE s != 'aaa'"
        ") SELECT s FROM growing2 ORDER BY s ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }
    ASSERT_OK(sql(db, "COMMIT"));

    /* error paths */
    ASSERT_ERR(sql(db, /* base case with no rows -- can't infer a schema */
        "WITH RECURSIVE z AS (SELECT name FROM employees WHERE 1=0 UNION ALL SELECT name FROM z) SELECT * FROM z"));
    ASSERT_ERR(sql(db, /* no UNION at all */
        "WITH RECURSIVE z AS (SELECT 1 AS n FROM dual) SELECT * FROM z"));
    ASSERT_ERR(sql(db, /* CTE name collides with a real table */
        "WITH RECURSIVE employees AS (SELECT 'a' AS s FROM dual UNION ALL SELECT CONCAT(s,'a') AS s FROM employees WHERE s != 'aa') "
        "SELECT * FROM employees"));
    ASSERT_ERR(sql(db, /* recursive term's column doesn't match the base case's (missing AS s) */
        "WITH RECURSIVE mism AS (SELECT 'a' AS s FROM dual UNION ALL SELECT CONCAT(s,'a') FROM mism WHERE s != 'aa') "
        "SELECT * FROM mism"));
    ASSERT_ERR(sql(db, /* never converges -- hits the iteration cap rather than hanging */
        "WITH RECURSIVE inf AS (SELECT 'a' AS s FROM dual UNION ALL SELECT CONCAT(s,'a') AS s FROM inf) SELECT * FROM inf"));

    teardown(db);
}

static void test_derived_tables(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE sales (region TEXT, amount FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('east', 100.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('east', 50.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, amount) VALUES ('west', 30.0)"));

    KdbRows *rows = NULL;

    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region FROM (SELECT * FROM sales WHERE amount > 40) AS big ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* derived table over an aggregate */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region FROM (SELECT region, SUM(amount) AS total FROM sales GROUP BY region) AS totals WHERE total > 60",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *region = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "region", &region));
        ASSERT_STR(region, "east");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* WHERE/ORDER BY/LIMIT apply on top of the derived table like a real
     * table or view */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT * FROM (SELECT * FROM sales) AS s WHERE region = 'east' ORDER BY amount DESC LIMIT 1",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double amount = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "amount", &amount));
        ASSERT(amount > 99.9 && amount < 100.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* bare alias (no AS) works, same as a real table */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT region FROM (SELECT * FROM sales WHERE amount > 40) big ORDER BY region ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a scalar function over a derived table's column */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT UPPER(region) AS r FROM (SELECT * FROM sales) AS s WHERE amount = 100.0",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *r = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "r", &r));
        ASSERT_STR(r, "EAST");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a derived table needs an alias -- there's no name to fall back on */
    ASSERT_ERR(sql(db, "SELECT * FROM (SELECT * FROM sales)"));

    teardown(db);
}

static void test_literal_select_items(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('alice')"));
    ASSERT_OK(sql(db, "INSERT INTO t (name) VALUES ('bob')"));

    KdbRows *rows = NULL;

    /* a bare literal alongside a real column -- same value on every row */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT name, 'x' AS tag, 42 AS n, TRUE AS b, NULL AS z FROM t ORDER BY name ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const char *tag0 = NULL;
        int64_t n0 = 0;
        int b0 = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "tag", &tag0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n0));
        ASSERT_OK(kdb_row_get_bool(&rows->rows[0], "b", &b0));
        ASSERT_STR(tag0, "x");
        ASSERT_EQ(n0, 42);
        ASSERT_EQ(b0, 1);
        const KdbField *z0 = kdb_row_get(&rows->rows[0], "z");
        ASSERT(z0 && z0->type == KDB_TYPE_NULL);
        /* same literals on the second row too */
        const char *tag1 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "tag", &tag1));
        ASSERT_STR(tag1, "x");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* an unaliased literal defaults to "?column?", same as real SQL */
    ASSERT_OK(kdb_exec_sql(db, "SELECT 1 FROM t WHERE name = 'alice'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t v = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "?column?", &v));
        ASSERT_EQ(v, 1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* still needs a FROM -- this engine's one global SELECT rule, not
     * something a bare literal gets to skip */
    ASSERT_ERR(sql(db, "SELECT 1"));

    /* not supported combined with GROUP BY/aggregates, same limit CASE
     * and scalar functions already have */
    ASSERT_ERR(sql(db, "SELECT 1, COUNT(*) FROM t"));
    ASSERT_ERR(sql(db, "SELECT 1, name FROM t GROUP BY name"));

    teardown(db);
}

static void test_arithmetic_expressions(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (price FLOAT, qty INT, name TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (price, qty, name) VALUES (10.0, 3, 'a')"));
    ASSERT_OK(sql(db, "INSERT INTO t (price, qty, name) VALUES (5.0, 0, 'b')"));

    /* negative-number-literal lexing everywhere else is unaffected by the
     * new operator tokens */
    ASSERT_OK(sql(db, "SELECT * FROM t WHERE qty >= -1"));
    ASSERT_OK(sql(db, "INSERT INTO t (price, qty, name) VALUES (-2.5, -1, 'neg')"));

    KdbRows *rows = NULL;

    /* basic multiply, column * column */
    ASSERT_OK(kdb_exec_sql(db, "SELECT price * qty AS total FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double v = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "total", &v));
        ASSERT(v > 29.9 && v < 30.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* * binds tighter than + -- "10 + 3*2" is 16, not "13*2"=26 */
    ASSERT_OK(kdb_exec_sql(db, "SELECT price + qty * 2 AS r FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double v = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "r", &v));
        ASSERT(v > 15.9 && v < 16.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a longer mixed chain: 10*3 - 3 + 1 = 28 */
    ASSERT_OK(kdb_exec_sql(db, "SELECT price * qty - qty + 1 AS r FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double v = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "r", &v));
        ASSERT(v > 27.9 && v < 28.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* modulo */
    ASSERT_OK(kdb_exec_sql(db, "SELECT qty % 2 AS r FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double v = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "r", &v));
        ASSERT(v > 0.9 && v < 1.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* division/modulo by zero is NULL for that row, not a query error */
    ASSERT_OK(kdb_exec_sql(db, "SELECT price / qty AS r FROM t WHERE name = 'b'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const KdbField *f = kdb_row_get(&rows->rows[0], "r");
        ASSERT(f && f->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* unary minus on a term after an operator: 10 + -3 = 7 */
    ASSERT_OK(kdb_exec_sql(db, "SELECT price + -qty AS r FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double v = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "r", &v));
        ASSERT(v > 6.9 && v < 7.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a literal-only expression: 2*3+1 = 7 */
    ASSERT_OK(kdb_exec_sql(db, "SELECT 2 * 3 + 1 AS r FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double v = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "r", &v));
        ASSERT(v > 6.9 && v < 7.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* unaliased expression defaults to "?column?", same as a bare literal */
    ASSERT_OK(kdb_exec_sql(db, "SELECT price * qty FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double v = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "?column?", &v));
        ASSERT(v > 29.9 && v < 30.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a non-numeric (or missing) column makes the expression NULL for
     * that row, not an error */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name * 2 AS r FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const KdbField *f = kdb_row_get(&rows->rows[0], "r");
        ASSERT(f && f->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* an expression alongside a plain column, and after AS */
    ASSERT_OK(kdb_exec_sql(db, "SELECT name, price * qty AS total FROM t WHERE name = 'a'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *n = NULL;
        double v = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &n));
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "total", &v));
        ASSERT_STR(n, "a");
        ASSERT(v > 29.9 && v < 30.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* not combined with GROUP BY/aggregates, same limit CASE/scalar
     * functions/bare literals already have */
    ASSERT_ERR(sql(db, "SELECT price * qty, COUNT(*) FROM t"));
    ASSERT_ERR(sql(db, "SELECT price * qty, name FROM t GROUP BY name"));

    /* not supported in WHERE -- documented scope boundary, only SELECT
     * items so far */
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE price * qty > 10"));

    /* too many terms in one expression is rejected, not silently
     * truncated */
    ASSERT_ERR(sql(db, "SELECT 1+1+1+1+1+1+1 AS r FROM t"));

    teardown(db);
}

static void test_scalar_functions(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (name TEXT, age INT, price FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, age, price) VALUES ('  Alice  ', 17, 19.567)"));
    ASSERT_OK(sql(db, "INSERT INTO t (name, age, price) VALUES ('bob', 70, -3.2)"));

    KdbRows *rows = NULL;

    /* string functions */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT UPPER(name) AS u, LOWER(name) AS l, LENGTH(name) AS n, TRIM(name) AS t FROM t WHERE name = 'bob'",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *u = NULL, *l = NULL, *tr = NULL;
        int64_t n = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "u", &u));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "l", &l));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "t", &tr));
        ASSERT_STR(u, "BOB"); ASSERT_STR(l, "bob"); ASSERT_EQ(n, 3); ASSERT_STR(tr, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT TRIM(name) AS t FROM t WHERE age = 17", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *t = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "t", &t));
        ASSERT_STR(t, "Alice"); /* leading/trailing spaces stripped */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT SUBSTR(name, 1, 2) AS s1, SUBSTR(name, 2) AS s2 FROM t WHERE name = 'bob'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *s1 = NULL, *s2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "s1", &s1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "s2", &s2));
        ASSERT_STR(s1, "bo"); ASSERT_STR(s2, "ob"); /* 1-based start */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT CONCAT(name, '-', age) AS c FROM t WHERE name = 'bob'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *c = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "c", &c));
        ASSERT_STR(c, "bob-70");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* numeric functions */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT ROUND(price, 2) AS r, ABS(price) AS a, CEIL(price) AS c, FLOOR(price) AS f FROM t WHERE name = 'bob'",
        &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        double r = 0, a = 0;
        int64_t c = 0, f = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "r", &r));
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "a", &a));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "c", &c));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "f", &f));
        ASSERT(r > -3.21 && r < -3.19);
        ASSERT(a > 3.19 && a < 3.21);
        ASSERT_EQ(c, -3);  /* CEIL rounds toward +inf */
        ASSERT_EQ(f, -4);  /* FLOOR rounds toward -inf */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT MOD(age, 7) AS m FROM t WHERE name = 'bob'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t m = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "m", &m));
        ASSERT_EQ(m, 0); /* 70 % 7 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* null-handling functions */
    ASSERT_OK(kdb_exec_sql(db, "SELECT NULLIF(age, 70) AS n FROM t ORDER BY age ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        int64_t n0 = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n0));
        ASSERT_EQ(n0, 17); /* not equal to 70 -> passes through */
        const KdbField *n1 = kdb_row_get(&rows->rows[1], "n");
        ASSERT(n1 && n1->type == KDB_TYPE_NULL); /* equal to 70 -> NULL */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    ASSERT_OK(kdb_exec_sql(db, "SELECT COALESCE(name, 'fallback') AS c FROM t WHERE name = 'bob'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *c = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "c", &c));
        ASSERT_STR(c, "bob");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* CAST */
    ASSERT_OK(kdb_exec_sql(db, "SELECT CAST(price AS INT) AS i, CAST(age AS TEXT) AS s FROM t WHERE name = 'bob'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t i = 0;
        const char *s = NULL;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "i", &i));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "s", &s));
        ASSERT_EQ(i, -3); /* truncated toward zero */
        ASSERT_STR(s, "70");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* NOW() -- just confirm it returns a plausible INT epoch, not the
     * exact value (that would make the test time-dependent) */
    ASSERT_OK(kdb_exec_sql(db, "SELECT NOW() AS n FROM t WHERE name = 'bob'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        int64_t n = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n));
        ASSERT(n > 1700000000); /* sometime after 2023 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a type-mismatched argument produces NULL, not an error */
    ASSERT_OK(kdb_exec_sql(db, "SELECT UPPER(age) AS u FROM t WHERE name = 'bob'", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const KdbField *u = kdb_row_get(&rows->rows[0], "u");
        ASSERT(u && u->type == KDB_TYPE_NULL);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* functions after a JOIN, on qualified columns */
    ASSERT_OK(sql(db, "CREATE TABLE addr (person TEXT, city TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO addr (person, city) VALUES ('bob', 'nyc')"));
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT UPPER(a.city) AS c FROM t AS x JOIN addr AS a ON x.name = a.person", &rows, NULL));
    ASSERT(rows && rows->count == 1u);
    if (rows && rows->count == 1) {
        const char *c = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "c", &c));
        ASSERT_STR(c, "NYC");
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* wrong argument count is rejected, not silently ignored */
    ASSERT_ERR(sql(db, "SELECT UPPER(name, age) FROM t"));
    ASSERT_ERR(sql(db, "SELECT MOD(age) FROM t"));

    /* a scalar function can't combine with GROUP BY/aggregates */
    ASSERT_ERR(sql(db, "SELECT UPPER(name), COUNT(*) FROM t GROUP BY name"));

    teardown(db);
}

static void test_window_functions(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE sales (region TEXT, rep TEXT, amount FLOAT)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('east', 'alice', 100.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('east', 'bob', 150.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('east', 'carol', 150.0)")); /* ties bob */
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('west', 'dave', 50.0)"));
    ASSERT_OK(sql(db, "INSERT INTO sales (region, rep, amount) VALUES ('west', 'erin', 200.0)"));

    KdbRows *rows = NULL;

    /* ROW_NUMBER: no ties, strictly 1..n per partition. Top-level ORDER BY
     * is single-column only (a separate, pre-existing limitation), so
     * WHERE narrows to one partition instead of sorting on two columns. */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount DESC) AS rn "
        "FROM sales WHERE region = 'east' ORDER BY rn ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *r0 = NULL, *r1 = NULL, *r2 = NULL;
        int64_t n0 = 0, n1 = 0, n2 = 0;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "rep", &r0)); ASSERT_OK(kdb_row_get_int(&rows->rows[0], "rn", &n0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[1], "rep", &r1)); ASSERT_OK(kdb_row_get_int(&rows->rows[1], "rn", &n1));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "rep", &r2)); ASSERT_OK(kdb_row_get_int(&rows->rows[2], "rn", &n2));
        ASSERT_STR(r0, "bob");   ASSERT_EQ(n0, 1); /* 150, first inserted among the tie */
        ASSERT_STR(r1, "carol"); ASSERT_EQ(n1, 2); /* 150, tied with bob */
        ASSERT_STR(r2, "alice"); ASSERT_EQ(n2, 3); /* 100 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* RANK: ties share a rank, with a gap afterward (1,1,3 not 1,1,2) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, RANK() OVER (PARTITION BY region ORDER BY amount DESC) AS r "
        "FROM sales WHERE region = 'east' ORDER BY r ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        int64_t r0 = 0, r1 = 0, r2 = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "r", &r0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "r", &r1));
        ASSERT_OK(kdb_row_get_int(&rows->rows[2], "r", &r2));
        ASSERT_EQ(r0, 1); ASSERT_EQ(r1, 1); ASSERT_EQ(r2, 3);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* DENSE_RANK: ties share a rank, no gap (1,1,2) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, DENSE_RANK() OVER (PARTITION BY region ORDER BY amount DESC) AS dr "
        "FROM sales WHERE region = 'east' ORDER BY dr ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        int64_t d0 = 0, d1 = 0, d2 = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "dr", &d0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "dr", &d1));
        ASSERT_OK(kdb_row_get_int(&rows->rows[2], "dr", &d2));
        ASSERT_EQ(d0, 1); ASSERT_EQ(d1, 1); ASSERT_EQ(d2, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* windowed SUM/COUNT: whole-partition value, same on every row, rows
     * NOT collapsed (unlike GROUP BY) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, SUM(amount) OVER (PARTITION BY region) AS total, "
        "COUNT(*) OVER (PARTITION BY region) AS n FROM sales WHERE region = 'east' ORDER BY rep ASC",
        &rows, NULL));
    ASSERT(rows && rows->count == 3u); /* still 3 rows, not collapsed to 1 */
    if (rows && rows->count == 3) {
        double t0 = 0, t1 = 0;
        int64_t n0 = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "total", &t0));
        ASSERT_OK(kdb_row_get_float(&rows->rows[1], "total", &t1));
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n0));
        ASSERT(t0 > 399.9 && t0 < 400.1); /* 100+150+150 */
        ASSERT(t1 > 399.9 && t1 < 400.1);
        ASSERT_EQ(n0, 3);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* no PARTITION BY at all -- whole result set is one partition */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT MAX(amount) OVER () AS m FROM sales", &rows, NULL));
    ASSERT(rows && rows->count == 5u);
    if (rows && rows->count == 5) {
        double m = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "m", &m));
        ASSERT(m > 199.9 && m < 200.1);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* a plain column freely coexists with a window function -- no
     * GROUP BY-style "must be in the partition" restriction */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, region, RANK() OVER (PARTITION BY region ORDER BY amount DESC) AS r FROM sales",
        &rows, NULL));
    ASSERT(rows && rows->count == 5u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* window function after a JOIN, on qualified columns */
    ASSERT_OK(sql(db, "CREATE TABLE reps (name TEXT, tier TEXT)"));
    ASSERT_OK(sql(db, "INSERT INTO reps (name, tier) VALUES ('alice', 'gold')"));
    ASSERT_OK(sql(db, "INSERT INTO reps (name, tier) VALUES ('bob', 'gold')"));
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT s.rep, RANK() OVER (PARTITION BY r.tier ORDER BY s.amount DESC) AS rk "
        "FROM sales AS s JOIN reps AS r ON s.rep = r.name", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* ROW_NUMBER()/RANK()/DENSE_RANK() require OVER */
    ASSERT_ERR(sql(db, "SELECT ROW_NUMBER() FROM sales"));
    ASSERT_ERR(sql(db, "SELECT RANK() FROM sales"));

    /* a window function can't combine with GROUP BY/a plain aggregate in
     * the same SELECT */
    ASSERT_ERR(sql(db, "SELECT region, COUNT(*), ROW_NUMBER() OVER (ORDER BY region) FROM sales GROUP BY region"));

    /* LAG/LEAD: neighbor within the partition, in ORDER BY order; NULL
     * (or the given default) past the partition's edge */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, LAG(amount) OVER (PARTITION BY region ORDER BY amount ASC) AS prev, "
        "LEAD(amount) OVER (PARTITION BY region ORDER BY amount ASC) AS next "
        "FROM sales WHERE region = 'west' ORDER BY amount ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        const KdbField *prev0 = kdb_row_get(&rows->rows[0], "prev");
        const KdbField *next1 = kdb_row_get(&rows->rows[1], "next");
        ASSERT(prev0 && prev0->type == KDB_TYPE_NULL); /* first row in the partition -- no predecessor */
        ASSERT(next1 && next1->type == KDB_TYPE_NULL); /* last row -- no successor */
        double next0 = 0, prev1 = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "next", &next0));
        ASSERT_OK(kdb_row_get_float(&rows->rows[1], "prev", &prev1));
        ASSERT(next0 > 199.9 && next0 < 200.1); /* dave(50) -> next is erin(200) */
        ASSERT(prev1 > 49.9 && prev1 < 50.1);   /* erin(200) -> prev is dave(50) */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* LAG with an explicit offset and a default for out-of-range rows */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, LAG(amount, 2, -1) OVER (PARTITION BY region ORDER BY amount ASC) AS lag2 "
        "FROM sales WHERE region = 'east' ORDER BY amount ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        double l0 = 0, l2 = 0;
        ASSERT_OK(kdb_row_get_float(&rows->rows[0], "lag2", &l0));
        ASSERT_OK(kdb_row_get_float(&rows->rows[2], "lag2", &l2));
        ASSERT(l0 > -1.1 && l0 < -0.9);         /* 100 is the first row -- 2 back is out of range, default -1 */
        ASSERT(l2 > 99.9 && l2 < 100.1);        /* 3rd row (150) -- 2 back is the 1st row (100) */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* FIRST_VALUE/LAST_VALUE: same value on every row of the partition,
     * whole-partition frame (no ROWS/RANGE BETWEEN) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, FIRST_VALUE(rep) OVER (PARTITION BY region ORDER BY amount ASC) AS f, "
        "LAST_VALUE(rep) OVER (PARTITION BY region ORDER BY amount ASC) AS l "
        "FROM sales WHERE region = 'east' ORDER BY amount ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        const char *f0 = NULL, *l0 = NULL, *f2 = NULL, *l2 = NULL;
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "f", &f0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[0], "l", &l0));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "f", &f2));
        ASSERT_OK(kdb_row_get_string(&rows->rows[2], "l", &l2));
        ASSERT_STR(f0, "alice"); ASSERT_STR(l0, "carol");
        ASSERT_STR(f2, "alice"); ASSERT_STR(l2, "carol"); /* same on every row */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* NTILE: splits a partition into n roughly-equal buckets in ORDER BY
     * order -- an uneven split gives earlier buckets the extra row(s) */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, NTILE(2) OVER (PARTITION BY region ORDER BY amount ASC) AS bucket "
        "FROM sales WHERE region = 'east' ORDER BY amount ASC", &rows, NULL));
    ASSERT(rows && rows->count == 3u);
    if (rows && rows->count == 3) {
        int64_t b0 = 0, b1 = 0, b2 = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "bucket", &b0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "bucket", &b1));
        ASSERT_OK(kdb_row_get_int(&rows->rows[2], "bucket", &b2));
        ASSERT_EQ(b0, 1); ASSERT_EQ(b1, 1); ASSERT_EQ(b2, 2); /* 3 rows into 2 buckets: 2,1 */
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* NTILE(n) with n bigger than the partition -- each row its own
     * bucket, no error */
    ASSERT_OK(kdb_exec_sql(db,
        "SELECT rep, NTILE(10) OVER (PARTITION BY region ORDER BY amount ASC) AS bucket "
        "FROM sales WHERE region = 'west' ORDER BY amount ASC", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows && rows->count == 2) {
        int64_t b0 = 0, b1 = 0;
        ASSERT_OK(kdb_row_get_int(&rows->rows[0], "bucket", &b0));
        ASSERT_OK(kdb_row_get_int(&rows->rows[1], "bucket", &b1));
        ASSERT_EQ(b0, 1); ASSERT_EQ(b1, 2);
    }
    if (rows) { kdb_rows_free(rows); rows = NULL; }

    /* LAG/LEAD/FIRST_VALUE/LAST_VALUE/NTILE all require OVER */
    ASSERT_ERR(sql(db, "SELECT LAG(amount) FROM sales"));
    ASSERT_ERR(sql(db, "SELECT FIRST_VALUE(amount) FROM sales"));
    ASSERT_ERR(sql(db, "SELECT NTILE(2) FROM sales"));

    /* NTILE needs a positive integer bucket count */
    ASSERT_ERR(sql(db, "SELECT NTILE(0) OVER (ORDER BY amount) FROM sales"));
    ASSERT_ERR(sql(db, "SELECT NTILE(-1) OVER (ORDER BY amount) FROM sales"));

    /* LAG/LEAD's offset must be a non-negative integer literal */
    ASSERT_ERR(sql(db, "SELECT LAG(amount, -1) OVER (ORDER BY amount) FROM sales"));

    /* explicit ROWS/RANGE BETWEEN frame clauses aren't supported */
    ASSERT_ERR(sql(db, "SELECT SUM(amount) OVER (ORDER BY amount ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) FROM sales"));

    teardown(db);
}

static void test_comments(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (n INT) -- trailing line comment"));
    ASSERT_OK(sql(db, "-- leading line comment\nINSERT INTO t (n) VALUES (1)"));
    ASSERT_OK(sql(db, "INSERT /* inline block */ INTO t (n) VALUES (2)"));

    KdbRows *rows = NULL;
    ASSERT_OK(kdb_exec_sql(db, "SELECT * FROM t /* multi\nline\ncomment */ WHERE n > 0", &rows, NULL));
    ASSERT(rows && rows->count == 2u);
    if (rows) kdb_rows_free(rows);

    /* an unterminated block comment doesn't hang or crash -- it just eats
     * to EOF, same as running out of input mid-statement any other way */
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE n /* unterminated"));

    teardown(db);
}

static void test_syntax_errors(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (n INT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (n) VALUES (1)"));

    ASSERT_ERR(sql(db, "SELEKT * FROM t"));
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE n = NULL"));
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE n LIKE 5"));           /* pattern must be a string literal */
    ASSERT_ERR(sql(db, "INSERT INTO t VALUES (1)"));                 /* no column list */
    ASSERT_ERR(sql(db, "INSERT INTO t (n) VALUES (1, 2)"));          /* count mismatch */
    ASSERT_ERR(sql(db, "CREATE TABLE t2 (n WEIRDTYPE)"));
    ASSERT_ERR(sql(db, ""));
    ASSERT_ERR(sql(db, "SELECT * FROM t; SELECT * FROM t"));         /* one statement per call */

    /* confirm none of the above actually mutated anything */
    ASSERT_EQ(kdb_count(db, "t", NULL), 1);

    teardown(db);
}

int main(void) {
    printf("=== test_sql ===\n");

    test_create_insert_select();
    test_multi_row_insert_and_insert_select();
    test_upsert();
    test_returning();
    test_transactions();
    test_projection();
    test_limit_offset();
    test_multi_column_order_by();
    test_update_delete();
    test_where_operators();
    test_regexp_and_ilike();
    test_bound_params();
    test_or();
    test_parenthesized_where();
    test_group_by_and_aggregates();
    test_group_by_extensions();
    test_aggregate_filter();
    test_multi_column_group_by();
    test_having();
    test_union();
    test_intersect_except();
    test_subqueries();
    test_join();
    test_outer_joins();
    test_theta_join();
    test_join_chain();
    test_bare_alias();
    test_exists();
    test_case_when();
    test_distinct();
    test_alter_table();
    test_unique_not_null_constraints();
    test_create_drop_index();
    test_alter_table_rename();
    test_nested_values_through_sql();
    test_reserved_columns_skipped();
    test_drop_table();
    test_views();
    test_view_cte_join_targets();
    test_ctes();
    test_recursive_ctes();
    test_derived_tables();
    test_correlated_subqueries();
    test_literal_select_items();
    test_arithmetic_expressions();
    test_scalar_functions();
    test_window_functions();
    test_comments();
    test_syntax_errors();

    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
