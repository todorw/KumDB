#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp -- POSIX, not string.h; glibc leaks it through string.h too but that's non-standard, don't rely on it */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>

#include "../include/types.h"
#include "../include/error.h"


static char *kdb__strdup(const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static const char *kdb__skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}


int kdb_str_is_null(const char *s) {
    if (!s) return 1;
    s = kdb__skip_ws(s);
    return (strcasecmp(s, "null") == 0 || strcasecmp(s, "nil") == 0 || *s == '\0');
}

int kdb_str_is_bool(const char *s) {
    s = kdb__skip_ws(s);
    return (strcasecmp(s, "true")  == 0 ||
            strcasecmp(s, "false") == 0 ||
            strcasecmp(s, "yes")   == 0 ||
            strcasecmp(s, "no")    == 0 ||
            strcmp(s, "1") == 0         ||
            strcmp(s, "0") == 0);
}

int kdb_str_is_int(const char *s) {
    s = kdb__skip_ws(s);
    if (!*s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

int kdb_str_is_float(const char *s) {
    s = kdb__skip_ws(s);
    if (!*s) return 0;
    int has_dot   = 0;
    int has_digit = 0;
    if (*s == '-' || *s == '+') s++;
    while (*s) {
        if (isdigit((unsigned char)*s)) {
            has_digit = 1;
        } else if (*s == '.' && !has_dot) {
            has_dot = 1;
        } else if ((*s == 'e' || *s == 'E') && has_digit) {
            s++;
            if (*s == '-' || *s == '+') s++;
            if (!isdigit((unsigned char)*s)) return 0;
            while (isdigit((unsigned char)*s)) s++;
            return has_digit && (*s == '\0');
        } else {
            return 0;
        }
        s++;
    }
    return has_digit && has_dot;
}


KdbType kdb_type_infer(const char *raw) {
    if (!raw) return KDB_TYPE_NULL;
    if (kdb_str_is_null(raw))  return KDB_TYPE_NULL;
    if (kdb_str_is_bool(raw))  return KDB_TYPE_BOOL;
    if (kdb_str_is_int(raw))   return KDB_TYPE_INT;
    if (kdb_str_is_float(raw)) return KDB_TYPE_FLOAT;
    return KDB_TYPE_STRING;
}


KdbStatus kdb_value_from_int(int64_t v, KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->type      = KDB_TYPE_INT;
    out->v.as_int  = v;
    return KDB_OK;
}

KdbStatus kdb_value_from_float(double v, KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->type        = KDB_TYPE_FLOAT;
    out->v.as_float  = v;
    return KDB_OK;
}

KdbStatus kdb_value_from_bool(uint8_t v, KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->type       = KDB_TYPE_BOOL;
    out->v.as_bool  = v ? 1 : 0;
    return KDB_OK;
}

KdbStatus kdb_value_from_null(KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->type = KDB_TYPE_NULL;
    return KDB_OK;
}

KdbStatus kdb_value_from_blob(const void *data, size_t len, KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->type = KDB_TYPE_BLOB;
    if (len == 0) {
        out->v.as_blob.data = NULL;
        out->v.as_blob.len  = 0;
        return KDB_OK;
    }
    if (!data) return KDB_ERR_BAD_ARG;
    uint8_t *copy = malloc(len);
    if (!copy) { kdb_err_oom("blob value"); return KDB_ERR_OOM; }
    memcpy(copy, data, len);
    out->v.as_blob.data = copy;
    out->v.as_blob.len  = len;
    return KDB_OK;
}

KdbStatus kdb_value_from_array(const KdbValue *items, size_t count, KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->type = KDB_TYPE_ARRAY;
    if (count == 0) return KDB_OK;
    if (!items) { kdb_err_null_arg("items", "kdb_value_from_array"); return KDB_ERR_BAD_ARG; }
    if (count > KDB_MAX_NEST_ELEMS) {
        kdb_err_bad_arg("count", "exceeds KDB_MAX_NEST_ELEMS");
        return KDB_ERR_FULL;
    }

    KdbValue *copy = (KdbValue *)calloc(count, sizeof(KdbValue));
    if (!copy) { kdb_err_oom("array value"); return KDB_ERR_OOM; }
    for (size_t i = 0; i < count; i++) {
        if (kdb_value_copy(&items[i], &copy[i]) != KDB_OK) {
            for (size_t j = 0; j < i; j++) kdb_value_free(&copy[j]);
            free(copy);
            return KDB_ERR_OOM;
        }
    }
    out->v.as_array.items = copy;
    out->v.as_array.count = count;
    return KDB_OK;
}

KdbStatus kdb_value_from_object(const KdbRecordField *fields, uint32_t count, KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));
    out->type = KDB_TYPE_OBJECT;
    if (count == 0) return KDB_OK;
    if (!fields) { kdb_err_null_arg("fields", "kdb_value_from_object"); return KDB_ERR_BAD_ARG; }
    if (count > KDB_MAX_NEST_ELEMS) {
        kdb_err_bad_arg("count", "exceeds KDB_MAX_NEST_ELEMS");
        return KDB_ERR_FULL;
    }

    KdbRecordField *copy = (KdbRecordField *)calloc(count, sizeof(KdbRecordField));
    if (!copy) { kdb_err_oom("object value"); return KDB_ERR_OOM; }
    for (uint32_t i = 0; i < count; i++) {
        KDB_STRLCPY(copy[i].col_name, fields[i].col_name, KDB_MAX_NAME_LEN);
        if (kdb_value_copy(&fields[i].value, &copy[i].value) != KDB_OK) {
            for (uint32_t j = 0; j < i; j++) kdb_value_free(&copy[j].value);
            free(copy);
            return KDB_ERR_OOM;
        }
    }
    out->v.as_object.fields = copy;
    out->v.as_object.count  = count;
    return KDB_OK;
}

