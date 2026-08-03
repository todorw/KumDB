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

    KdbColumnDef cols[] = { { "name", KDB_TYPE_STRING, 0, 0, 0 } };
    ASSERT_OK(kdb_create_table(db, TABLE, cols, 1));

    KdbField f[] = { kdb_field_string("name", "Alice"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f));

    ASSERT_OK(kdb_add_column(db, TABLE, "age", KDB_TYPE_INT, 1, 1, 0));

    KdbColumnInfo schema[16];
    uint32_t n = 0;
    ASSERT_OK(kdb_get_schema(db, TABLE, schema, 16, &n));
    ASSERT_EQ(n, 2u);
    if (n == 2) {
        ASSERT_STR(schema[1].name, "age");
        ASSERT(schema[1].indexed);
    }

    /* duplicate column: real column-specific error, not a table-exists one */
    ASSERT_ERR(kdb_add_column(db, TABLE, "age", KDB_TYPE_INT, 1, 0, 0));
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

static void test_constraints_api(void) {
    KumDB *db;
    setup(&db);

    KdbColumnDef cols[] = {
        { "email", KDB_TYPE_STRING, 1, 0, 1 }, /* unique, nullable */
        { "name",  KDB_TYPE_STRING, 0, 0, 0 }, /* not null */
    };
    ASSERT_OK(kdb_create_table(db, TABLE, cols, 2));

    KdbField f1[] = { kdb_field_string("email", "a@x.com"), kdb_field_string("name", "alice"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f1));

    /* duplicate unique value rejected */
    KdbField f2[] = { kdb_field_string("email", "a@x.com"), kdb_field_string("name", "bob"), kdb_field_end() };
    ASSERT_ERR(kdb_add(db, TABLE, f2));
    ASSERT_EQ(kdb_last_status(), KDB_ERR_VALIDATION);

    /* missing NOT NULL value rejected */
    KdbField f3[] = { kdb_field_string("email", "b@x.com"), kdb_field_end() }; /* no "name" at all */
    ASSERT_ERR(kdb_add(db, TABLE, f3));
    ASSERT_EQ(kdb_last_status(), KDB_ERR_VALIDATION);

    /* NULL email doesn't conflict with another NULL email */
    KdbField f4[] = { kdb_field_null("email"), kdb_field_string("name", "carol"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f4));
    KdbField f5[] = { kdb_field_null("email"), kdb_field_string("name", "dave"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f5));

    /* kdb_update also enforces it: rejects creating a duplicate */
    KdbField patch[] = { kdb_field_string("email", "a@x.com"), kdb_field_end() };
    const char *where[] = { "name=carol", NULL };
    size_t updated = 0;
    ASSERT_ERR(kdb_update(db, TABLE, where, patch, &updated));
    ASSERT_EQ(kdb_last_status(), KDB_ERR_VALIDATION);

    /* a table with no unique/not-null columns skips the check entirely --
     * cheap path, still correct (nothing to reject) */
    KdbColumnDef plain_cols[] = { { "x", KDB_TYPE_INT, 1, 0, 0 } };
    ASSERT_OK(kdb_create_table(db, "plain", plain_cols, 1));
    KdbField p1[] = { kdb_field_int("x", 1), kdb_field_end() };
    ASSERT_OK(kdb_add(db, "plain", p1));
    KdbField p2[] = { kdb_field_int("x", 1), kdb_field_end() };
    ASSERT_OK(kdb_add(db, "plain", p2)); /* duplicate x is fine -- not unique */

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

    /* a one-element id__in list used to silently match nothing -- the
     * whole raw value ("3") got type-inferred into an INT instead of
     * staying list-text, and the IN matcher requires a STRING value to
     * split on commas (see kdb_query_add_filter's KDB_OP_IN special case,
     * query.c). Exercised through kdb_delete too since that's the code
     * path SQL's WHERE-with-parens/EXISTS UPDATE/DELETE fallback depends
     * on (sql__resolve_where_to_id_filter, sql.c). */
    const char *id_in_one[] = { "id__in=3", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, id_in_one), 1);

    const char *id_in_many[] = { "id__in=2,3,4", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, id_in_many), 3);

    size_t deleted = 0;
    ASSERT_OK(kdb_delete(db, TABLE, id_in_one, &deleted));
    ASSERT_EQ(deleted, 1u);
    ASSERT_EQ(kdb_count(db, TABLE, NULL), 4);

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

