#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

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

    
    return INT32_MIN;
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

    kdb_err_bad_filter(key, "unknown operator suffix — valid: eq, neq, gt, gte, lt, lte, "
                            "contains, startswith, endswith, in, between, isnull, isnotnull");
    return KDB_ERR_BAD_FILTER;
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