KdbStatus kdb_value_from_string(const char *raw, KdbType hint, KdbValue *out) {
    if (!out) return KDB_ERR_BAD_ARG;
    memset(out, 0, sizeof(*out));

    KdbType t = (hint == KDB_TYPE_UNKNOWN || hint == KDB_TYPE_NULL)
                ? kdb_type_infer(raw)
                : hint;

    switch (t) {
        case KDB_TYPE_NULL:
            out->type = KDB_TYPE_NULL;
            return KDB_OK;

        case KDB_TYPE_BOOL: {
            uint8_t b = (strcasecmp(raw, "true")  == 0 ||
                         strcasecmp(raw, "yes")   == 0 ||
                         strcmp(raw, "1")         == 0) ? 1 : 0;
            return kdb_value_from_bool(b, out);
        }

        case KDB_TYPE_INT: {
            char *end = NULL;
            errno = 0;
            int64_t v = (int64_t)strtoll(raw, &end, 10);
            if (errno != 0 || (end && *end != '\0' && !isspace((unsigned char)*end))) {
                kdb_err_bad_type("value", KDB_TYPE_INT, KDB_TYPE_UNKNOWN);
                return KDB_ERR_BAD_TYPE;
            }
            return kdb_value_from_int(v, out);
        }

        case KDB_TYPE_FLOAT: {
            char *end = NULL;
            errno = 0;
            double v = strtod(raw, &end);
            if (errno != 0 || (end && *end != '\0' && !isspace((unsigned char)*end))) {
                kdb_err_bad_type("value", KDB_TYPE_FLOAT, KDB_TYPE_UNKNOWN);
                return KDB_ERR_BAD_TYPE;
            }
            return kdb_value_from_float(v, out);
        }

        case KDB_TYPE_STRING: {
            char *copy = kdb__strdup(raw);
            if (!copy) {
                kdb_err_oom("string value");
                return KDB_ERR_OOM;
            }
            out->type            = KDB_TYPE_STRING;
            out->v.as_string.data = copy;
            out->v.as_string.len  = strlen(copy);
            return KDB_OK;
        }

        default:
            kdb_err_bad_type("value", KDB_TYPE_UNKNOWN, KDB_TYPE_UNKNOWN);
            return KDB_ERR_BAD_TYPE;
    }
}


