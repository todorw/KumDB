#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../include/kumdb.h"

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

#define TEST_DIR   "/tmp/kumdb_test_core"
#define TABLE      "users"

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

static void test_insert_and_find(void) {
    KumDB *db;
    setup(&db);

    KdbField row1[] = {
        kdb_field_string("name",   "Alice"),
        kdb_field_int   ("age",    30),
        kdb_field_bool  ("active", 1),
        kdb_field_end   ()
    };
    KdbField row2[] = {
        kdb_field_string("name",   "Bob"),
        kdb_field_int   ("age",    25),
        kdb_field_bool  ("active", 0),
        kdb_field_end   ()
    };

    ASSERT_OK(kdb_add(db, TABLE, row1));
    ASSERT_OK(kdb_add(db, TABLE, row2));

    KdbRows *rows = kdb_find(db, TABLE, NULL);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);

    teardown(db);
}

static void test_find_with_filter(void) {
    KumDB *db;
    setup(&db);

    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof(name), "user%d", i);
        KdbField f[] = {
            kdb_field_string("name",  name),
            kdb_field_int   ("score", i * 10),
            kdb_field_end   ()
        };
        ASSERT_OK(kdb_add(db, TABLE, f));
    }

    const char *filters[] = { "score__gte=50", NULL };
    KdbRows *rows = kdb_find(db, TABLE, filters);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 5u);
    kdb_rows_free(rows);

    const char *between[] = { "score__between=20,60", NULL };
    rows = kdb_find(db, TABLE, between);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 5u);
    kdb_rows_free(rows);

    /* __in used to be a stub that always matched nothing */
    const char *in_filter[] = { "score__in=0,30,90", NULL };
    rows = kdb_find(db, TABLE, in_filter);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 3u);
    kdb_rows_free(rows);

    const char *in_none[] = { "score__in=5,15,25", NULL };
    rows = kdb_find(db, TABLE, in_none);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 0u);
    kdb_rows_free(rows);

    teardown(db);
}

static void test_find_one(void) {
    KumDB *db;
    setup(&db);

    KdbField f[] = {
        kdb_field_string("name", "Charlie"),
        kdb_field_int   ("age",  40),
        kdb_field_end   ()
    };
    ASSERT_OK(kdb_add(db, TABLE, f));

    const char *filters[] = { "name=Charlie", NULL };
    KdbRow *row = kdb_find_one(db, TABLE, filters);
    ASSERT(row != NULL);

    const char *name = NULL;
    ASSERT_OK(kdb_row_get_string(row, "name", &name));
    ASSERT_STR(name, "Charlie");

    int64_t age = 0;
    ASSERT_OK(kdb_row_get_int(row, "age", &age));
    ASSERT_EQ(age, 40);

    kdb_row_free(row);
    teardown(db);
}

static void test_count(void) {
    KumDB *db;
    setup(&db);

    for (int i = 0; i < 5; i++) {
        KdbField f[] = {
            kdb_field_int("val", i),
            kdb_field_end()
        };
        ASSERT_OK(kdb_add(db, TABLE, f));
    }

    ASSERT_EQ(kdb_count(db, TABLE, NULL), 5);

    const char *filters[] = { "val__lt=3", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, filters), 3);

    teardown(db);
}

static void test_update(void) {
    KumDB *db;
    setup(&db);

    KdbField f[] = {
        kdb_field_string("name",   "Dave"),
        kdb_field_int   ("age",    22),
        kdb_field_bool  ("active", 0),
        kdb_field_end   ()
    };
    ASSERT_OK(kdb_add(db, TABLE, f));

    const char *where[] = { "name=Dave", NULL };
    KdbField patch[] = {
        kdb_field_bool("active", 1),
        kdb_field_int ("age",    23),
        kdb_field_end ()
    };
    size_t updated = 0;
    ASSERT_OK(kdb_update(db, TABLE, where, patch, &updated));
    ASSERT_EQ(updated, 1u);

    KdbRow *row = kdb_find_one(db, TABLE, where);
    ASSERT(row != NULL);
    int64_t age = 0;
    ASSERT_OK(kdb_row_get_int(row, "age", &age));
    ASSERT_EQ(age, 23);
    int active = 0;
    ASSERT_OK(kdb_row_get_bool(row, "active", &active));
    ASSERT_EQ(active, 1);
    kdb_row_free(row);

    teardown(db);
}

