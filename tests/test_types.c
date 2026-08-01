#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

#define ASSERT_EQ_INT(a, b)  ASSERT((a) == (b))
#define ASSERT_STR(a, b)     ASSERT(strcmp((a), (b)) == 0)

static void test_type_inference(void) {
    ASSERT_EQ_INT(kdb_type_infer(NULL),    KDB_TYPE_NULL);
    ASSERT_EQ_INT(kdb_type_infer(""),      KDB_TYPE_NULL);
    ASSERT_EQ_INT(kdb_type_infer("null"),  KDB_TYPE_NULL);
    ASSERT_EQ_INT(kdb_type_infer("NULL"),  KDB_TYPE_NULL);
    ASSERT_EQ_INT(kdb_type_infer("nil"),   KDB_TYPE_NULL);

    ASSERT_EQ_INT(kdb_type_infer("true"),  KDB_TYPE_BOOL);
    ASSERT_EQ_INT(kdb_type_infer("false"), KDB_TYPE_BOOL);
    ASSERT_EQ_INT(kdb_type_infer("True"),  KDB_TYPE_BOOL);
    ASSERT_EQ_INT(kdb_type_infer("yes"),   KDB_TYPE_BOOL);
    ASSERT_EQ_INT(kdb_type_infer("no"),    KDB_TYPE_BOOL);

    ASSERT_EQ_INT(kdb_type_infer("0"),     KDB_TYPE_BOOL);
    ASSERT_EQ_INT(kdb_type_infer("1"),     KDB_TYPE_BOOL);
    ASSERT_EQ_INT(kdb_type_infer("42"),    KDB_TYPE_INT);
    ASSERT_EQ_INT(kdb_type_infer("-7"),    KDB_TYPE_INT);
    ASSERT_EQ_INT(kdb_type_infer("99999"), KDB_TYPE_INT);

    ASSERT_EQ_INT(kdb_type_infer("3.14"),  KDB_TYPE_FLOAT);
    ASSERT_EQ_INT(kdb_type_infer("-0.5"),  KDB_TYPE_FLOAT);
    ASSERT_EQ_INT(kdb_type_infer("1e10"),  KDB_TYPE_FLOAT);

    ASSERT_EQ_INT(kdb_type_infer("hello"), KDB_TYPE_STRING);
    ASSERT_EQ_INT(kdb_type_infer("42abc"), KDB_TYPE_STRING);
}

static void test_value_int(void) {
    KdbValue v;
    ASSERT_EQ_INT(kdb_value_from_int(42, &v), KDB_OK);
    ASSERT_EQ_INT(v.type,      KDB_TYPE_INT);
    ASSERT_EQ_INT(v.v.as_int,  42);

    ASSERT_EQ_INT(kdb_value_from_int(-100, &v), KDB_OK);
    ASSERT_EQ_INT(v.v.as_int, -100);
}

static void test_value_float(void) {
    KdbValue v;
    ASSERT_EQ_INT(kdb_value_from_float(3.14, &v), KDB_OK);
    ASSERT_EQ_INT(v.type, KDB_TYPE_FLOAT);
    ASSERT(v.v.as_float > 3.13 && v.v.as_float < 3.15);
}

static void test_value_bool(void) {
    KdbValue v;
    ASSERT_EQ_INT(kdb_value_from_bool(1, &v), KDB_OK);
    ASSERT_EQ_INT(v.type,      KDB_TYPE_BOOL);
    ASSERT_EQ_INT(v.v.as_bool, 1);

    ASSERT_EQ_INT(kdb_value_from_bool(0, &v), KDB_OK);
    ASSERT_EQ_INT(v.v.as_bool, 0);
}

static void test_value_string(void) {
    KdbValue v;
    ASSERT_EQ_INT(kdb_value_from_string("hello", KDB_TYPE_STRING, &v), KDB_OK);
    ASSERT_EQ_INT(v.type, KDB_TYPE_STRING);
    int ok_data = strcmp(v.v.as_string.data, "hello") == 0;
    int ok_len  = (int)v.v.as_string.len == 5;
    kdb_value_free(&v);
    ASSERT(ok_data);
    ASSERT(ok_len);
}

static void test_value_null(void) {
    KdbValue v;
    ASSERT_EQ_INT(kdb_value_from_null(&v), KDB_OK);
    ASSERT_EQ_INT(v.type, KDB_TYPE_NULL);
}

static void test_value_copy(void) {
    KdbValue src, dst;
    kdb_value_from_string("copytest", KDB_TYPE_STRING, &src);
    ASSERT_EQ_INT(kdb_value_copy(&src, &dst), KDB_OK);
    ASSERT_STR(dst.v.as_string.data, "copytest");
    ASSERT(dst.v.as_string.data != src.v.as_string.data);
    kdb_value_free(&src);
    kdb_value_free(&dst);
}