KdbStatus kdb_value_copy(const KdbValue *src, KdbValue *dst) {
    if (!src || !dst) return KDB_ERR_BAD_ARG;
    memcpy(dst, src, sizeof(*dst));

    switch (src->type) {
        case KDB_TYPE_STRING: {
            char *copy = malloc(src->v.as_string.len + 1);
            if (!copy) { kdb_err_oom("string copy"); return KDB_ERR_OOM; }
            memcpy(copy, src->v.as_string.data, src->v.as_string.len + 1);
            dst->v.as_string.data = copy;
            dst->v.as_string.len  = src->v.as_string.len;
            break;
        }
        case KDB_TYPE_BLOB: {
            if (src->v.as_blob.len == 0) {
                dst->v.as_blob.data = NULL;
                dst->v.as_blob.len  = 0;
                break;
            }
            uint8_t *copy = malloc(src->v.as_blob.len);
            if (!copy) { kdb_err_oom("blob copy"); return KDB_ERR_OOM; }
            memcpy(copy, src->v.as_blob.data, src->v.as_blob.len);
            dst->v.as_blob.data = copy;
            dst->v.as_blob.len  = src->v.as_blob.len;
            break;
        }
        case KDB_TYPE_ARRAY: {
            dst->v.as_array.items = NULL;
            dst->v.as_array.count = 0;
            if (src->v.as_array.count == 0) break;
            KdbValue *copy = (KdbValue *)calloc(src->v.as_array.count, sizeof(KdbValue));
            if (!copy) { kdb_err_oom("array copy"); return KDB_ERR_OOM; }
            for (size_t i = 0; i < src->v.as_array.count; i++) {
                if (kdb_value_copy(&src->v.as_array.items[i], &copy[i]) != KDB_OK) {
                    for (size_t j = 0; j < i; j++) kdb_value_free(&copy[j]);
                    free(copy);
                    return KDB_ERR_OOM;
                }
            }
            dst->v.as_array.items = copy;
            dst->v.as_array.count = src->v.as_array.count;
            break;
        }
        case KDB_TYPE_OBJECT: {
            dst->v.as_object.fields = NULL;
            dst->v.as_object.count  = 0;
            if (src->v.as_object.count == 0) break;
            KdbRecordField *copy = (KdbRecordField *)calloc(src->v.as_object.count, sizeof(KdbRecordField));
            if (!copy) { kdb_err_oom("object copy"); return KDB_ERR_OOM; }
            for (uint32_t i = 0; i < src->v.as_object.count; i++) {
                KDB_STRLCPY(copy[i].col_name, src->v.as_object.fields[i].col_name, KDB_MAX_NAME_LEN);
                if (kdb_value_copy(&src->v.as_object.fields[i].value, &copy[i].value) != KDB_OK) {
                    for (uint32_t j = 0; j < i; j++) kdb_value_free(&copy[j].value);
                    free(copy);
                    return KDB_ERR_OOM;
                }
            }
            dst->v.as_object.fields = copy;
            dst->v.as_object.count  = src->v.as_object.count;
            break;
        }
        default:
            break;
    }
    return KDB_OK;
}

void kdb_value_free(KdbValue *v) {
    if (!v) return;
    switch (v->type) {
        case KDB_TYPE_STRING:
            free(v->v.as_string.data);
            v->v.as_string.data = NULL;
            v->v.as_string.len  = 0;
            break;
        case KDB_TYPE_BLOB:
            free(v->v.as_blob.data);
            v->v.as_blob.data = NULL;
            v->v.as_blob.len  = 0;
            break;
        case KDB_TYPE_ARRAY:
            for (size_t i = 0; i < v->v.as_array.count; i++) kdb_value_free(&v->v.as_array.items[i]);
            free(v->v.as_array.items);
            v->v.as_array.items = NULL;
            v->v.as_array.count = 0;
            break;
        case KDB_TYPE_OBJECT:
            for (uint32_t i = 0; i < v->v.as_object.count; i++) kdb_value_free(&v->v.as_object.fields[i].value);
            free(v->v.as_object.fields);
            v->v.as_object.fields = NULL;
            v->v.as_object.count  = 0;
            break;
        default:
            break;
    }
    v->type = KDB_TYPE_NULL;
}


static double kdb__to_double(const KdbValue *v) {
    if (v->type == KDB_TYPE_INT)   return (double)v->v.as_int;
    if (v->type == KDB_TYPE_FLOAT) return v->v.as_float;
    if (v->type == KDB_TYPE_BOOL)  return (double)v->v.as_bool;
    return 0.0;
}

/* Arrays compare lexicographically by element (shorter-is-less on a common
 * prefix), same idea as comparing tuples/lists in most languages -- this
 * gives EQ/NEQ a real answer (not just "incomparable") and GT/LT/etc. a
 * defined, if not hugely useful, ordering. An incomparable element pair
 * deep inside (e.g. int vs string at the same position) makes the whole
 * comparison INT32_MIN, same as any other genuinely incompatible pairing. */
static int kdb__compare_array(const KdbValue *a, const KdbValue *b) {
    size_t na = a->v.as_array.count, nb = b->v.as_array.count;
    size_t n  = na < nb ? na : nb;
    for (size_t i = 0; i < n; i++) {
        int c = kdb_value_compare(&a->v.as_array.items[i], &b->v.as_array.items[i]);
        if (c == INT32_MIN) return INT32_MIN;
        if (c != 0) return c;
    }
    if (na < nb) return -1;
    if (na > nb) return  1;
    return 0;
}