static void test_delete(void) {
    KumDB *db;
    setup(&db);

    for (int i = 0; i < 6; i++) {
        KdbField f[] = {
            kdb_field_int("val", i),
            kdb_field_end()
        };
        ASSERT_OK(kdb_add(db, TABLE, f));
    }
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 6);

    const char *where[] = { "val__gte=3", NULL };
    size_t deleted = 0;
    ASSERT_OK(kdb_delete(db, TABLE, where, &deleted));
    ASSERT_EQ(deleted, 3u);
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 3);

    teardown(db);
}

static void test_compact(void) {
    KumDB *db;
    setup(&db);

    for (int i = 0; i < 10; i++) {
        KdbField f[] = { kdb_field_int("n", i), kdb_field_end() };
        ASSERT_OK(kdb_add(db, TABLE, f));
    }

    const char *where[] = { "n__lt=5", NULL };
    size_t deleted = 0;
    ASSERT_OK(kdb_delete(db, TABLE, where, &deleted));
    ASSERT_EQ(deleted, 5u);

    ASSERT_OK(kdb_compact(db, TABLE));
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 5);

    teardown(db);
}

static void test_table_exists_and_drop(void) {
    KumDB *db;
    setup(&db);

    ASSERT(!kdb_table_exists(db, TABLE));

    KdbField f[] = { kdb_field_int("x", 1), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f));
    ASSERT(kdb_table_exists(db, TABLE));

    ASSERT_OK(kdb_drop_table(db, TABLE));
    ASSERT(!kdb_table_exists(db, TABLE));

    teardown(db);
}

static void test_alter_table_api(void) {
    KumDB *db;
    setup(&db);

    KdbColumnDef cols[] = { { "name", KDB_TYPE_STRING, 0, 0 } };
    ASSERT_OK(kdb_create_table(db, TABLE, cols, 1));

    KdbField f[] = { kdb_field_string("name", "Alice"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f));

    ASSERT_OK(kdb_add_column(db, TABLE, "age", KDB_TYPE_INT, 1, 1));

    KdbColumnInfo schema[16];
    uint32_t n = 0;
    ASSERT_OK(kdb_get_schema(db, TABLE, schema, 16, &n));
    ASSERT_EQ(n, 2u);
    if (n == 2) {
        ASSERT_STR(schema[1].name, "age");
        ASSERT(schema[1].indexed);
    }

    /* duplicate column: real column-specific error, not a table-exists one */
    ASSERT_ERR(kdb_add_column(db, TABLE, "age", KDB_TYPE_INT, 1, 0));
    ASSERT_EQ(kdb_last_status(), KDB_ERR_EXISTS);

    KdbField patch[] = { kdb_field_int("age", 30), kdb_field_end() };
    const char *where[] = { "name=Alice", NULL };
    size_t updated = 0;
    ASSERT_OK(kdb_update(db, TABLE, where, patch, &updated));
    ASSERT_EQ(updated, 1u);

    ASSERT_OK(kdb_drop_column(db, TABLE, "age"));
    ASSERT_OK(kdb_get_schema(db, TABLE, schema, 16, &n));
    ASSERT_EQ(n, 1u);

    KdbRow *row = kdb_find_one(db, TABLE, where);
    ASSERT(row != NULL);
    if (row) {
        ASSERT(kdb_row_get(row, "age") == NULL);
        kdb_row_free(row);
    }

    teardown(db);
}