static void test_nested_dot_path_filter(void) {
    KumDB *db;
    setup(&db);

    KdbField addr1[] = { kdb_field_string("city", "NYC"), kdb_field_int("zip", 10001), kdb_field_end() };
    KdbField f1[] = { kdb_field_string("name", "alice"), kdb_field_object("address", addr1), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f1));

    KdbField addr2[] = { kdb_field_string("city", "LA"), kdb_field_int("zip", 90001), kdb_field_end() };
    KdbField f2[] = { kdb_field_string("name", "bob"), kdb_field_object("address", addr2), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f2));

    /* no address at all -- dot-path just doesn't match, doesn't crash */
    KdbField f3[] = { kdb_field_string("name", "carol"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, TABLE, f3));

    const char *by_city[] = { "address.city=NYC", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, by_city), 1);

    const char *by_zip[] = { "address.zip__gt=50000", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, by_zip), 1);

    const char *missing_addr[] = { "address.city__isnull", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, missing_addr), 1); /* carol */

    /* a path into a field that isn't there, or isn't an OBJECT -- neither
     * crashes, both just resolve to "not found" */
    const char *no_such_path[] = { "address.country=US", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, no_such_path), 0);

    const char *not_an_object[] = { "name.first=alice", NULL };
    ASSERT_EQ(kdb_count(db, TABLE, not_an_object), 0);

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

