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

static void test_syntax_errors(void) {
    KumDB *db;
    setup(&db);
    ASSERT_OK(sql(db, "CREATE TABLE t (n INT)"));
    ASSERT_OK(sql(db, "INSERT INTO t (n) VALUES (1)"));

    ASSERT_ERR(sql(db, "SELEKT * FROM t"));
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE n = NULL"));
    ASSERT_ERR(sql(db, "SELECT * FROM t WHERE n LIKE '%mid%dle%'"));
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
    test_update_delete();
    test_where_operators();
    test_or();
    test_group_by_and_aggregates();
    test_alter_table();
    test_reserved_columns_skipped();
    test_drop_table();
    test_syntax_errors();

    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