static void test_tx_commit(void) {
    KumDB *db;
    setup(&db);

    KdbField seed[] = { kdb_field_string("name", "seed"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, seed));

    KdbTx *tx = kdb_tx_begin(db);
    ASSERT(tx != NULL);

    KdbField u[] = { kdb_field_string("name", "Alice"), kdb_field_end() };
    KdbField o[] = { kdb_field_string("item", "widget"), kdb_field_end() };
    ASSERT_OK(kdb_tx_add(tx, TABLE, u));
    ASSERT_OK(kdb_tx_add(tx, "orders", o)); /* table doesn't exist yet */

    ASSERT_OK(kdb_tx_commit(tx));

    ASSERT_EQ(kdb_count(db, TABLE, NULL), 2);
    ASSERT_EQ(kdb_count(db, "orders", NULL), 1);
    ASSERT(kdb_table_exists(db, "orders"));

    teardown(db);
}

static void test_tx_rollback(void) {
    KumDB *db;
    setup(&db);

    KdbField seed[] = { kdb_field_string("name", "seed"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, seed));

    KdbTx *tx = kdb_tx_begin(db);
    KdbField u[] = { kdb_field_string("name", "Bob"), kdb_field_end() };
    KdbField p[] = { kdb_field_string("name", "gadget"), kdb_field_end() };
    ASSERT_OK(kdb_tx_add(tx, TABLE, u));
    ASSERT_OK(kdb_tx_add(tx, "products", p)); /* new table, should vanish */

    ASSERT(kdb_table_exists(db, "products"));
    ASSERT_OK(kdb_tx_rollback(tx));

    /* everything back to exactly the pre-transaction state */
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 1);
    ASSERT(!kdb_table_exists(db, "products"));

    KdbRow *row = kdb_find_by_id(db, TABLE, 1);
    ASSERT(row != NULL);
    if (row) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(row, "name", &name));
        ASSERT_STR(name, "seed");
        kdb_row_free(row);
    }

    teardown(db);
}

static void test_tx_failed_op_forces_rollback(void) {
    KumDB *db;
    setup(&db);

    KdbField seed[] = { kdb_field_string("name", "seed"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, seed));

    KdbTx *tx = kdb_tx_begin(db);
    KdbField u[] = { kdb_field_string("name", "Carol"), kdb_field_end() };
    ASSERT_OK(kdb_tx_add(tx, TABLE, u));

    /* updating a table that doesn't exist is a real failure, not just 0 rows matched */
    ASSERT_ERR(kdb_tx_update(tx, "nonexistent_table", NULL, u, NULL));

    /* commit must refuse once the tx has a failed operation */
    ASSERT_ERR(kdb_tx_commit(tx));
    ASSERT_OK(kdb_tx_rollback(tx));

    /* Carol's insert must not have survived */
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 1);

    teardown(db);
}

static void test_tx_crash_recovery_uncommitted(void) {
    /* simulates a crash mid-transaction: operations applied to disk, but
       neither commit nor rollback ever ran. the next kdb_open() must undo it. */
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    KumDB *db = kdb_open(TEST_DIR);
    KdbField seed[] = { kdb_field_string("name", "seed"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, seed));

    KdbTx *tx = kdb_tx_begin(db);
    KdbField ghost[] = { kdb_field_string("name", "ghost"), kdb_field_end() };
    ASSERT_OK(kdb_tx_add(tx, TABLE, ghost));
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 2); /* visible mid-transaction */

    /* "crash": abandon the tx (never commit/rollback) right here -- the
       on-disk state (real file already has 2 rows, .txbak has the pre-tx
       snapshot) is exactly what a real crash at this point would leave.
       kdb_close() here only releases this process's in-memory/fd
       resources so the test binary doesn't leak across many tests --
       it doesn't touch the .txbak/marker files that matter for recovery. */
    free(tx);
    kdb_close(db);

    KumDB *db2 = kdb_open(TEST_DIR);
    ASSERT(db2 != NULL);
    ASSERT_EQ(kdb_count(db2, TABLE, NULL), 1); /* ghost insert rolled back automatically */

    KdbRow *row = kdb_find_by_id(db2, TABLE, 1);
    ASSERT(row != NULL);
    if (row) {
        const char *name = NULL;
        kdb_row_get_string(row, "name", &name);
        ASSERT_STR(name, "seed");
        kdb_row_free(row);
    }

    kdb_close(db2);
    system("rm -rf " TEST_DIR);
}