static void test_value_compare(void) {
    KdbValue a, b;
    kdb_value_from_int(10, &a);
    kdb_value_from_int(20, &b);
    ASSERT(kdb_value_compare(&a, &b) < 0);
    ASSERT(kdb_value_compare(&b, &a) > 0);
    ASSERT(kdb_value_compare(&a, &a) == 0);

    kdb_value_from_float(1.5, &a);
    kdb_value_from_float(1.5, &b);
    ASSERT(kdb_value_compare(&a, &b) == 0);

    kdb_value_from_string("apple", KDB_TYPE_STRING, &a);
    kdb_value_from_string("banana", KDB_TYPE_STRING, &b);
    ASSERT(kdb_value_compare(&a, &b) < 0);
    kdb_value_free(&a);
    kdb_value_free(&b);
}

static void test_value_matches(void) {
    KdbValue field, fv, fv2;

    kdb_value_from_int(10, &field);
    kdb_value_from_int(10, &fv);
    ASSERT(kdb_value_matches(&field, KDB_OP_EQ,  &fv, NULL));
    ASSERT(!kdb_value_matches(&field, KDB_OP_NEQ, &fv, NULL));

    kdb_value_from_int(5, &fv);
    ASSERT(kdb_value_matches(&field, KDB_OP_GT,  &fv, NULL));
    ASSERT(kdb_value_matches(&field, KDB_OP_GTE, &fv, NULL));
    ASSERT(!kdb_value_matches(&field, KDB_OP_LT,  &fv, NULL));

    kdb_value_from_int(5, &fv);
    kdb_value_from_int(15, &fv2);
    ASSERT(kdb_value_matches(&field, KDB_OP_BETWEEN, &fv, &fv2));

    kdb_value_from_string("hello world", KDB_TYPE_STRING, &field);
    kdb_value_from_string("hello", KDB_TYPE_STRING, &fv);
    ASSERT(kdb_value_matches(&field, KDB_OP_STARTSWITH, &fv, NULL));
    ASSERT(kdb_value_matches(&field, KDB_OP_CONTAINS,   &fv, NULL));

    kdb_value_free(&fv);
    kdb_value_from_string("world", KDB_TYPE_STRING, &fv);
    ASSERT(kdb_value_matches(&field, KDB_OP_ENDSWITH, &fv, NULL));

    kdb_value_free(&field);
    kdb_value_free(&fv);

    KdbValue null_val;
    kdb_value_from_null(&null_val);
    ASSERT(kdb_value_matches(&null_val, KDB_OP_IS_NULL,     NULL, NULL));
    ASSERT(!kdb_value_matches(&null_val, KDB_OP_IS_NOT_NULL, NULL, NULL));
}

static void test_parse_filter_key(void) {
    char col[128];
    KdbOperator op;

    ASSERT_EQ_INT(kdb_parse_filter_key("name",           col, &op), KDB_OK);
    ASSERT_STR(col, "name"); ASSERT_EQ_INT(op, KDB_OP_EQ);

    ASSERT_EQ_INT(kdb_parse_filter_key("age__gt",        col, &op), KDB_OK);
    ASSERT_STR(col, "age"); ASSERT_EQ_INT(op, KDB_OP_GT);

    ASSERT_EQ_INT(kdb_parse_filter_key("score__between", col, &op), KDB_OK);
    ASSERT_EQ_INT(op, KDB_OP_BETWEEN);

    ASSERT_EQ_INT(kdb_parse_filter_key("x__isnull",      col, &op), KDB_OK);
    ASSERT_EQ_INT(op, KDB_OP_IS_NULL);

    ASSERT(kdb_parse_filter_key("age__bogus", col, &op) != KDB_OK);
}

static void test_value_cast(void) {
    KdbValue src, dst;

    kdb_value_from_int(42, &src);
    ASSERT_EQ_INT(kdb_value_cast(&src, KDB_TYPE_FLOAT, &dst), KDB_OK);
    ASSERT_EQ_INT(dst.type, KDB_TYPE_FLOAT);
    ASSERT(dst.v.as_float == 42.0);

    kdb_value_from_float(1.0, &src);
    ASSERT_EQ_INT(kdb_value_cast(&src, KDB_TYPE_INT, &dst), KDB_OK);
    ASSERT_EQ_INT(dst.v.as_int, 1);

    kdb_value_from_int(1, &src);
    ASSERT_EQ_INT(kdb_value_cast(&src, KDB_TYPE_BOOL, &dst), KDB_OK);
    ASSERT_EQ_INT(dst.v.as_bool, 1);

    kdb_value_from_int(0, &src);
    ASSERT_EQ_INT(kdb_value_cast(&src, KDB_TYPE_BOOL, &dst), KDB_OK);
    ASSERT_EQ_INT(dst.v.as_bool, 0);
}