static void test_aggregate_pipeline(void) {
    KumDB *db;
    setup(&db);

    KdbField f1[] = { kdb_field_string("region", "east"), kdb_field_string("rep", "alice"), kdb_field_float("amount", 100.0), kdb_field_end() };
    KdbField f2[] = { kdb_field_string("region", "east"), kdb_field_string("rep", "bob"),   kdb_field_float("amount", 150.0), kdb_field_end() };
    KdbField f3[] = { kdb_field_string("region", "west"), kdb_field_string("rep", "carol"), kdb_field_float("amount", 200.0), kdb_field_end() };
    KdbField f4[] = { kdb_field_string("region", "west"), kdb_field_string("rep", "dave"),  kdb_field_float("amount", 50.0),  kdb_field_end() };
    ASSERT_OK(kdb_add(db, "sales", f1));
    ASSERT_OK(kdb_add(db, "sales", f2));
    ASSERT_OK(kdb_add(db, "sales", f3));
    ASSERT_OK(kdb_add(db, "sales", f4));

    /* $match alone */
    {
        const char *filters[] = { "region=east", NULL };
        KdbStage stages[] = {{ .type = KDB_STAGE_MATCH, .as = { .match_filters = filters } }};
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_aggregate(db, "sales", stages, 1, &rows));
        ASSERT(rows && rows->count == 2u);
        if (rows) kdb_rows_free(rows);
    }

    /* $group by region with every accumulator type */
    {
        KdbAccumulator accs[] = {
            { "total",      KDB_ACC_SUM,   "amount" },
            { "avg_amount", KDB_ACC_AVG,   "amount" },
            { "n",          KDB_ACC_COUNT, NULL },
            { "min_amount", KDB_ACC_MIN,   "amount" },
            { "max_amount", KDB_ACC_MAX,   "amount" },
        };
        const char *group_by[] = { "region", NULL };
        KdbStage stages[] = {{
            .type = KDB_STAGE_GROUP,
            .as = { .group = { .group_by = group_by, .accumulators = accs, .n_accumulators = 5 } }
        }};
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_aggregate(db, "sales", stages, 1, &rows));
        ASSERT(rows && rows->count == 2u);
        if (rows) {
            for (size_t i = 0; i < rows->count; i++) {
                const char *region = NULL;
                double total = 0, mn = 0, mx = 0;
                int64_t n = 0;
                ASSERT_OK(kdb_row_get_string(&rows->rows[i], "region", &region));
                ASSERT_OK(kdb_row_get_float(&rows->rows[i], "total", &total));
                ASSERT_OK(kdb_row_get_int(&rows->rows[i], "n", &n));
                ASSERT_OK(kdb_row_get_float(&rows->rows[i], "min_amount", &mn));
                ASSERT_OK(kdb_row_get_float(&rows->rows[i], "max_amount", &mx));
                ASSERT_EQ(n, 2);
                ASSERT(total > 249.9 && total < 250.1); /* both regions sum to 250 */
                if (strcmp(region, "east") == 0) {
                    ASSERT(mn > 99.9 && mn < 100.1);
                    ASSERT(mx > 149.9 && mx < 150.1);
                } else {
                    ASSERT_STR(region, "west");
                    ASSERT(mn > 49.9 && mn < 50.1);
                    ASSERT(mx > 199.9 && mx < 200.1);
                }
            }
            kdb_rows_free(rows);
        }
    }

    /* full pipeline: $match -> $group -> $sort -> $limit */
    {
        const char *filters[] = { "amount__gt=40", NULL };
        KdbAccumulator accs[] = { { "total", KDB_ACC_SUM, "amount" } };
        const char *group_by[] = { "region", NULL };
        KdbStage stages[] = {
            { .type = KDB_STAGE_MATCH, .as = { .match_filters = filters } },
            { .type = KDB_STAGE_GROUP, .as = { .group = { .group_by = group_by, .accumulators = accs, .n_accumulators = 1 } } },
            { .type = KDB_STAGE_SORT,  .as = { .sort = { .fields = {"total"}, .ascending = {0}, .n_fields = 1 } } },
            { .type = KDB_STAGE_LIMIT, .as = { .limit = 1 } },
        };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_aggregate(db, "sales", stages, 4, &rows));
        ASSERT(rows && rows->count == 1u);
        if (rows) kdb_rows_free(rows);
    }

    /* $project keeps only the named fields, id included */
    {
        const char *fields[] = { "id", "rep", NULL };
        KdbStage stages[] = {{ .type = KDB_STAGE_PROJECT, .as = { .project_fields = fields } }};
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_aggregate(db, "sales", stages, 1, &rows));
        ASSERT(rows && rows->count == 4u);
        if (rows && rows->count > 0) {
            ASSERT_EQ(rows->rows[0].field_count, 2u);
            ASSERT(kdb_row_get(&rows->rows[0], "id") != NULL);
            ASSERT(kdb_row_get(&rows->rows[0], "region") == NULL);
        }
        if (rows) kdb_rows_free(rows);
    }

    /* $sort (multi-key would tie-break here, single key is enough) + $skip */
    {
        KdbStage stages[] = {
            { .type = KDB_STAGE_SORT, .as = { .sort = { .fields = {"amount"}, .ascending = {1}, .n_fields = 1 } } },
            { .type = KDB_STAGE_SKIP, .as = { .skip = 3 } },
        };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_aggregate(db, "sales", stages, 2, &rows));
        ASSERT(rows && rows->count == 1u);
        if (rows && rows->count == 1) {
            const char *rep = NULL;
            ASSERT_OK(kdb_row_get_string(&rows->rows[0], "rep", &rep));
            ASSERT_STR(rep, "carol"); /* highest amount (200) */
        }
        if (rows) kdb_rows_free(rows);
    }

    /* $group with no GROUP BY-equivalent (group_by NULL) collapses to one row,
     * even over zero matching rows -- same as SQL's aggregate-with-no-GROUP-BY */
    {
        KdbField ex[] = { kdb_field_int("x", 1), kdb_field_end() };
        ASSERT_OK(kdb_add(db, "empty2", ex));
        ASSERT_OK(kdb_delete(db, "empty2", NULL, NULL));

        KdbAccumulator accs[] = { { "n", KDB_ACC_COUNT, NULL } };
        KdbStage stages[] = {{ .type = KDB_STAGE_GROUP, .as = { .group = { .group_by = NULL, .accumulators = accs, .n_accumulators = 1 } } }};
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_aggregate(db, "empty2", stages, 1, &rows));
        ASSERT(rows && rows->count == 1u);
        if (rows && rows->count == 1) {
            int64_t n = -1;
            ASSERT_OK(kdb_row_get_int(&rows->rows[0], "n", &n));
            ASSERT_EQ(n, 0);
        }
        if (rows) kdb_rows_free(rows);
    }

    /* error paths */
    {
        KdbRows *rows = NULL;
        ASSERT_ERR(kdb_aggregate(db, "nonexistent", NULL, 0, &rows));
        ASSERT_ERR(kdb_aggregate(NULL, "sales", NULL, 0, &rows));
    }

    teardown(db);
}