static void test_tx_crash_recovery_committed(void) {
    /* simulates a crash *after* the commit marker was written but *before*
       cleanup finished -- recovery must NOT roll back in this case, the
       marker means the transaction already committed. */
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    KumDB *db = kdb_open(TEST_DIR);
    KdbField seed[] = { kdb_field_string("name", "seed"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, seed));

    KdbTx *tx = kdb_tx_begin(db);
    KdbField committed_row[] = { kdb_field_string("name", "committed_row"), kdb_field_end() };
    ASSERT_OK(kdb_tx_add(tx, TABLE, committed_row));
    /* users.kdb now has 2 rows; users.kdb.txbak has 1 (the pre-tx snapshot) --
       normal mid-transaction state, exactly what kdb_tx_commit() would see
       right before it writes the marker */

    char marker_path[512];
    snprintf(marker_path, sizeof(marker_path), "%s/.kdb_tx_commit", TEST_DIR);
    FILE *mf = fopen(marker_path, "wb");
    ASSERT(mf != NULL);
    fputs(TABLE "\n", mf);
    fclose(mf);
    /* "crash" right here: marker written, backup not yet deleted */
    free(tx);
    kdb_close(db); /* just releases this process's handles, see comment above */

    KumDB *db2 = kdb_open(TEST_DIR);
    ASSERT(db2 != NULL);
    ASSERT_EQ(kdb_count(db2, TABLE, NULL), 2); /* committed_row must survive */

    kdb_close(db2);
    system("rm -rf " TEST_DIR);
}

static void test_reopen(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    KumDB *db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);

    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "item%d", i);
        KdbField f[] = {
            kdb_field_string("name", name),
            kdb_field_int   ("idx",  i),
            kdb_field_end   ()
        };
        ASSERT_OK(kdb_add(db, TABLE, f));
    }
    kdb_close(db);

    db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 5);

    kdb_close(db);
    system("rm -rf " TEST_DIR);
}

static void test_row_accessors(void) {
    KumDB *db;
    setup(&db);

    KdbField f[] = {
        kdb_field_string("name",  "Eve"),
        kdb_field_int   ("age",   29),
        kdb_field_float ("score", 9.5),
        kdb_field_bool  ("vip",   1),
        kdb_field_null  ("notes"),
        kdb_field_end   ()
    };
    ASSERT_OK(kdb_add(db, TABLE, f));

    const char *filters[] = { "name=Eve", NULL };
    KdbRow *row = kdb_find_one(db, TABLE, filters);
    ASSERT(row != NULL);

    const char *name = NULL;
    int64_t age = 0;
    double score = 0.0;
    int vip = 0;

    ASSERT_OK(kdb_row_get_string(row, "name",  &name));
    ASSERT_STR(name, "Eve");
    ASSERT_OK(kdb_row_get_int   (row, "age",   &age));
    ASSERT_EQ(age, 29);
    ASSERT_OK(kdb_row_get_float (row, "score", &score));
    ASSERT(score > 9.4 && score < 9.6);
    ASSERT_OK(kdb_row_get_bool  (row, "vip",   &vip));
    ASSERT_EQ(vip, 1);

    kdb_row_free(row);
    teardown(db);
}

