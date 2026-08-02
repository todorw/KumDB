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

    /* a view can't be JOINed (either side) */
    ASSERT_OK(sql(db, "CREATE VIEW v2 AS SELECT name FROM employees"));
    ASSERT_OK(sql(db, "CREATE TABLE t2 (x TEXT)"));
    ASSERT_ERR(sql(db, "SELECT * FROM v2 JOIN t2 AS t ON v2.name = t.x"));
    ASSERT_ERR(sql(db, "SELECT * FROM t2 AS t JOIN v2 ON t.x = v2.name"));

    /* CREATE VIEW validates its query immediately */
    ASSERT_ERR(sql(db, "CREATE VIEW bad_view AS SELECT * FROM nonexistent_table"));

    /* can't shadow an existing table, or redefine an existing view */
    ASSERT_ERR(sql(db, "CREATE VIEW employees AS SELECT name FROM employees"));
    ASSERT_ERR(sql(db, "CREATE VIEW v2 AS SELECT name FROM employees"));

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
    test_projection();
    test_limit_offset();
    test_multi_column_order_by();
    test_update_delete();
    test_where_operators();
    test_or();
    test_parenthesized_where();
    test_group_by_and_aggregates();
    test_multi_column_group_by();
    test_having();
    test_union();
    test_intersect_except();
    test_subqueries();
    test_join();
    test_join_chain();
    test_bare_alias();
    test_exists();
    test_case_when();
    test_distinct();
    test_alter_table();
    test_nested_values_through_sql();
    test_reserved_columns_skipped();
    test_drop_table();
    test_views();
    test_ctes();
    test_derived_tables();
    test_correlated_subqueries();
    test_scalar_functions();
    test_window_functions();
    test_comments();
    test_syntax_errors();

    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