static void test_value_array(void) {
    KdbValue elems[3];
    kdb_value_from_int(1, &elems[0]);
    kdb_value_from_string("two", KDB_TYPE_STRING, &elems[1]);
    kdb_value_from_float(3.5, &elems[2]);

    KdbValue arr;
    ASSERT(kdb_value_from_array(elems, 3, &arr) == KDB_OK);
    for (int i = 0; i < 3; i++) kdb_value_free(&elems[i]);

    ASSERT_EQ_INT(arr.type, KDB_TYPE_ARRAY);
    ASSERT_EQ_INT((int)arr.v.as_array.count, 3);
    ASSERT_EQ_INT(arr.v.as_array.items[0].v.as_int, 1);
    ASSERT_STR(arr.v.as_array.items[1].v.as_string.data, "two");

    char buf[128];
    kdb_value_to_str(&arr, buf, sizeof(buf));
    ASSERT_STR(buf, "[1, \"two\", 3.5]");

    /* deep copy is independent of the original */
    KdbValue arr_copy;
    ASSERT(kdb_value_copy(&arr, &arr_copy) == KDB_OK);
    ASSERT_EQ_INT(kdb_value_compare(&arr, &arr_copy), 0);
    kdb_value_free(&arr);
    ASSERT_EQ_INT(arr_copy.v.as_array.count, 3u); /* survives freeing the source */
    ASSERT_STR(arr_copy.v.as_array.items[1].v.as_string.data, "two");

    /* different array: NEQ must actually detect the difference, not just
       report "incomparable" (that was the trap here) */
    KdbValue diff_elems[1];
    kdb_value_from_int(99, &diff_elems[0]);
    KdbValue arr_diff;
    kdb_value_from_array(diff_elems, 1, &arr_diff);
    kdb_value_free(&diff_elems[0]);
    int cmp = kdb_value_compare(&arr_copy, &arr_diff);
    ASSERT(cmp != 0 && cmp != INT32_MIN);

    kdb_value_free(&arr_copy);
    kdb_value_free(&arr_diff);
}

static void test_value_object(void) {
    KdbRecordField fields[2];
    snprintf(fields[0].col_name, sizeof(fields[0].col_name), "city");
    kdb_value_from_string("NYC", KDB_TYPE_STRING, &fields[0].value);
    snprintf(fields[1].col_name, sizeof(fields[1].col_name), "zip");
    kdb_value_from_int(10001, &fields[1].value);

    KdbValue obj;
    ASSERT(kdb_value_from_object(fields, 2, &obj) == KDB_OK);
    kdb_value_free(&fields[0].value);
    kdb_value_free(&fields[1].value);

    ASSERT_EQ_INT(obj.type, KDB_TYPE_OBJECT);
    ASSERT_EQ_INT((int)obj.v.as_object.count, 2);
    ASSERT_STR(obj.v.as_object.fields[0].col_name, "city");

    char buf[128];
    kdb_value_to_str(&obj, buf, sizeof(buf));
    ASSERT_STR(buf, "{city: \"NYC\", zip: 10001}");

    KdbValue obj_copy;
    ASSERT(kdb_value_copy(&obj, &obj_copy) == KDB_OK);
    ASSERT_EQ_INT(kdb_value_compare(&obj, &obj_copy), 0);

    kdb_value_free(&obj);
    kdb_value_free(&obj_copy);
}

static void test_value_nested_deep(void) {
    /* array containing an object containing an array -- exercises real
       recursion, not just one level */
    KdbRecordField inner_fields[1];
    snprintf(inner_fields[0].col_name, sizeof(inner_fields[0].col_name), "tags");
    KdbValue inner_arr_elems[2];
    kdb_value_from_string("a", KDB_TYPE_STRING, &inner_arr_elems[0]);
    kdb_value_from_string("b", KDB_TYPE_STRING, &inner_arr_elems[1]);
    kdb_value_from_array(inner_arr_elems, 2, &inner_fields[0].value);
    kdb_value_free(&inner_arr_elems[0]);
    kdb_value_free(&inner_arr_elems[1]);

    KdbValue obj;
    kdb_value_from_object(inner_fields, 1, &obj);
    kdb_value_free(&inner_fields[0].value);

    KdbValue outer_elems[1];
    outer_elems[0] = obj; /* move semantics: obj's heap data now owned by outer_elems[0] */
    KdbValue outer;
    kdb_value_from_array(outer_elems, 1, &outer);
    kdb_value_free(&outer_elems[0]);

    char buf[256];
    kdb_value_to_str(&outer, buf, sizeof(buf));
    ASSERT_STR(buf, "[{tags: [\"a\", \"b\"]}]");

    kdb_value_free(&outer);
}

int main(void) {
    printf("=== test_types ===\n");

    test_type_inference();
    test_value_int();
    test_value_float();
    test_value_bool();
    test_value_string();
    test_value_null();
    test_value_copy();
    test_value_compare();
    test_value_matches();
    test_parse_filter_key();
    test_value_cast();
    test_value_array();
    test_value_object();
    test_value_nested_deep();

    printf("passed=%d  failed=%d\n", passed, failed);
    return failed > 0 ? 1 : 0;
}