static void test_find_by_id(void) {
    KumDB *db;
    setup(&db);

    KdbField f1[] = { kdb_field_string("name", "First"),  kdb_field_end() };
    KdbField f2[] = { kdb_field_string("name", "Second"), kdb_field_end() };
    KdbField f3[] = { kdb_field_string("name", "Third"),  kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f1));
    ASSERT_OK(kdb_add(db, TABLE, f2));
    ASSERT_OK(kdb_add(db, TABLE, f3));

    /* id=1 is the case that used to break: "1" got type-inferred as bool */
    KdbRow *row = kdb_find_by_id(db, TABLE, 1);
    ASSERT(row != NULL);
    if (row) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(row, "name", &name));
        ASSERT_STR(name, "First");
        ASSERT_EQ(row->id, 1u);
        kdb_row_free(row);
    }

    row = kdb_find_by_id(db, TABLE, 3);
    ASSERT(row != NULL);
    if (row) {
        const char *name = NULL;
        ASSERT_OK(kdb_row_get_string(row, "name", &name));
        ASSERT_STR(name, "Third");
        kdb_row_free(row);
    }

    row = kdb_find_by_id(db, TABLE, 999);
    ASSERT(row == NULL);

    teardown(db);
}

static void test_id_and_timestamp_filters(void) {
    KumDB *db;
    setup(&db);

    for (int i = 0; i < 5; i++) {
        KdbField f[] = { kdb_field_int("n", i), kdb_field_end() };
        ASSERT_OK(kdb_add(db, TABLE, f));
    }

    const char *gte3[] = { "id__gte=3", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, gte3), 3);

    const char *between[] = { "id__between=2,4", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, between), 3);

    const char *notnull[] = { "created_at__isnotnull", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, notnull), 5);

    teardown(db);
}

static void test_numeric_literal_edge_cases(void) {
    KumDB *db;
    setup(&db);

    /* "0"/"1" used to type-infer as bool, breaking equality against int columns */
    KdbField f0[] = { kdb_field_int("count", 0), kdb_field_end() };
    KdbField f1[] = { kdb_field_int("count", 1), kdb_field_end() };
    KdbField f2[] = { kdb_field_int("count", 2), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f0));
    ASSERT_OK(kdb_add(db, TABLE, f1));
    ASSERT_OK(kdb_add(db, TABLE, f2));

    const char *eq0[] = { "count=0", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, eq0), 1);

    const char *eq1[] = { "count=1", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, eq1), 1);

    const char *gt0[] = { "count__gt=0", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, gt0), 2);

    teardown(db);
}

static void test_blob_field(void) {
    KumDB *db;
    setup(&db);

    unsigned char payload[] = { 0x00, 0x01, 0xFF, 0x42, 0x00, 0x99 };
    KdbField f[] = {
        kdb_field_string("name", "thumb"),
        kdb_field_blob  ("data", payload, sizeof(payload)),
        kdb_field_end   ()
    };
    ASSERT_OK(kdb_add(db, TABLE, f));

    const char *filters[] = { "name=thumb", NULL };
    KdbRow *row = kdb_find_one(db, TABLE, filters);
    ASSERT(row != NULL);
    if (row) {
        const void *data = NULL;
        size_t      len  = 0;
        ASSERT_OK(kdb_row_get_blob(row, "data", &data, &len));
        ASSERT_EQ(len, sizeof(payload));
        ASSERT(data != NULL && memcmp(data, payload, len) == 0);
        kdb_row_free(row);
    }

    teardown(db);
}