/* Objects compare by (name, value) pairs in stored order -- order-sensitive
 * on purpose, keeps this simple and gives EQ/NEQ a real answer. */
static int kdb__compare_object(const KdbValue *a, const KdbValue *b) {
    uint32_t na = a->v.as_object.count, nb = b->v.as_object.count;
    uint32_t n  = na < nb ? na : nb;
    for (uint32_t i = 0; i < n; i++) {
        int nc = strcmp(a->v.as_object.fields[i].col_name, b->v.as_object.fields[i].col_name);
        if (nc != 0) return nc < 0 ? -1 : 1;
        int c = kdb_value_compare(&a->v.as_object.fields[i].value, &b->v.as_object.fields[i].value);
        if (c == INT32_MIN) return INT32_MIN;
        if (c != 0) return c;
    }
    if (na < nb) return -1;
    if (na > nb) return  1;
    return 0;
}

int kdb_value_compare(const KdbValue *a, const KdbValue *b) {
    if (!a || !b) return INT32_MIN;


    if (a->type == KDB_TYPE_NULL && b->type == KDB_TYPE_NULL) return 0;
    if (a->type == KDB_TYPE_NULL) return -1;
    if (b->type == KDB_TYPE_NULL) return  1;



    int a_num = (a->type == KDB_TYPE_INT || a->type == KDB_TYPE_FLOAT || a->type == KDB_TYPE_BOOL);
    int b_num = (b->type == KDB_TYPE_INT || b->type == KDB_TYPE_FLOAT || b->type == KDB_TYPE_BOOL);
    if (a_num && b_num) {
        double da = kdb__to_double(a);
        double db = kdb__to_double(b);
        if (da < db) return -1;
        if (da > db) return  1;
        return 0;
    }


    if (a->type == KDB_TYPE_STRING && b->type == KDB_TYPE_STRING)
        return strcmp(a->v.as_string.data, b->v.as_string.data);

    if (a->type == KDB_TYPE_ARRAY && b->type == KDB_TYPE_ARRAY)
        return kdb__compare_array(a, b);

    if (a->type == KDB_TYPE_OBJECT && b->type == KDB_TYPE_OBJECT)
        return kdb__compare_object(a, b);


    return INT32_MIN;
}


/* Standard SQL LIKE wildcards: '%' matches any run of characters
 * (including none), '_' matches exactly one. No ESCAPE clause -- there's
 * no way to match a literal '%'/'_' -- same "don't over-build it" call as
 * the rest of this engine's pattern matching. Case-sensitive, same as
 * every other string comparison here. Classic linear-time greedy-with-
 * backtrack algorithm (remember the last '%' and retry from just past it
 * on a mismatch), not recursive -- a pathological pattern can't blow the
 * stack. */
int kdb_like_match(const char *pattern, const char *text) {
    if (!pattern || !text) return 0;
    const char *p = pattern, *t = text;
    const char *star_p = NULL, *star_t = NULL;

    while (*t) {
        if (*p == '_' || *p == *t) { p++; t++; }
        else if (*p == '%') { star_p = p++; star_t = t; }
        else if (star_p) { p = star_p + 1; t = ++star_t; }
        else return 0;
    }
    while (*p == '%') p++;
    return *p == '\0';
}

/* Same algorithm as kdb_like_match, case-insensitively -- a separate
 * small function rather than a flag on kdb_like_match's own signature,
 * since that's public API already in use. */
int kdb_ilike_match(const char *pattern, const char *text) {
    if (!pattern || !text) return 0;
    const char *p = pattern, *t = text;
    const char *star_p = NULL, *star_t = NULL;

    while (*t) {
        if (*p == '_' || tolower((unsigned char)*p) == tolower((unsigned char)*t)) { p++; t++; }
        else if (*p == '%') { star_p = p++; star_t = t; }
        else if (star_p) { p = star_p + 1; t = ++star_t; }
        else return 0;
    }
    while (*p == '%') p++;
    return *p == '\0';
}

/* Whether c matches the atom at *pp (a literal, '.', a backslash-escaped
 * literal, or a [...]/[^...] character class with 'a-z'-style ranges) --
 * always advances *pp past the atom regardless of the match result, so
 * callers that only want to skip an atom (during backtracking) can call
 * this with any character and ignore the return value. */