static void test_text_search(void) {
    KumDB *db;
    setup(&db);

    KdbField f1[] = { kdb_field_string("title", "The Quick Brown Fox"), kdb_field_string("body", "jumps over the lazy dog"), kdb_field_end() };
    KdbField f2[] = { kdb_field_string("title", "Lazy Cats"), kdb_field_string("body", "cats are lazy and quick sometimes"), kdb_field_end() };
    KdbField f3[] = { kdb_field_string("title", "Dogs and Foxes"), kdb_field_string("body", "the fox and the dog are friends"), kdb_field_end() };
    KdbField f4[] = { kdb_field_string("title", "Unrelated"), kdb_field_string("body", "nothing relevant here at all"), kdb_field_end() };
    ASSERT_OK(kdb_add(db, "articles", f1));
    ASSERT_OK(kdb_add(db, "articles", f2));
    ASSERT_OK(kdb_add(db, "articles", f3));
    ASSERT_OK(kdb_add(db, "articles", f4));

    /* default KDB_TEXT_MATCH_ALL: every term must appear somewhere */
    {
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_text_search(db, "articles", "fox dog", NULL, &rows));
        ASSERT(rows && rows->count == 2u); /* article 1 (title+body) and article 3 (title+body) */
        if (rows) {
            for (size_t i = 0; i < rows->count; i++) {
                double score = 0;
                ASSERT_OK(kdb_row_get_float(&rows->rows[i], "_score", &score));
                ASSERT(score > 0.0);
            }
            kdb_rows_free(rows);
        }
    }

    /* KDB_TEXT_MATCH_ANY: at least one term */
    {
        KdbTextSearchOpts opts = { .mode = KDB_TEXT_MATCH_ANY };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_text_search(db, "articles", "fox cats", &opts, &rows));
        ASSERT(rows && rows->count == 3u); /* 1(fox), 2(cats), 3(fox) */
        if (rows) kdb_rows_free(rows);
    }

    /* case-insensitive, whole-word (not substring) matching */
    {
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_text_search(db, "articles", "QUICK", NULL, &rows));
        ASSERT(rows && rows->count == 2u); /* article1 title, article2 body */
        if (rows) kdb_rows_free(rows);
    }

    /* relevance ranking: higher term frequency sorts first */
    {
        KdbField rep[] = { kdb_field_string("title", "dog dog dog"), kdb_field_string("body", "just dog"), kdb_field_end() };
        ASSERT_OK(kdb_add(db, "articles", rep));
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_text_search(db, "articles", "dog", NULL, &rows));
        ASSERT(rows && rows->count > 0);
        if (rows && rows->count > 0) {
            const char *title = NULL;
            ASSERT_OK(kdb_row_get_string(&rows->rows[0], "title", &title));
            ASSERT_STR(title, "dog dog dog");
        }
        if (rows) kdb_rows_free(rows);
    }

    /* opts->fields restricts which fields are searched */
    {
        const char *only_title[] = { "title", NULL };
        KdbTextSearchOpts opts = { .fields = only_title };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_text_search(db, "articles", "jumps", &opts, &rows));
        ASSERT(rows && rows->count == 0u); /* "jumps" is only in a body field */
        if (rows) kdb_rows_free(rows);
    }

    /* limit */
    {
        KdbTextSearchOpts opts = { .mode = KDB_TEXT_MATCH_ANY, .limit = 1 };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_text_search(db, "articles", "dog fox cats", &opts, &rows));
        ASSERT(rows && rows->count == 1u);
        if (rows) kdb_rows_free(rows);
    }

    /* an empty/punctuation-only query matches nothing */
    {
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_text_search(db, "articles", "   ", NULL, &rows));
        ASSERT(rows && rows->count == 0u);
        if (rows) kdb_rows_free(rows);
    }

    /* error paths */
    {
        KdbField nsf[] = { kdb_field_int("x", 1), kdb_field_end() };
        ASSERT_OK(kdb_add(db, "nostrings", nsf));
    }
    {
        KdbRows *rows = NULL;
        ASSERT_ERR(kdb_text_search(db, "nostrings", "x", NULL, &rows));    /* no STRING field to search */
        ASSERT_ERR(kdb_text_search(db, "nonexistent", "x", NULL, &rows)); /* table doesn't exist */
    }

    teardown(db);
}