static void test_nested_array_object(void) {
    KumDB *db;
    setup(&db);

    KdbField tags[] = {
        kdb_field_string(NULL, "vip"),
        kdb_field_string(NULL, "premium"),
    };
    KdbField address[] = {
        kdb_field_string("city", "NYC"),
        kdb_field_int   ("zip",  10001),
        kdb_field_end   ()
    };
    KdbField f[] = {
        kdb_field_string("name", "Alice"),
        kdb_field_array ("tags", tags, 2),
        kdb_field_object("address", address),
        kdb_field_end   ()
    };
    ASSERT_OK(kdb_add(db, TABLE, f));

    const char *filters[] = { "name=Alice", NULL };
    KdbRow *row = kdb_find_one(db, TABLE, filters);
    ASSERT(row != NULL);
    if (row) {
        const KdbField *items = NULL;
        size_t count = 0;
        ASSERT_OK(kdb_row_get_array(row, "tags", &items, &count));
        ASSERT_EQ(count, 2u);
        if (count == 2) {
            ASSERT_STR(items[0].v.as_string, "vip");
            ASSERT_STR(items[1].v.as_string, "premium");
        }

        const KdbField *obj = NULL;
        ASSERT_OK(kdb_row_get_object(row, "address", &obj));
        ASSERT(obj != NULL);
        if (obj) {
            const KdbField *city = NULL;
            for (const KdbField *sub = obj; sub->name != NULL; sub++)
                if (strcmp(sub->name, "city") == 0) city = sub;
            ASSERT(city != NULL);
            if (city) ASSERT_STR(city->v.as_string, "NYC");
        }

        /* wrong-type getters fail cleanly instead of misreading memory */
        ASSERT_ERR(kdb_row_get_array(row, "address", &items, &count));
        ASSERT_ERR(kdb_row_get_object(row, "tags", &obj));

        kdb_row_free(row);
    }

    teardown(db);
}

static void test_nested_survives_reopen(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);

    KumDB *db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);

    KdbField addr[] = { kdb_field_string("city", "Berlin"), kdb_field_end() };
    KdbField f[] = {
        kdb_field_string("name", "Bob"),
        kdb_field_object("address", addr),
        kdb_field_end()
    };
    ASSERT_OK(kdb_add(db, TABLE, f));
    kdb_close(db);

    db = kdb_open(TEST_DIR);
    ASSERT(db != NULL);
    const char *filters[] = { "name=Bob", NULL };
    KdbRow *row = kdb_find_one(db, TABLE, filters);
    ASSERT(row != NULL);
    if (row) {
        const KdbField *obj = NULL;
        ASSERT_OK(kdb_row_get_object(row, "address", &obj));
        ASSERT(obj != NULL && obj->name != NULL);
        if (obj && obj->name) ASSERT_STR(obj->v.as_string, "Berlin");
        kdb_row_free(row);
    }

    kdb_close(db);
    system("rm -rf " TEST_DIR);
}

static void test_list_tables_repeated(void) {
    KumDB *db;
    setup(&db);

    KdbField f[] = { kdb_field_int("x", 1), kdb_field_end() };
    ASSERT_OK(kdb_add(db, "alpha", f));
    ASSERT_OK(kdb_add(db, "beta",  f));

    /* used to return dangling pointers into a freed stack frame */
    const char *names[16];
    size_t      count = 0;
    ASSERT_OK(kdb_list_tables(db, names, 16, &count));
    ASSERT_EQ(count, 2u);

    int saw_alpha = 0, saw_beta = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], "alpha") == 0) saw_alpha = 1;
        if (strcmp(names[i], "beta")  == 0) saw_beta  = 1;
    }
    ASSERT(saw_alpha && saw_beta);

    /* calling again shouldn't corrupt the first snapshot's semantics either */
    const char *names2[16];
    size_t      count2 = 0;
    ASSERT_OK(kdb_list_tables(db, names2, 16, &count2));
    ASSERT_EQ(count2, 2u);

    teardown(db);
}

int main(void) {
    printf("=== test_core ===\n");

    test_insert_and_find();
    test_find_with_filter();
    test_find_one();
    test_count();
    test_update();
    test_delete();
    test_compact();
    test_table_exists_and_drop();
    test_alter_table_api();
    test_tx_commit();
    test_tx_rollback();
    test_tx_failed_op_forces_rollback();
    test_tx_crash_recovery_uncommitted();
    test_tx_crash_recovery_committed();
    test_reopen();
    test_row_accessors();
    test_find_by_id();
    test_id_and_timestamp_filters();
    test_numeric_literal_edge_cases();
    test_blob_field();
    test_nested_array_object();
    test_nested_survives_reopen();
    test_list_tables_repeated();

    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}