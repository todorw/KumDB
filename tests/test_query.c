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

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_OK(st)   ASSERT((st) == KDB_OK)

#define TEST_DIR "/tmp/kumdb_test_query"
#define TABLE    "items"

static KumDB *db = NULL;

static void setup(void) {
    system("rm -rf " TEST_DIR);
    mkdir(TEST_DIR, 0755);
    db = kdb_open(TEST_DIR);

    const char *names[]   = { "alpha", "beta", "gamma", "delta", "epsilon" };
    int         scores[]  = { 10, 20, 30, 40, 50 };
    int         active[]  = { 1, 0, 1, 0, 1 };
    double      ratings[] = { 1.5, 2.5, 3.5, 4.5, 5.0 };

    for (int i = 0; i < 5; i++) {
        KdbField f[] = {
            kdb_field_string("name",   names[i]),
            kdb_field_int   ("score",  scores[i]),
            kdb_field_bool  ("active", active[i]),
            kdb_field_float ("rating", ratings[i]),
            kdb_field_end   ()
        };
        kdb_add(db, TABLE, f);
    }
}

static void teardown(void) {
    kdb_close(db);
    db = NULL;
    system("rm -rf " TEST_DIR);
}

static void test_eq(void) {
    const char *f[] = { "name=alpha", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 1u);
    kdb_rows_free(rows);
}

static void test_neq(void) {
    const char *f[] = { "name__neq=alpha", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 4u);
    kdb_rows_free(rows);
}

static void test_gt_lt(void) {
    const char *f[] = { "score__gt=20", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 3u);
    kdb_rows_free(rows);

    const char *f2[] = { "score__lt=30", NULL };
    rows = kdb_find(db, TABLE, f2);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);
}

static void test_gte_lte(void) {
    const char *f[] = { "score__gte=30", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 3u);
    kdb_rows_free(rows);

    const char *f2[] = { "score__lte=30", NULL };
    rows = kdb_find(db, TABLE, f2);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 3u);
    kdb_rows_free(rows);
}

static void test_between(void) {
    const char *f[] = { "score__between=20,40", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 3u);
    kdb_rows_free(rows);
}

static void test_bool_filter(void) {
    const char *f[] = { "active=true", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 3u);
    kdb_rows_free(rows);

    const char *f2[] = { "active=false", NULL };
    rows = kdb_find(db, TABLE, f2);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);
}

static void test_contains(void) {
    const char *f[] = { "name__contains=lph", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 1u);
    kdb_rows_free(rows);
}

static void test_startswith(void) {
    const char *f[] = { "name__startswith=be", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 1u);
    kdb_rows_free(rows);
}

static void test_endswith(void) {
    const char *f[] = { "name__endswith=ta", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);
}

static void test_multi_filter_and(void) {
    const char *f[] = { "active=true", "score__gt=20", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);
}

static void test_or_filter(void) {
    /* name=alpha(score10) OR score__gt=45(epsilon,50) -> 2 rows */
    const char *f[] = { "name=alpha", "OR:score__gt=45", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);

    /* AND binds tighter than OR:
       (active=true AND score__lt=20 -> alpha) OR (name=delta -> delta) = 2 rows */
    const char *f2[] = { "active=true", "score__lt=20", "OR:name=delta", NULL };
    rows = kdb_find(db, TABLE, f2);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);

    /* the AND-group doesn't match on its own, only the OR fallback should */
    const char *f3[] = { "active=true", "score__lt=20", "OR:name=beta", NULL };
    rows = kdb_find(db, TABLE, f3);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u); /* alpha (matches first group too) + beta */
    kdb_rows_free(rows);
}

static void test_in_filter(void) {
    const char *f[] = { "score__in=10,30,50", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 3u);
    kdb_rows_free(rows);

    const char *f2[] = { "name__in=beta,delta", NULL };
    rows = kdb_find(db, TABLE, f2);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);

    /* a one-element IN list whose sole value looks numeric used to get
     * type-inferred into an INT instead of staying the list-text the IN
     * matcher expects, silently matching nothing regardless of a real
     * match -- see kdb_query_add_filter's KDB_OP_IN special case. */
    const char *f3[] = { "score__in=30", NULL };
    rows = kdb_find(db, TABLE, f3);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 1u);
    kdb_rows_free(rows);

    const char *f4[] = { "name__in=beta", NULL };
    rows = kdb_find(db, TABLE, f4);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 1u);
    kdb_rows_free(rows);
}

static void test_like_filter(void) {
    /* real SQL LIKE wildcards -- '%' any run (incl. none), '_' exactly
     * one -- available as a NoSQL filter operator too, not just through
     * SQL's LIKE keyword. names: alpha, beta, gamma, delta, epsilon. */
    const char *f1[] = { "name__like=%eta", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f1);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 1u); /* beta */
    kdb_rows_free(rows);

    const char *f2[] = { "name__like=%a%", NULL };
    rows = kdb_find(db, TABLE, f2);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 4u); /* alpha, beta, gamma, delta -- not epsilon */
    kdb_rows_free(rows);

    const char *f3[] = { "name__like=_eta", NULL };
    rows = kdb_find(db, TABLE, f3);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 1u); /* beta */
    kdb_rows_free(rows);

    const char *f4[] = { "name__like=z%", NULL };
    rows = kdb_find(db, TABLE, f4);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 0u);
    kdb_rows_free(rows);

    const char *f5[] = { "name__like=%", NULL };
    rows = kdb_find(db, TABLE, f5);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 5u);
    kdb_rows_free(rows);
}

static void test_find_all(void) {
    KdbRows *rows = kdb_find(db, TABLE, NULL);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 5u);
    kdb_rows_free(rows);
}

static void test_no_results(void) {
    const char *f[] = { "score__gt=999", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 0u);
    kdb_rows_free(rows);
}

static void test_float_filter(void) {
    const char *f[] = { "rating__gte=4.0", NULL };
    KdbRows *rows = kdb_find(db, TABLE, f);
    ASSERT(rows != NULL);
    ASSERT_EQ(rows->count, 2u);
    kdb_rows_free(rows);
}

int main(void) {
    printf("=== test_query ===\n");
    setup();

    test_eq();
    test_neq();
    test_gt_lt();
    test_gte_lte();
    test_between();
    test_bool_filter();
    test_contains();
    test_startswith();
    test_endswith();
    test_multi_filter_and();
    test_or_filter();
    test_in_filter();
    test_like_filter();
    test_find_all();
    test_no_results();
    test_float_filter();

    teardown();
    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}