static int kdb__re_atom_matches(const char **pp, char c) {
    const char *p = *pp;
    if (*p == '.') { *pp = p + 1; return 1; }
    if (*p == '[') {
        p++;
        int negate = 0;
        if (*p == '^') { negate = 1; p++; }
        int matched = 0;
        int first = 1;
        while (*p && (*p != ']' || first)) {
            first = 0;
            if (p[1] == '-' && p[2] && p[2] != ']') {
                if ((unsigned char)c >= (unsigned char)p[0] && (unsigned char)c <= (unsigned char)p[2]) matched = 1;
                p += 3;
            } else if (*p == '\\' && p[1]) {
                if (p[1] == c) matched = 1;
                p += 2;
            } else {
                if (*p == c) matched = 1;
                p++;
            }
        }
        if (*p == ']') p++;
        *pp = p;
        return negate ? !matched : matched;
    }
    if (*p == '\\' && p[1]) {
        int m = (p[1] == c);
        *pp = p + 2;
        return m;
    }
    int m = (*p == c);
    *pp = p + 1;
    return m;
}

static int kdb__re_match_here(const char *pat, const char *text);

/* Greedy repetition (','*'/'+'): consumes as many repetitions of the atom
 * at atom_start as possible, then backtracks one at a time until the
 * rest of the pattern matches or the minimum repeat count is reached. */
static int kdb__re_match_repeat(const char *atom_start, const char *pat_after_atom, const char *text, int min_count) {
    const char *t = text;
    int count = 0;
    for (;;) {
        const char *ap = atom_start;
        if (!*t || !kdb__re_atom_matches(&ap, *t)) break;
        t++;
        count++;
    }
    while (count >= min_count) {
        if (kdb__re_match_here(pat_after_atom, text + count)) return 1;
        count--;
    }
    return 0;
}

static int kdb__re_match_here(const char *pat, const char *text) {
    if (*pat == '\0') return 1;
    if (*pat == '$' && pat[1] == '\0') return *text == '\0';

    const char *atom_start = pat;
    const char *after_atom = pat;
    kdb__re_atom_matches(&after_atom, *text); /* advance-only; result unused here */

    if (*after_atom == '*') return kdb__re_match_repeat(atom_start, after_atom + 1, text, 0);
    if (*after_atom == '+') return kdb__re_match_repeat(atom_start, after_atom + 1, text, 1);
    if (*after_atom == '?') {
        if (*text) {
            const char *ap = atom_start;
            if (kdb__re_atom_matches(&ap, *text) && kdb__re_match_here(after_atom + 1, text + 1)) return 1;
        }
        return kdb__re_match_here(after_atom + 1, text);
    }

    if (*text == '\0') return 0;
    const char *ap = atom_start;
    if (!kdb__re_atom_matches(&ap, *text)) return 0;
    return kdb__re_match_here(after_atom, text + 1);
}

/* A small, self-contained regex matcher -- not a POSIX <regex.h> wrapper,
 * since that header isn't available when cross-compiling for Windows via
 * mingw-w64 (this project stays portable there as a hard requirement).
 * Supports literal characters, '.' (any character), '*'/'+'/'?'
 * (quantifiers on the immediately preceding atom), '[...]'/'[^...]'
 * character classes (with 'a-z'-style ranges), '\' to escape the next
 * character literally, and optional '^'/'$' anchors (unanchored
 * otherwise -- matches anywhere in text, same as SQL's REGEXP
 * convention). Deliberately doesn't support alternation ('|'), groups or
 * backreferences, or bounded repetition ('{n,m}') -- a real regex engine
 * covering those would be a much bigger undertaking than this dialect's
 * pattern-matching needs justify (LIKE's own wildcard matcher makes the
 * same "cover the common case, not the whole spec" call). Backtracking,
 * not NFA-based -- exponential worst case on a pathological pattern, but
 * fine for the short patterns this engine's queries actually use. */
int kdb_regex_match(const char *pattern, const char *text) {
    if (!pattern || !text) return 0;
    if (*pattern == '^') return kdb__re_match_here(pattern + 1, text);
    const char *t = text;
    do {
        if (kdb__re_match_here(pattern, t)) return 1;
    } while (*t++);
    return 0;
}

