#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/kumdb.h"

#define DEFAULT_ROWS    10000
#define DEFAULT_DIR     "/tmp/kumdb_bench"
#define TABLE_NAME      "bench_users"

static double now_ms(void) {
    return (double)clock() / (double)(CLOCKS_PER_SEC / 1000);
}

static void print_result(const char *op, size_t count, double elapsed_ms) {
    double per_op = elapsed_ms / (double)count;
    double ops_sec = 1000.0 / per_op;
    printf("  %-20s  %7zu rows  %8.2f ms  %10.0f ops/sec\n",
           op, count, elapsed_ms, ops_sec);
}

static void bench_insert(KumDB *db, size_t n) {
    char name_buf[64];
    double t0 = now_ms();

    for (size_t i = 0; i < n; i++) {
        snprintf(name_buf, sizeof(name_buf), "user_%zu", i);
        KdbField fields[] = {
            kdb_field_string("name",   name_buf),
            kdb_field_int   ("age",    (int64_t)(20 + (i % 60))),
            kdb_field_float ("score",  (double)(i % 100) / 10.0),
            kdb_field_bool  ("active", i % 3 != 0),
            kdb_field_end   ()
        };
        KdbStatus st = kdb_add(db, TABLE_NAME, fields);
        if (st != KDB_OK) {
            fprintf(stderr, "Insert failed at row %zu: %s\n", i, kdb_last_error());
            return;
        }
    }

    print_result("insert", n, now_ms() - t0);
}

static void bench_find_all(KumDB *db, size_t n) {
    double t0 = now_ms();

    KdbRows *rows = kdb_find(db, TABLE_NAME, NULL);
    if (!rows) {
        fprintf(stderr, "find_all failed: %s\n", kdb_last_error());
        return;
    }

    size_t count = rows->count;
    kdb_rows_free(rows);
    print_result("find (all)", count, now_ms() - t0);
    (void)n;
}

static void bench_find_filter(KumDB *db) {
    const char *filters[] = { "active=true", NULL };
    double t0 = now_ms();

    KdbRows *rows = kdb_find(db, TABLE_NAME, filters);
    if (!rows) {
        fprintf(stderr, "find_filter failed: %s\n", kdb_last_error());
        return;
    }

    size_t count = rows->count;
    kdb_rows_free(rows);
    print_result("find (active=true)", count, now_ms() - t0);
}

static void bench_count(KumDB *db, size_t n) {
    double t0 = now_ms();

    for (size_t i = 0; i < 100; i++) {
        int64_t c = kdb_count(db, TABLE_NAME, NULL);
        (void)c;
    }

    print_result("count x100", 100, now_ms() - t0);
    (void)n;
}

static void bench_update(KumDB *db, size_t n) {
    const char *where[] = { "active=false", NULL };
    KdbField set[] = {
        kdb_field_bool("active", 1),
        kdb_field_end()
    };

    double t0 = now_ms();
    size_t updated = 0;
    KdbStatus st = kdb_update(db, TABLE_NAME, where, set, &updated);
    if (st != KDB_OK) {
        fprintf(stderr, "update failed: %s\n", kdb_last_error());
        return;
    }

    print_result("update (batch)", updated, now_ms() - t0);
    (void)n;
}

static void bench_delete(KumDB *db, size_t n) {
    const char *where[] = { "age__lt=25", NULL };

    double t0 = now_ms();
    size_t deleted = 0;
    KdbStatus st = kdb_delete(db, TABLE_NAME, where, &deleted);
    if (st != KDB_OK) {
        fprintf(stderr, "delete failed: %s\n", kdb_last_error());
        return;
    }

    print_result("delete (age<25)", deleted, now_ms() - t0);
    (void)n;
}

static void bench_compact(KumDB *db) {
    double t0 = now_ms();
    KdbStatus st = kdb_compact(db, TABLE_NAME);
    if (st != KDB_OK) {
        fprintf(stderr, "compact failed: %s\n", kdb_last_error());
        return;
    }
    printf("  %-20s  %8.2f ms\n", "compact", now_ms() - t0);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [rows] [data_dir]\n", prog);
    fprintf(stderr, "  rows      number of rows to benchmark (default %d)\n", DEFAULT_ROWS);
    fprintf(stderr, "  data_dir  database directory (default %s)\n", DEFAULT_DIR);
}

int main(int argc, char **argv) {
    size_t     n       = DEFAULT_ROWS;
    const char *dir    = DEFAULT_DIR;

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        n = (size_t)atol(argv[1]);
        if (n == 0) { fprintf(stderr, "Invalid row count.\n"); return 1; }
    }
    if (argc >= 3) dir = argv[2];

    printf("KumDB Benchmark\n");
    printf("  rows: %zu  dir: %s\n\n", n, dir);

    KumDB *db = kdb_open(dir);
    if (!db) {
        fprintf(stderr, "Failed to open database: %s\n", kdb_last_error());
        return 1;
    }

    if (kdb_table_exists(db, TABLE_NAME)) {
        kdb_drop_table(db, TABLE_NAME);
    }

    bench_insert     (db, n);
    bench_find_all   (db, n);
    bench_find_filter(db);
    bench_count      (db, n);
    bench_update     (db, n);
    bench_delete     (db, n);
    bench_compact    (db);

    printf("\nFinal row count: %lld\n", (long long)kdb_count(db, TABLE_NAME, NULL));

    kdb_close(db);
    return 0;
}