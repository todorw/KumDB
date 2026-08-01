#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../include/kumdb.h"
#include "../include/internal.h"
#include "../include/storage.h"
#include "../include/record.h"
#include "../include/types.h"
#include "../include/error.h"

static int passed = 0;
static int failed = 0;

#define ASSERT(expr) do { \
    if (expr) { passed++; } \
    else { \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, #expr); \
        failed++; \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_OK(st)   ASSERT((st) == KDB_OK)

#define TEST_DIR "/tmp/kumdb_test_storage"
#define TABLE    "things"

static void test_record_serialize_roundtrip(void) {
    KdbRecord *r = kdb_record_new(4);
    ASSERT(r != NULL);

    KdbValue v;
    kdb_value_from_int(42, &v);
    ASSERT_OK(kdb_record_set_field(r, "num", &v));

    kdb_value_from_string("hello", KDB_TYPE_STRING, &v);
    ASSERT_OK(kdb_record_set_field(r, "str", &v));
    kdb_value_free(&v);

    kdb_value_from_bool(1, &v);
    ASSERT_OK(kdb_record_set_field(r, "flag", &v));

    kdb_value_from_float(3.14, &v);
    ASSERT_OK(kdb_record_set_field(r, "pi", &v));

    size_t sz = kdb_record_serial_size(r);
    ASSERT(sz > 0);

    uint8_t *buf = malloc(sz);
    ASSERT(buf != NULL);

    size_t written = kdb_record_serialize(r, buf, sz);
    ASSERT_EQ(written, sz);

    size_t read_back = 0;
    KdbRecord *r2 = kdb_record_deserialize(buf, sz, &read_back);
    ASSERT(r2 != NULL);
    ASSERT_EQ(read_back, sz);
    ASSERT_EQ(r2->field_count, r->field_count);

    int64_t num = 0;
    ASSERT_OK(kdb_record_get_int(r2, "num", &num));
    ASSERT_EQ(num, 42);

    const char *str = NULL;
    ASSERT_OK(kdb_record_get_string(r2, "str", &str));
    ASSERT(strcmp(str, "hello") == 0);

    uint8_t flag = 0;
    ASSERT_OK(kdb_record_get_bool(r2, "flag", &flag));
    ASSERT_EQ(flag, 1);

    double pi = 0.0;
    ASSERT_OK(kdb_record_get_float(r2, "pi", &pi));
    ASSERT(pi > 3.13 && pi < 3.15);

    free(buf);
    kdb_record_free(r);
    kdb_record_free(r2);
}

static void test_storage_create_open_close(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    ASSERT_OK(kdb_storage_create(TEST_DIR, TABLE, NULL, 0));
    ASSERT(kdb_storage_exists(TEST_DIR, TABLE));

    KdbTable tbl;
    ASSERT_OK(kdb_storage_open(&tbl, TEST_DIR, TABLE));
    ASSERT(tbl.fp != NULL);

    kdb_storage_close(&tbl);
    system("rm -rf " TEST_DIR);
}

static void test_storage_append_and_scan(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    ASSERT_OK(kdb_storage_create(TEST_DIR, TABLE, NULL, 0));

    KdbTable tbl;
    ASSERT_OK(kdb_storage_open(&tbl, TEST_DIR, TABLE));

    for (int i = 0; i < 5; i++) {
        KdbRecord *r = kdb_record_new(1);
        KdbValue v;
        kdb_value_from_int(i * 10, &v);
        kdb_record_set_field(r, "val", &v);
        ASSERT_OK(kdb_storage_append(&tbl, r));
        kdb_record_free(r);
    }

    ASSERT_OK(kdb_storage_flush_header(&tbl));
    ASSERT_EQ(tbl.header.record_count, 5u);

    kdb_storage_close(&tbl);
    system("rm -rf " TEST_DIR);
}

typedef struct { int count; } ScanCtx;
static int count_cb(const KdbRecord *r, void *ud) {
    (void)r;
    ((ScanCtx *)ud)->count++;
    return 1;
}

static void test_storage_scan_c(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    ASSERT_OK(kdb_storage_create(TEST_DIR, TABLE, NULL, 0));

    KdbTable tbl;
    ASSERT_OK(kdb_storage_open(&tbl, TEST_DIR, TABLE));

    for (int i = 0; i < 7; i++) {
        KdbRecord *r = kdb_record_new(1);
        KdbValue v;
        kdb_value_from_int(i, &v);
        kdb_record_set_field(r, "n", &v);
        kdb_storage_append(&tbl, r);
        kdb_record_free(r);
    }
    kdb_storage_flush_header(&tbl);

    ScanCtx ctx = { 0 };
    ASSERT_OK(kdb_storage_scan(&tbl, count_cb, &ctx));
    ASSERT_EQ(ctx.count, 7);

    kdb_storage_close(&tbl);
    system("rm -rf " TEST_DIR);
}

static void test_compact_removes_deleted(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    KumDB *db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);

    for (int i = 0; i < 10; i++) {
        KdbField f[] = { kdb_field_int("n", i), kdb_field_end() };
        kdb_add(db, TABLE, f);
    }
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 10);

    const char *where[] = { "n__lt=5", NULL };
    size_t deleted = 0;
    ASSERT_OK(kdb_delete(db, TABLE, where, &deleted));
    ASSERT_EQ(deleted, 5u);

    ASSERT_OK(kdb_compact(db, TABLE));
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 5);

    kdb_close(db);
    system("rm -rf " TEST_DIR);
}

static void test_durability_across_reopen(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    KumDB *db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);

    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof(name), "item%d", i);
        KdbField f[] = {
            kdb_field_string("name", name),
            kdb_field_int   ("idx",  i),
            kdb_field_end   ()
        };
        kdb_add(db, TABLE, f);
    }
    kdb_close(db);

    db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 20);

    const char *where[] = { "idx__gte=10", NULL };
    size_t deleted = 0;
    kdb_delete(db, TABLE, where, &deleted);
    ASSERT_EQ(deleted, 10u);
    kdb_close(db);

    db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 10);

    kdb_close(db);
    system("rm -rf " TEST_DIR);
}

static void test_storage_drop(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    ASSERT_OK(kdb_storage_create(TEST_DIR, TABLE, NULL, 0));
    ASSERT(kdb_storage_exists(TEST_DIR, TABLE));
    ASSERT_OK(kdb_storage_drop(TEST_DIR, TABLE));
    ASSERT(!kdb_storage_exists(TEST_DIR, TABLE));

    system("rm -rf " TEST_DIR);
}

static void test_storage_list_tables(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    kdb_storage_create(TEST_DIR, "t1", NULL, 0);
    kdb_storage_create(TEST_DIR, "t2", NULL, 0);
    kdb_storage_create(TEST_DIR, "t3", NULL, 0);

    char names[KDB_MAX_TABLES][KDB_MAX_NAME_LEN];
    uint32_t count = 0;
    ASSERT_OK(kdb_storage_list_tables(TEST_DIR, names, &count));
    ASSERT_EQ(count, 3u);

    system("rm -rf " TEST_DIR);
}

int main(void) {
    printf("=== test_storage ===\n");

    test_record_serialize_roundtrip();
    test_storage_create_open_close();
    test_storage_append_and_scan();
    test_storage_scan_c();
    test_compact_removes_deleted();
    test_durability_across_reopen();
    test_storage_drop();
    test_storage_list_tables();

    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}