int kdb_value_matches(const KdbValue *field,
                      KdbOperator     op,
                      const KdbValue *fv,
                      const KdbValue *fv2) {
    if (!field) return 0;

    switch (op) {
        case KDB_OP_IS_NULL:
            return field->type == KDB_TYPE_NULL;

        case KDB_OP_IS_NOT_NULL:
            return field->type != KDB_TYPE_NULL;

        case KDB_OP_EQ: {
            int cmp = kdb_value_compare(field, fv);
            return cmp != INT32_MIN && cmp == 0;
        }
        case KDB_OP_NEQ: {
            int cmp = kdb_value_compare(field, fv);
            return cmp != INT32_MIN && cmp != 0;
        }
        case KDB_OP_GT: {
            int cmp = kdb_value_compare(field, fv);
            return cmp != INT32_MIN && cmp > 0;
        }
        case KDB_OP_GTE: {
            int cmp = kdb_value_compare(field, fv);
            return cmp != INT32_MIN && cmp >= 0;
        }
        case KDB_OP_LT: {
            int cmp = kdb_value_compare(field, fv);
            return cmp != INT32_MIN && cmp < 0;
        }
        case KDB_OP_LTE: {
            int cmp = kdb_value_compare(field, fv);
            return cmp != INT32_MIN && cmp <= 0;
        }
        case KDB_OP_CONTAINS:
            if (field->type != KDB_TYPE_STRING || fv->type != KDB_TYPE_STRING)
                return 0;
            return strstr(field->v.as_string.data, fv->v.as_string.data) != NULL;

        case KDB_OP_STARTSWITH:
            if (field->type != KDB_TYPE_STRING || fv->type != KDB_TYPE_STRING)
                return 0;
            return strncmp(field->v.as_string.data,
                           fv->v.as_string.data,
                           fv->v.as_string.len) == 0;

        case KDB_OP_ENDSWITH: {
            if (field->type != KDB_TYPE_STRING || fv->type != KDB_TYPE_STRING)
                return 0;
            size_t flen = field->v.as_string.len;
            size_t slen = fv->v.as_string.len;
            if (slen > flen) return 0;
            return strcmp(field->v.as_string.data + (flen - slen),
                          fv->v.as_string.data) == 0;
        }

        case KDB_OP_BETWEEN: {
            if (!fv2) return 0;
            int lo = kdb_value_compare(field, fv);
            int hi = kdb_value_compare(field, fv2);
            return (lo != INT32_MIN && hi != INT32_MIN && lo >= 0 && hi <= 0);
        }

        case KDB_OP_IN: {
            if (!fv || fv->type != KDB_TYPE_STRING || !fv->v.as_string.data) return 0;

            const char *p = fv->v.as_string.data;
            while (*p) {
                const char *comma = strchr(p, ',');
                size_t len = comma ? (size_t)(comma - p) : strlen(p);

                while (len > 0 && isspace((unsigned char)*p)) { p++; len--; }
                while (len > 0 && isspace((unsigned char)p[len - 1])) len--;

                if (len > 0) {
                    char token[256];
                    size_t tlen = len < sizeof(token) - 1 ? len : sizeof(token) - 1;
                    memcpy(token, p, tlen);
                    token[tlen] = '\0';

                    KdbValue tv;
                    if (kdb_value_from_string(token, kdb_type_infer(token), &tv) == KDB_OK) {
                        int cmp = kdb_value_compare(field, &tv);
                        kdb_value_free(&tv);
                        if (cmp == 0) return 1;
                    }
                }

                if (!comma) break;
                p = comma + 1;
            }
            return 0;
        }

        case KDB_OP_LIKE:
            if (field->type != KDB_TYPE_STRING || !fv || fv->type != KDB_TYPE_STRING)
                return 0;
            return kdb_like_match(fv->v.as_string.data, field->v.as_string.data);

        case KDB_OP_ILIKE:
            if (field->type != KDB_TYPE_STRING || !fv || fv->type != KDB_TYPE_STRING)
                return 0;
            return kdb_ilike_match(fv->v.as_string.data, field->v.as_string.data);

        case KDB_OP_REGEXP:
            if (field->type != KDB_TYPE_STRING || !fv || fv->type != KDB_TYPE_STRING)
                return 0;
            return kdb_regex_match(fv->v.as_string.data, field->v.as_string.data);

        default:
            return 0;
    }
}


const char *kdb_type_name(KdbType type) {
    switch (type) {
        case KDB_TYPE_NULL:    return "null";
        case KDB_TYPE_INT:     return "int";
        case KDB_TYPE_FLOAT:   return "float";
        case KDB_TYPE_BOOL:    return "bool";
        case KDB_TYPE_STRING:  return "string";
        case KDB_TYPE_BLOB:    return "blob";
        case KDB_TYPE_ARRAY:   return "array";
        case KDB_TYPE_OBJECT:  return "object";
        default:               return "unknown";
    }
}