static void test_geo_queries(void) {
    KumDB *db;
    setup(&db);

    /* NYC ~ (40.7128, -74.0060), Boston ~ (42.3601, -71.0589),
     * LA ~ (34.0522, -118.2437), London ~ (51.5074, -0.1278) */
    KdbField f1[] = { kdb_field_string("name", "NYC"),    kdb_field_float("lat", 40.7128), kdb_field_float("lon", -74.0060),  kdb_field_end() };
    KdbField f2[] = { kdb_field_string("name", "Boston"), kdb_field_float("lat", 42.3601), kdb_field_float("lon", -71.0589),  kdb_field_end() };
    KdbField f3[] = { kdb_field_string("name", "LA"),     kdb_field_float("lat", 34.0522), kdb_field_float("lon", -118.2437), kdb_field_end() };
    KdbField f4[] = { kdb_field_string("name", "London"), kdb_field_float("lat", 51.5074), kdb_field_float("lon", -0.1278),   kdb_field_end() };
    ASSERT_OK(kdb_add(db, "cities", f1));
    ASSERT_OK(kdb_add(db, "cities", f2));
    ASSERT_OK(kdb_add(db, "cities", f3));
    ASSERT_OK(kdb_add(db, "cities", f4));

    /* Haversine sanity: known real-world distances, and a point to itself is ~0 */
    {
        double d = kdb_geo_distance_km(40.7128, -74.0060, 42.3601, -71.0589);
        ASSERT(d > 290.0 && d < 320.0); /* NYC-Boston is ~306km */
        double d_self = kdb_geo_distance_km(40.7128, -74.0060, 40.7128, -74.0060);
        ASSERT(d_self < 0.01);
    }

    /* kdb_geo_near with a radius cap, sorted nearest-first, "_distance_km" attached */
    {
        KdbGeoNearOpts opts = { .lat_field = "lat", .lon_field = "lon",
                                 .center_lat = 40.7128, .center_lon = -74.0060, .max_distance_km = 500 };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_geo_near(db, "cities", &opts, &rows));
        ASSERT(rows && rows->count == 2u); /* NYC itself + Boston, LA/London too far */
        if (rows && rows->count == 2) {
            const char *first = NULL;
            double d0 = -1, d1 = -1;
            ASSERT_OK(kdb_row_get_string(&rows->rows[0], "name", &first));
            ASSERT_OK(kdb_row_get_float(&rows->rows[0], "_distance_km", &d0));
            ASSERT_OK(kdb_row_get_float(&rows->rows[1], "_distance_km", &d1));
            ASSERT_STR(first, "NYC"); /* nearest first */
            ASSERT(d0 <= d1);
        }
        if (rows) kdb_rows_free(rows);
    }

    /* no radius cap: every row with valid coordinates, still sorted by distance */
    {
        KdbGeoNearOpts opts = { .lat_field = "lat", .lon_field = "lon", .center_lat = 40.7128, .center_lon = -74.0060 };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_geo_near(db, "cities", &opts, &rows));
        ASSERT(rows && rows->count == 4u);
        if (rows && rows->count == 4) {
            const char *farthest = NULL;
            ASSERT_OK(kdb_row_get_string(&rows->rows[3], "name", &farthest));
            ASSERT_STR(farthest, "London");
        }
        if (rows) kdb_rows_free(rows);
    }

    /* limit */
    {
        KdbGeoNearOpts opts = { .lat_field = "lat", .lon_field = "lon", .center_lat = 40.7128, .center_lon = -74.0060, .limit = 1 };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_geo_near(db, "cities", &opts, &rows));
        ASSERT(rows && rows->count == 1u);
        if (rows) kdb_rows_free(rows);
    }

    /* kdb_geo_within_box: a US bounding box excludes London */
    {
        KdbGeoBoxOpts opts = { .lat_field = "lat", .lon_field = "lon",
                                .min_lat = 20.0, .max_lat = 50.0, .min_lon = -130.0, .max_lon = -60.0 };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_geo_within_box(db, "cities", &opts, &rows));
        ASSERT(rows && rows->count == 3u); /* NYC, Boston, LA -- not London */
        if (rows) {
            for (size_t i = 0; i < rows->count; i++) {
                const char *name = NULL;
                ASSERT_OK(kdb_row_get_string(&rows->rows[i], "name", &name));
                ASSERT(strcmp(name, "London") != 0);
            }
            kdb_rows_free(rows);
        }
    }

    /* a row missing lat/lon is silently excluded, not an error */
    {
        KdbField f5[] = { kdb_field_string("name", "NoCoords"), kdb_field_end() };
        ASSERT_OK(kdb_add(db, "cities", f5));
        KdbGeoNearOpts opts = { .lat_field = "lat", .lon_field = "lon" };
        KdbRows *rows = NULL;
        ASSERT_OK(kdb_geo_near(db, "cities", &opts, &rows));
        ASSERT(rows && rows->count == 4u); /* still just the 4 with real coordinates */
        if (rows) kdb_rows_free(rows);
    }

    /* error paths */
    {
        KdbGeoNearOpts bad_lat = { .lat_field = "lat", .lon_field = "lon", .center_lat = 999.0 };
        KdbRows *rows = NULL;
        ASSERT_ERR(kdb_geo_near(db, "cities", &bad_lat, &rows));

        KdbGeoBoxOpts bad_box = { .lat_field = "lat", .lon_field = "lon", .min_lat = 50, .max_lat = 10, .min_lon = -1, .max_lon = 1 };
        ASSERT_ERR(kdb_geo_within_box(db, "cities", &bad_box, &rows));

        KdbGeoNearOpts plain_opts = { .lat_field = "lat", .lon_field = "lon" };
        ASSERT_ERR(kdb_geo_near(db, "nonexistent", &plain_opts, &rows));
    }

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
    test_constraints_api();
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
    test_nested_dot_path_filter();
    test_nested_survives_reopen();
    test_list_tables_repeated();
    test_aggregate_pipeline();
    test_text_search();
    test_geo_queries();

    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}