const char *kdb_op_name(KdbOperator op) {
    switch (op) {
        case KDB_OP_EQ:          return "eq";
        case KDB_OP_NEQ:         return "neq";
        case KDB_OP_GT:          return "gt";
        case KDB_OP_GTE:         return "gte";
        case KDB_OP_LT:          return "lt";
        case KDB_OP_LTE:         return "lte";
        case KDB_OP_CONTAINS:    return "contains";
        case KDB_OP_STARTSWITH:  return "startswith";
        case KDB_OP_ENDSWITH:    return "endswith";
        case KDB_OP_IN:          return "in";
        case KDB_OP_BETWEEN:     return "between";
        case KDB_OP_IS_NULL:     return "isnull";
        case KDB_OP_IS_NOT_NULL: return "isnotnull";
        case KDB_OP_LIKE:        return "like";
        case KDB_OP_ILIKE:       return "ilike";
        case KDB_OP_REGEXP:      return "regexp";
        default:                 return "unknown";
    }
}


KdbStatus kdb_parse_filter_key(const char  *key,
                               char         col_name_out[KDB_MAX_NAME_LEN],
                               KdbOperator *op_out) {
    if (!key || !col_name_out || !op_out) {
        kdb_err_null_arg("key/col_name_out/op_out", "kdb_parse_filter_key");
        return KDB_ERR_BAD_ARG;
    }

    
    const char *sep = strstr(key, "__");
    if (!sep) {
        
        KDB_STRLCPY(col_name_out, key, KDB_MAX_NAME_LEN);
        *op_out = KDB_OP_EQ;
        return KDB_OK;
    }

    
    size_t col_len = (size_t)(sep - key);
    if (col_len == 0 || col_len >= KDB_MAX_NAME_LEN) {
        kdb_err_bad_filter(key, "column name is empty or too long");
        return KDB_ERR_BAD_FILTER;
    }
    memcpy(col_name_out, key, col_len);
    col_name_out[col_len] = '\0';

    
    const char *op_str = sep + 2;

    if (strcmp(op_str, "eq")          == 0) { *op_out = KDB_OP_EQ;          return KDB_OK; }
    if (strcmp(op_str, "neq")         == 0) { *op_out = KDB_OP_NEQ;         return KDB_OK; }
    if (strcmp(op_str, "gt")          == 0) { *op_out = KDB_OP_GT;          return KDB_OK; }
    if (strcmp(op_str, "gte")         == 0) { *op_out = KDB_OP_GTE;         return KDB_OK; }
    if (strcmp(op_str, "lt")          == 0) { *op_out = KDB_OP_LT;          return KDB_OK; }
    if (strcmp(op_str, "lte")         == 0) { *op_out = KDB_OP_LTE;         return KDB_OK; }
    if (strcmp(op_str, "contains")    == 0) { *op_out = KDB_OP_CONTAINS;    return KDB_OK; }
    if (strcmp(op_str, "startswith")  == 0) { *op_out = KDB_OP_STARTSWITH;  return KDB_OK; }
    if (strcmp(op_str, "endswith")    == 0) { *op_out = KDB_OP_ENDSWITH;    return KDB_OK; }
    if (strcmp(op_str, "in")          == 0) { *op_out = KDB_OP_IN;          return KDB_OK; }
    if (strcmp(op_str, "between")     == 0) { *op_out = KDB_OP_BETWEEN;     return KDB_OK; }
    if (strcmp(op_str, "isnull")      == 0) { *op_out = KDB_OP_IS_NULL;     return KDB_OK; }
    if (strcmp(op_str, "isnotnull")   == 0) { *op_out = KDB_OP_IS_NOT_NULL; return KDB_OK; }
    if (strcmp(op_str, "like")        == 0) { *op_out = KDB_OP_LIKE;        return KDB_OK; }
    if (strcmp(op_str, "ilike")       == 0) { *op_out = KDB_OP_ILIKE;       return KDB_OK; }
    if (strcmp(op_str, "regexp")      == 0) { *op_out = KDB_OP_REGEXP;      return KDB_OK; }

    kdb_err_bad_filter(key, "unknown operator suffix — valid: eq, neq, gt, gte, lt, lte, "
                            "contains, startswith, endswith, in, between, isnull, isnotnull, like, "
                            "ilike, regexp");
    return KDB_ERR_BAD_FILTER;
}


/* Appends to buf at pos (bounded, always leaves buf nul-terminated within
 * buf_size) and returns the new pos. Safe to keep calling even after the
 * buffer fills up -- just stops writing, like snprintf itself. */
static size_t kdb__append(char *buf, size_t buf_size, size_t pos, const char *fmt, ...) {
    if (pos >= buf_size) return pos;
    va_list args;
    va_start(args, fmt);
    int w = vsnprintf(buf + pos, buf_size - pos, fmt, args);
    va_end(args);
    if (w <= 0) return pos;
    size_t avail   = buf_size - pos - 1; /* excluding the nul terminator slot */
    size_t written = (size_t)w < avail ? (size_t)w : avail;
    return pos + written;
}

int kdb_value_to_str(const KdbValue *v, char *buf, size_t buf_size) {
    if (!v || !buf || buf_size == 0) return 0;

    switch (v->type) {
        case KDB_TYPE_NULL:
            return snprintf(buf, buf_size, "null");
        case KDB_TYPE_INT:
            return snprintf(buf, buf_size, "%lld", (long long)v->v.as_int);
        case KDB_TYPE_FLOAT:
            return snprintf(buf, buf_size, "%g", v->v.as_float);
        case KDB_TYPE_BOOL:
            return snprintf(buf, buf_size, "%s", v->v.as_bool ? "true" : "false");
        case KDB_TYPE_STRING:
            return snprintf(buf, buf_size, "\"%s\"", v->v.as_string.data ? v->v.as_string.data : "");
        case KDB_TYPE_BLOB:
            return snprintf(buf, buf_size, "<blob:%zu bytes>", v->v.as_blob.len);
        case KDB_TYPE_ARRAY: {
            size_t pos = kdb__append(buf, buf_size, 0, "[");
            for (size_t i = 0; i < v->v.as_array.count; i++) {
                char elem[128];
                kdb_value_to_str(&v->v.as_array.items[i], elem, sizeof(elem));
                pos = kdb__append(buf, buf_size, pos, "%s%s", i > 0 ? ", " : "", elem);
            }
            pos = kdb__append(buf, buf_size, pos, "]");
            return (int)pos;
        }
        case KDB_TYPE_OBJECT: {
            size_t pos = kdb__append(buf, buf_size, 0, "{");
            for (uint32_t i = 0; i < v->v.as_object.count; i++) {
                char elem[128];
                kdb_value_to_str(&v->v.as_object.fields[i].value, elem, sizeof(elem));
                pos = kdb__append(buf, buf_size, pos, "%s%s: %s",
                                  i > 0 ? ", " : "", v->v.as_object.fields[i].col_name, elem);
            }
            pos = kdb__append(buf, buf_size, pos, "}");
            return (int)pos;
        }
        default:
            return snprintf(buf, buf_size, "<unknown>");
    }
}


KdbStatus kdb_value_cast(const KdbValue *src, KdbType target_type, KdbValue *dst) {
    if (!src || !dst) return KDB_ERR_BAD_ARG;

    
    if (src->type == target_type) return kdb_value_copy(src, dst);

    
    if (src->type == KDB_TYPE_NULL) {
        memset(dst, 0, sizeof(*dst));
        dst->type = target_type;
        return KDB_OK;
    }

    switch (target_type) {
        case KDB_TYPE_INT:
            if (src->type == KDB_TYPE_FLOAT) {
                return kdb_value_from_int((int64_t)src->v.as_float, dst);
            }
            if (src->type == KDB_TYPE_BOOL) {
                return kdb_value_from_int((int64_t)src->v.as_bool, dst);
            }
            if (src->type == KDB_TYPE_STRING && kdb_str_is_int(src->v.as_string.data)) {
                return kdb_value_from_string(src->v.as_string.data, KDB_TYPE_INT, dst);
            }
            break;

        case KDB_TYPE_FLOAT:
            if (src->type == KDB_TYPE_INT) {
                return kdb_value_from_float((double)src->v.as_int, dst);
            }
            if (src->type == KDB_TYPE_BOOL) {
                return kdb_value_from_float((double)src->v.as_bool, dst);
            }
            if (src->type == KDB_TYPE_STRING && kdb_str_is_float(src->v.as_string.data)) {
                return kdb_value_from_string(src->v.as_string.data, KDB_TYPE_FLOAT, dst);
            }
            break;

        case KDB_TYPE_BOOL:
            if (src->type == KDB_TYPE_INT) {
                return kdb_value_from_bool(src->v.as_int != 0 ? 1 : 0, dst);
            }
            if (src->type == KDB_TYPE_STRING && kdb_str_is_bool(src->v.as_string.data)) {
                return kdb_value_from_string(src->v.as_string.data, KDB_TYPE_BOOL, dst);
            }
            break;

        case KDB_TYPE_STRING: {
            char buf[64];
            kdb_value_to_str(src, buf, sizeof(buf));
            return kdb_value_from_string(buf, KDB_TYPE_STRING, dst);
        }

        case KDB_TYPE_NULL:
            return kdb_value_from_null(dst);

        default:
            break;
    }

    kdb_err_bad_type("cast", target_type, src->type);
    return KDB_ERR_BAD_TYPE;
}