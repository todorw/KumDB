#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#include "../include/sql.h"
#include "../include/error.h"

#define KDB_SQL_TOK_MAX      2200
#define KDB_SQL_MAX_COLUMNS  64
#define KDB_SQL_MAX_COND     32
#define KDB_SQL_IDENT_BUF    256

typedef enum {
    SQLTOK_EOF, SQLTOK_IDENT, SQLTOK_NUMBER, SQLTOK_STRING,
    SQLTOK_LPAREN, SQLTOK_RPAREN, SQLTOK_COMMA, SQLTOK_STAR, SQLTOK_SEMI,
    SQLTOK_EQ, SQLTOK_NEQ, SQLTOK_LT, SQLTOK_LTE, SQLTOK_GT, SQLTOK_GTE,
    SQLTOK_ERROR
} SqlTokType;

typedef struct {
    SqlTokType type;
    char       text[KDB_SQL_TOK_MAX];
} SqlToken;

typedef struct {
    const char *src;
    size_t      pos;
} SqlLexer;

typedef struct {
    SqlLexer lx;
    SqlToken cur;
} SqlParser;

/* Forward declaration: WHERE/HAVING condition parsing needs to be able to
 * run a full SELECT for scalar/IN subqueries, but SELECT parsing (further
 * down this file) needs WHERE/HAVING parsing first -- there's no way to
 * order these top-to-bottom without one forward declaration somewhere. */
static KdbStatus sql__exec_select_stmt(SqlParser *p, KumDB *db, KdbRows **rows_out);

static KdbStatus sql__err(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    kdb_set_error(KDB_ERR_SQL_SYNTAX, "SQL error: %s", buf);
    return KDB_ERR_SQL_SYNTAX;
}

static void sql__skip_ws(SqlLexer *lx) {
    for (;;) {
        while (lx->src[lx->pos] && isspace((unsigned char)lx->src[lx->pos])) lx->pos++;
        if (lx->src[lx->pos] == '-' && lx->src[lx->pos + 1] == '-') {
            while (lx->src[lx->pos] && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        break;
    }
}

static SqlToken sql__lex_next(SqlLexer *lx) {
    SqlToken t;
    memset(&t, 0, sizeof(t));
    sql__skip_ws(lx);
    const char *s = lx->src;
    size_t i = lx->pos;
    char c = s[i];

    if (!c) { t.type = SQLTOK_EOF; return t; }

    if (c == '(') { t.type = SQLTOK_LPAREN; lx->pos = i + 1; return t; }
    if (c == ')') { t.type = SQLTOK_RPAREN; lx->pos = i + 1; return t; }
    if (c == ',') { t.type = SQLTOK_COMMA;  lx->pos = i + 1; return t; }
    if (c == '*') { t.type = SQLTOK_STAR;   lx->pos = i + 1; return t; }
    if (c == ';') { t.type = SQLTOK_SEMI;   lx->pos = i + 1; return t; }
    if (c == '!' && s[i + 1] == '=') { t.type = SQLTOK_NEQ; lx->pos = i + 2; return t; }
    if (c == '<' && s[i + 1] == '>') { t.type = SQLTOK_NEQ; lx->pos = i + 2; return t; }
    if (c == '<' && s[i + 1] == '=') { t.type = SQLTOK_LTE; lx->pos = i + 2; return t; }
    if (c == '>' && s[i + 1] == '=') { t.type = SQLTOK_GTE; lx->pos = i + 2; return t; }
    if (c == '=') { t.type = SQLTOK_EQ;  lx->pos = i + 1; return t; }
    if (c == '<') { t.type = SQLTOK_LT;  lx->pos = i + 1; return t; }
    if (c == '>') { t.type = SQLTOK_GT;  lx->pos = i + 1; return t; }

    if (c == '\'' || c == '"') {
        char quote = c;
        size_t j = i + 1;
        size_t bi = 0;
        while (s[j] && bi + 1 < sizeof(t.text)) {
            if (s[j] == quote) {
                if (s[j + 1] == quote) { t.text[bi++] = quote; j += 2; continue; }
                break;
            }
            t.text[bi++] = s[j++];
        }
        t.text[bi] = '\0';
        if (s[j] != quote) {
            t.type = SQLTOK_ERROR;
            lx->pos = j;
            return t;
        }
        t.type = SQLTOK_STRING;
        lx->pos = j + 1;
        return t;
    }

    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)s[i + 1]))) {
        size_t j = i;
        if (s[j] == '-') j++;
        while (isdigit((unsigned char)s[j])) j++;
        if (s[j] == '.') { j++; while (isdigit((unsigned char)s[j])) j++; }
        size_t len = j - i;
        if (len >= sizeof(t.text)) len = sizeof(t.text) - 1;
        memcpy(t.text, s + i, len);
        t.text[len] = '\0';
        t.type = SQLTOK_NUMBER;
        lx->pos = j;
        return t;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t j = i;
        /* "t1.name" lexes as one identifier token (table-qualified column
         * reference, for JOIN) -- but only when the '.' is actually
         * followed by another identifier char, so a stray trailing '.'
         * doesn't get silently swallowed into the token. */
        while (isalnum((unsigned char)s[j]) || s[j] == '_' ||
               (s[j] == '.' && (isalpha((unsigned char)s[j + 1]) || s[j + 1] == '_')))
            j++;
        size_t len = j - i;
        if (len >= sizeof(t.text)) len = sizeof(t.text) - 1;
        memcpy(t.text, s + i, len);
        t.text[len] = '\0';
        t.type = SQLTOK_IDENT;
        lx->pos = j;
        return t;
    }

    t.type = SQLTOK_ERROR;
    lx->pos = i + 1;
    return t;
}

static void sql__advance(SqlParser *p) { p->cur = sql__lex_next(&p->lx); }

static void sql__init(SqlParser *p, const char *src) {
    memset(p, 0, sizeof(*p));
    p->lx.src = src;
    p->lx.pos = 0;
    sql__advance(p);
}

static int sql__kw_is(const SqlToken *t, const char *kw) {
    return t->type == SQLTOK_IDENT && strcasecmp(t->text, kw) == 0;
}

static int sql__ident_text(const SqlToken *t, const char **out) {
    if (t->type == SQLTOK_IDENT || t->type == SQLTOK_STRING) { *out = t->text; return 1; }
    return 0;
}

static int sql__value_to_field(const char *col, const SqlToken *t, KdbField *out) {
    switch (t->type) {
        case SQLTOK_NUMBER:
            if (strchr(t->text, '.')) *out = kdb_field_float(col, atof(t->text));
            else                      *out = kdb_field_int(col, atoll(t->text));
            return 1;
        case SQLTOK_STRING:
            *out = kdb_field_string(col, t->text);
            return 1;
        case SQLTOK_IDENT:
            if (strcasecmp(t->text, "true")  == 0) { *out = kdb_field_bool(col, 1); return 1; }
            if (strcasecmp(t->text, "false") == 0) { *out = kdb_field_bool(col, 0); return 1; }
            if (strcasecmp(t->text, "null")  == 0) { *out = kdb_field_null(col); return 1; }
            return 0;
        default:
            return 0;
    }
}

static char *sql__value_text(const SqlToken *t) {
    if (t->type == SQLTOK_NUMBER) return strdup(t->text);
    if (t->type == SQLTOK_STRING) return strdup(t->text);
    if (t->type == SQLTOK_IDENT) {
        if (strcasecmp(t->text, "true")  == 0) return strdup("true");
        if (strcasecmp(t->text, "false") == 0) return strdup("false");
    }
    return NULL;
}

static void sql__free_filters(char **filters, int count) {
    for (int i = 0; i < count; i++) free(filters[i]);
}

/* Renders a field's value as plain filter-value text, the same shape a
 * literal token would produce (no quotes, "%g" for floats -- matches
 * kdb_value_to_str's own float format elsewhere in the engine). NULL/BLOB/
 * ARRAY/OBJECT aren't meaningful scalar filter values, so those return 0. */
static int sql__field_to_filter_text(const KdbField *f, char *buf, size_t buf_size) {
    switch (f->type) {
        case KDB_TYPE_INT:    snprintf(buf, buf_size, "%lld", (long long)f->v.as_int); return 1;
        case KDB_TYPE_FLOAT:  snprintf(buf, buf_size, "%g", f->v.as_float); return 1;
        case KDB_TYPE_BOOL:   snprintf(buf, buf_size, "%s", f->v.as_bool ? "true" : "false"); return 1;
        case KDB_TYPE_STRING: snprintf(buf, buf_size, "%s", f->v.as_string ? f->v.as_string : ""); return 1;
        default: return 0;
    }
}

/* Executes "(SELECT ...)" as a scalar subquery -- p must be positioned
 * right after the '('. Non-correlated only: the inner query can't see the
 * outer row, it just runs once, up front, same as any other SELECT. Must
 * return exactly one row with exactly one column; that value becomes the
 * comparison's right-hand side, same text shape as a literal. Leaves the
 * parser positioned right after the closing ')'. Returns NULL on error
 * (error already set). */
static char *sql__parse_scalar_subquery(SqlParser *p, KumDB *db, const char *col_ctx) {
    if (!sql__kw_is(&p->cur, "SELECT")) { sql__err("expected SELECT after '(' for '%s'", col_ctx); return NULL; }

    KdbRows *sub = NULL;
    if (sql__exec_select_stmt(p, db, &sub) != KDB_OK) return NULL;

    if (p->cur.type != SQLTOK_RPAREN) {
        kdb_rows_free(sub);
        sql__err("expected ')' closing the subquery for '%s'", col_ctx);
        return NULL;
    }
    sql__advance(p);

    if (sub->count != 1 || sub->rows[0].field_count != 1) {
        size_t rc = sub->count;
        size_t fc = sub->count > 0 ? sub->rows[0].field_count : 0;
        kdb_rows_free(sub);
        sql__err("scalar subquery for '%s' must return exactly one row and one column (got %zu row(s), %zu column(s))",
                 col_ctx, rc, fc);
        return NULL;
    }

    char valbuf[KDB_SQL_IDENT_BUF];
    if (!sql__field_to_filter_text(&sub->rows[0].fields[0], valbuf, sizeof(valbuf))) {
        kdb_rows_free(sub);
        sql__err("scalar subquery for '%s' returned a value that can't be used in a comparison", col_ctx);
        return NULL;
    }
    kdb_rows_free(sub);
    return strdup(valbuf);
}

/* Executes "(SELECT ...)" as the right-hand side of IN -- p must be
 * positioned right after the '('. Same non-correlated rule as the scalar
 * case, but any number of rows, each contributing one value to the IN
 * list; still exactly one column per row. Leaves the parser positioned
 * right after the closing ')'. Returns NULL on error (error already set). */
static char *sql__parse_in_subquery(SqlParser *p, KumDB *db, const char *col_ctx) {
    if (!sql__kw_is(&p->cur, "SELECT")) { sql__err("expected SELECT after '(' for '%s'", col_ctx); return NULL; }

    KdbRows *sub = NULL;
    if (sql__exec_select_stmt(p, db, &sub) != KDB_OK) return NULL;

    if (p->cur.type != SQLTOK_RPAREN) {
        kdb_rows_free(sub);
        sql__err("expected ')' closing the subquery for '%s'", col_ctx);
        return NULL;
    }
    sql__advance(p);

    char list[KDB_SQL_TOK_MAX];
    size_t list_len = 0;
    list[0] = '\0';

    for (size_t i = 0; i < sub->count; i++) {
        if (sub->rows[i].field_count != 1) {
            size_t fc = sub->rows[i].field_count;
            kdb_rows_free(sub);
            sql__err("IN subquery for '%s' must return exactly one column (got %zu)", col_ctx, fc);
            return NULL;
        }
        char valbuf[KDB_SQL_IDENT_BUF];
        if (!sql__field_to_filter_text(&sub->rows[i].fields[0], valbuf, sizeof(valbuf))) {
            kdb_rows_free(sub);
            sql__err("IN subquery for '%s' returned a value that can't be used in a comparison", col_ctx);
            return NULL;
        }
        size_t vlen = strlen(valbuf);
        size_t need = list_len + (i > 0 ? 1 : 0) + vlen;
        if (need >= sizeof(list)) {
            kdb_rows_free(sub);
            sql__err("IN subquery result too long for '%s'", col_ctx);
            return NULL;
        }
        if (i > 0) list[list_len++] = ',';
        memcpy(list + list_len, valbuf, vlen);
        list_len += vlen;
        list[list_len] = '\0';
    }
    kdb_rows_free(sub);
    return strdup(list);
}

static char *sql__parse_condition(SqlParser *p, KumDB *db) {
    const char *col;
    if (!sql__ident_text(&p->cur, &col)) { sql__err("expected a column name in WHERE clause"); return NULL; }
    char col_buf[KDB_SQL_IDENT_BUF];
    snprintf(col_buf, sizeof(col_buf), "%.255s", col);
    sql__advance(p);

    if (sql__kw_is(&p->cur, "IS")) {
        sql__advance(p);
        int negate = 0;
        if (sql__kw_is(&p->cur, "NOT")) { negate = 1; sql__advance(p); }
        if (!sql__kw_is(&p->cur, "NULL")) { sql__err("expected NULL after IS [NOT] on '%s'", col_buf); return NULL; }
        sql__advance(p);
        char *buf = malloc(strlen(col_buf) + 16);
        if (buf) sprintf(buf, "%s__%s", col_buf, negate ? "isnotnull" : "isnull");
        return buf;
    }

    if (sql__kw_is(&p->cur, "BETWEEN")) {
        sql__advance(p);
        char *lo_text = sql__value_text(&p->cur);
        if (!lo_text) { sql__err("expected a value after BETWEEN on '%s'", col_buf); return NULL; }
        sql__advance(p);
        if (!sql__kw_is(&p->cur, "AND")) {
            sql__err("expected AND in BETWEEN ... AND ... on '%s'", col_buf);
            free(lo_text);
            return NULL;
        }
        sql__advance(p);
        char *hi_text = sql__value_text(&p->cur);
        if (!hi_text) {
            sql__err("expected a value after BETWEEN ... AND on '%s'", col_buf);
            free(lo_text);
            return NULL;
        }
        sql__advance(p);
        size_t need = strlen(col_buf) + strlen(lo_text) + strlen(hi_text) + 20;
        char *buf = malloc(need);
        if (buf) snprintf(buf, need, "%s__between=%s,%s", col_buf, lo_text, hi_text);
        free(lo_text);
        free(hi_text);
        return buf;
    }

    if (sql__kw_is(&p->cur, "IN")) {
        sql__advance(p);
        if (p->cur.type != SQLTOK_LPAREN) { sql__err("expected '(' after IN on '%s'", col_buf); return NULL; }
        sql__advance(p);

        if (sql__kw_is(&p->cur, "SELECT")) {
            char *list_text = sql__parse_in_subquery(p, db, col_buf);
            if (!list_text) return NULL;
            size_t need = strlen(col_buf) + strlen(list_text) + 8;
            char *buf = malloc(need);
            if (buf) snprintf(buf, need, "%s__in=%s", col_buf, list_text);
            free(list_text);
            return buf;
        }

        char list[KDB_SQL_TOK_MAX];
        size_t list_len = 0;
        list[0] = '\0';
        int n = 0;

        for (;;) {
            char *val_text = sql__value_text(&p->cur);
            if (!val_text) { sql__err("expected a value in the IN list for '%s'", col_buf); return NULL; }
            sql__advance(p);

            size_t vlen = strlen(val_text);
            size_t need = list_len + (n > 0 ? 1 : 0) + vlen;
            if (need >= sizeof(list)) {
                sql__err("IN list too long for '%s'", col_buf);
                free(val_text);
                return NULL;
            }
            if (n > 0) list[list_len++] = ',';
            memcpy(list + list_len, val_text, vlen);
            list_len += vlen;
            list[list_len] = '\0';
            free(val_text);
            n++;

            if (p->cur.type == SQLTOK_COMMA) { sql__advance(p); continue; }
            break;
        }
        if (p->cur.type != SQLTOK_RPAREN) { sql__err("expected ')' closing the IN list for '%s'", col_buf); return NULL; }
        sql__advance(p);

        size_t need = strlen(col_buf) + list_len + 8;
        char *buf = malloc(need);
        if (buf) snprintf(buf, need, "%s__in=%s", col_buf, list);
        return buf;
    }

    if (sql__kw_is(&p->cur, "LIKE")) {
        sql__advance(p);
        if (p->cur.type != SQLTOK_STRING) { sql__err("expected a string pattern after LIKE on '%s'", col_buf); return NULL; }
        const char *pat = p->cur.text;
        size_t plen = strlen(pat);
        int lead  = plen > 0 && pat[0] == '%';
        int trail = plen > 0 && pat[plen - 1] == '%';
        size_t start = lead ? 1 : 0;
        size_t end   = plen - (trail ? 1 : 0);
        if (end < start) end = start;
        size_t slen = end - start;

        char stripped[KDB_SQL_TOK_MAX];
        if (slen >= sizeof(stripped)) slen = sizeof(stripped) - 1;
        memcpy(stripped, pat + start, slen);
        stripped[slen] = '\0';

        if (strchr(stripped, '%') || strchr(stripped, '_')) {
            sql__err("LIKE only supports a leading and/or trailing %% on '%s' -- no mid-pattern wildcards", col_buf);
            return NULL;
        }
        const char *suffix = (lead && trail) ? "contains" : (trail ? "startswith" : (lead ? "endswith" : "eq"));
        sql__advance(p);
        size_t need = strlen(col_buf) + strlen(suffix) + slen + 8;
        char *buf = malloc(need);
        if (buf) snprintf(buf, need, "%s__%s=%s", col_buf, suffix, stripped);
        return buf;
    }

    const char *suffix = NULL;
    switch (p->cur.type) {
        case SQLTOK_EQ:  suffix = "";    break;
        case SQLTOK_NEQ: suffix = "neq"; break;
        case SQLTOK_GT:  suffix = "gt";  break;
        case SQLTOK_GTE: suffix = "gte"; break;
        case SQLTOK_LT:  suffix = "lt";  break;
        case SQLTOK_LTE: suffix = "lte"; break;
        default:
            sql__err("expected a comparison operator, BETWEEN, LIKE, or IS [NOT] NULL after '%s'", col_buf);
            return NULL;
    }
    sql__advance(p);

    if (p->cur.type == SQLTOK_IDENT && strcasecmp(p->cur.text, "null") == 0) {
        sql__err("use IS [NOT] NULL to compare '%s' against NULL, not =/!=", col_buf);
        return NULL;
    }

    char *val_text;
    if (p->cur.type == SQLTOK_LPAREN) {
        sql__advance(p);
        val_text = sql__parse_scalar_subquery(p, db, col_buf);
        if (!val_text) return NULL;
    } else {
        val_text = sql__value_text(&p->cur);
        if (!val_text) { sql__err("expected a value after the comparison operator on '%s'", col_buf); return NULL; }
        sql__advance(p);
    }

    size_t need = strlen(col_buf) + strlen(suffix) + strlen(val_text) + 8;
    char *buf = malloc(need);
    if (buf) {
        if (suffix[0]) snprintf(buf, need, "%s__%s=%s", col_buf, suffix, val_text);
        else           snprintf(buf, need, "%s=%s", col_buf, val_text);
    }
    free(val_text);
    return buf;
}

/* AND binds tighter than OR, same as standard SQL -- no parens/nesting.
 * "a=1 AND b=2 OR c=3" means (a=1 AND b=2) OR (c=3). Groups get built by
 * prefixing the first condition of each OR'd group with "OR:", the same
 * convention kdb_find()/kdb_update()/kdb_delete() understand. Shared by
 * WHERE and HAVING -- the keyword itself is consumed by the caller.
 * returns 0 on error (error already set), 1 on success */
static int sql__parse_cond_list(SqlParser *p, KumDB *db, char *filters_buf[KDB_SQL_MAX_COND], int *count_out) {
    int n = 0;
    *count_out = 0;
    int start_new_group = 0;
    for (;;) {
        if (n >= KDB_SQL_MAX_COND) {
            sql__err("too many conditions (max %d)", KDB_SQL_MAX_COND);
            sql__free_filters(filters_buf, n);
            return 0;
        }
        char *f = sql__parse_condition(p, db);
        if (!f) { sql__free_filters(filters_buf, n); return 0; }

        if (start_new_group) {
            size_t need = strlen(f) + 4;
            char *prefixed = malloc(need);
            if (!prefixed) {
                kdb_err_oom("OR-prefixed filter string");
                free(f);
                sql__free_filters(filters_buf, n);
                return 0;
            }
            snprintf(prefixed, need, "OR:%s", f);
            free(f);
            f = prefixed;
            start_new_group = 0;
        }
        filters_buf[n++] = f;

        if (sql__kw_is(&p->cur, "OR")) {
            sql__advance(p);
            start_new_group = 1;
            continue;
        }
        if (sql__kw_is(&p->cur, "AND")) { sql__advance(p); continue; }
        break;
    }
    *count_out = n;
    return 1;
}

/* returns 0 on error (error already set), 1 on success */
static int sql__parse_where(SqlParser *p, KumDB *db, char *filters_buf[KDB_SQL_MAX_COND], int *count_out) {
    *count_out = 0;
    if (!sql__kw_is(&p->cur, "WHERE")) return 1;
    sql__advance(p);
    return sql__parse_cond_list(p, db, filters_buf, count_out);
}

/* returns 0 on error (error already set), 1 on success */
static int sql__parse_having(SqlParser *p, KumDB *db, char *filters_buf[KDB_SQL_MAX_COND], int *count_out) {
    *count_out = 0;
    if (!sql__kw_is(&p->cur, "HAVING")) return 1;
    sql__advance(p);
    return sql__parse_cond_list(p, db, filters_buf, count_out);
}

static int sql__is_reserved_column(const char *name) {
    return strcasecmp(name, "id") == 0 ||
           strcasecmp(name, "created_at") == 0 ||
           strcasecmp(name, "updated_at") == 0;
}

static KdbStatus sql__type_from_ident(const char *s, KdbFieldType *out) {
    if (strcasecmp(s, "INT") == 0 || strcasecmp(s, "INTEGER") == 0) { *out = KDB_TYPE_INT; return KDB_OK; }
    if (strcasecmp(s, "FLOAT") == 0 || strcasecmp(s, "REAL") == 0 || strcasecmp(s, "DOUBLE") == 0) { *out = KDB_TYPE_FLOAT; return KDB_OK; }
    if (strcasecmp(s, "BOOL") == 0 || strcasecmp(s, "BOOLEAN") == 0) { *out = KDB_TYPE_BOOL; return KDB_OK; }
    if (strcasecmp(s, "TEXT") == 0 || strcasecmp(s, "STRING") == 0 ||
        strcasecmp(s, "VARCHAR") == 0 || strcasecmp(s, "CHAR") == 0) { *out = KDB_TYPE_STRING; return KDB_OK; }
    if (strcasecmp(s, "BLOB") == 0) { *out = KDB_TYPE_BLOB; return KDB_OK; }
    return KDB_ERR_SQL_SYNTAX;
}

/* NOT NULL / INDEX(ED) / UNIQUE / KEY / PRIMARY KEY / DEFAULT val -- shared
 * between CREATE TABLE's column defs and ALTER TABLE ADD COLUMN. Only
 * NOT NULL and INDEX(ED)/UNIQUE/PRIMARY KEY actually change the schema;
 * the rest are accepted and ignored, best-effort SQL DDL compatibility. */
static KdbStatus sql__parse_column_modifiers(SqlParser *p, const char *col_name, int *nullable, int *indexed) {
    for (;;) {
        if (sql__kw_is(&p->cur, "NOT")) {
            sql__advance(p);
            if (!sql__kw_is(&p->cur, "NULL")) return sql__err("expected NULL after NOT for column '%s'", col_name);
            sql__advance(p);
            *nullable = 0;
            continue;
        }
        if (sql__kw_is(&p->cur, "INDEX") || sql__kw_is(&p->cur, "INDEXED")) { sql__advance(p); *indexed = 1; continue; }
        if (sql__kw_is(&p->cur, "UNIQUE")) { sql__advance(p); *indexed = 1; continue; }
        if (sql__kw_is(&p->cur, "KEY"))    { sql__advance(p); *indexed = 1; continue; }
        if (sql__kw_is(&p->cur, "PRIMARY")) {
            sql__advance(p);
            if (sql__kw_is(&p->cur, "KEY")) sql__advance(p);
            *indexed = 1; *nullable = 0;
            continue;
        }
        if (sql__kw_is(&p->cur, "DEFAULT")) {
            sql__advance(p);
            sql__advance(p);
            continue;
        }
        break;
    }
    return KDB_OK;
}

static KdbStatus sql__exec_create_table(SqlParser *p, KumDB *db) {
    sql__advance(p); /* CREATE */
    if (!sql__kw_is(&p->cur, "TABLE")) return sql__err("expected TABLE after CREATE");
    sql__advance(p);

    const char *tname;
    if (!sql__ident_text(&p->cur, &tname)) return sql__err("expected a table name after CREATE TABLE");
    char table_name[KDB_SQL_IDENT_BUF];
    snprintf(table_name, sizeof(table_name), "%.255s", tname);
    sql__advance(p);

    if (p->cur.type != SQLTOK_LPAREN) return sql__err("expected '(' after table name '%s'", table_name);
    sql__advance(p);

    KdbColumnDef cols[KDB_SQL_MAX_COLUMNS];
    char names[KDB_SQL_MAX_COLUMNS][KDB_SQL_IDENT_BUF];
    uint32_t n = 0;

    for (;;) {
        const char *cname;
        if (!sql__ident_text(&p->cur, &cname)) return sql__err("expected a column name in CREATE TABLE '%s'", table_name);
        char this_name[KDB_SQL_IDENT_BUF];
        snprintf(this_name, sizeof(this_name), "%.255s", cname);
        sql__advance(p);

        const char *type_ident;
        if (!sql__ident_text(&p->cur, &type_ident)) return sql__err("expected a type for column '%s'", this_name);
        KdbFieldType ftype;
        if (sql__type_from_ident(type_ident, &ftype) != KDB_OK)
            return sql__err("unknown type '%s' for column '%s' -- use INT, FLOAT, BOOL, TEXT, or BLOB", type_ident, this_name);
        sql__advance(p);

        if (p->cur.type == SQLTOK_LPAREN) {
            /* VARCHAR(n) / CHAR(n) length spec: accepted, ignored -- KumDB strings aren't fixed-width */
            sql__advance(p);
            if (p->cur.type != SQLTOK_NUMBER) return sql__err("expected a number in the length spec for '%s'", this_name);
            sql__advance(p);
            if (p->cur.type != SQLTOK_RPAREN) return sql__err("expected ')' closing the length spec for '%s'", this_name);
            sql__advance(p);
        }

        int nullable = 1, indexed = 0;
        KdbStatus mst = sql__parse_column_modifiers(p, this_name, &nullable, &indexed);
        if (mst != KDB_OK) return mst;

        if (!sql__is_reserved_column(this_name)) {
            if (n >= KDB_SQL_MAX_COLUMNS) return sql__err("too many columns (max %d)", KDB_SQL_MAX_COLUMNS);
            snprintf(names[n], sizeof(names[n]), "%.255s", this_name);
            cols[n].name     = names[n];
            cols[n].type     = ftype;
            cols[n].nullable = nullable;
            cols[n].indexed  = indexed;
            n++;
        }

        if (p->cur.type == SQLTOK_COMMA) { sql__advance(p); continue; }
        break;
    }

    if (p->cur.type != SQLTOK_RPAREN) return sql__err("expected ')' closing the column list for '%s'", table_name);
    sql__advance(p);

    return kdb_create_table(db, table_name, cols, n);
}

static KdbStatus sql__exec_drop_table(SqlParser *p, KumDB *db) {
    sql__advance(p); /* DROP */
    if (!sql__kw_is(&p->cur, "TABLE")) return sql__err("expected TABLE after DROP");
    sql__advance(p);
    const char *tname;
    if (!sql__ident_text(&p->cur, &tname)) return sql__err("expected a table name after DROP TABLE");
    char table_name[KDB_SQL_IDENT_BUF];
    snprintf(table_name, sizeof(table_name), "%.255s", tname);
    sql__advance(p);
    return kdb_drop_table(db, table_name);
}

static KdbStatus sql__exec_alter_table(SqlParser *p, KumDB *db) {
    sql__advance(p); /* ALTER */
    if (!sql__kw_is(&p->cur, "TABLE")) return sql__err("expected TABLE after ALTER");
    sql__advance(p);

    const char *tname;
    if (!sql__ident_text(&p->cur, &tname)) return sql__err("expected a table name after ALTER TABLE");
    char table_name[KDB_SQL_IDENT_BUF];
    snprintf(table_name, sizeof(table_name), "%.255s", tname);
    sql__advance(p);

    if (sql__kw_is(&p->cur, "ADD")) {
        sql__advance(p);
        if (sql__kw_is(&p->cur, "COLUMN")) sql__advance(p); /* optional, both spellings accepted */

        const char *cname;
        if (!sql__ident_text(&p->cur, &cname)) return sql__err("expected a column name after ALTER TABLE %s ADD", table_name);
        char col_name[KDB_SQL_IDENT_BUF];
        snprintf(col_name, sizeof(col_name), "%.255s", cname);
        sql__advance(p);

        const char *type_ident;
        if (!sql__ident_text(&p->cur, &type_ident)) return sql__err("expected a type for column '%s'", col_name);
        KdbFieldType ftype;
        if (sql__type_from_ident(type_ident, &ftype) != KDB_OK)
            return sql__err("unknown type '%s' for column '%s' -- use INT, FLOAT, BOOL, TEXT, or BLOB", type_ident, col_name);
        sql__advance(p);

        if (p->cur.type == SQLTOK_LPAREN) {
            sql__advance(p);
            if (p->cur.type != SQLTOK_NUMBER) return sql__err("expected a number in the length spec for '%s'", col_name);
            sql__advance(p);
            if (p->cur.type != SQLTOK_RPAREN) return sql__err("expected ')' closing the length spec for '%s'", col_name);
            sql__advance(p);
        }

        int nullable = 1, indexed = 0;
        KdbStatus mst = sql__parse_column_modifiers(p, col_name, &nullable, &indexed);
        if (mst != KDB_OK) return mst;

        if (sql__is_reserved_column(col_name))
            return sql__err("'%s' is reserved -- KumDB already manages id/created_at/updated_at", col_name);

        return kdb_add_column(db, table_name, col_name, ftype, nullable, indexed);
    }

    if (sql__kw_is(&p->cur, "DROP")) {
        sql__advance(p);
        if (sql__kw_is(&p->cur, "COLUMN")) sql__advance(p);

        const char *cname;
        if (!sql__ident_text(&p->cur, &cname)) return sql__err("expected a column name after ALTER TABLE %s DROP", table_name);
        char col_name[KDB_SQL_IDENT_BUF];
        snprintf(col_name, sizeof(col_name), "%.255s", cname);
        sql__advance(p);

        return kdb_drop_column(db, table_name, col_name);
    }

    return sql__err("expected ADD [COLUMN] or DROP [COLUMN] after ALTER TABLE %s", table_name);
}

static KdbStatus sql__exec_insert(SqlParser *p, KumDB *db, size_t *affected_out) {
    sql__advance(p); /* INSERT */
    if (!sql__kw_is(&p->cur, "INTO")) return sql__err("expected INTO after INSERT");
    sql__advance(p);

    const char *tname;
    if (!sql__ident_text(&p->cur, &tname)) return sql__err("expected a table name after INSERT INTO");
    char table_name[KDB_SQL_IDENT_BUF];
    snprintf(table_name, sizeof(table_name), "%.255s", tname);
    sql__advance(p);

    char col_names[KDB_SQL_MAX_COLUMNS][KDB_SQL_IDENT_BUF];
    uint32_t ncols = 0;

    if (p->cur.type != SQLTOK_LPAREN) {
        return sql__err("INSERT needs an explicit column list: INSERT INTO %s (a, b) VALUES (...)", table_name);
    }
    sql__advance(p);
    for (;;) {
        const char *cname;
        if (!sql__ident_text(&p->cur, &cname)) return sql__err("expected a column name in the INSERT column list");
        if (ncols >= KDB_SQL_MAX_COLUMNS) return sql__err("too many columns (max %d)", KDB_SQL_MAX_COLUMNS);
        snprintf(col_names[ncols++], sizeof(col_names[0]), "%.255s", cname);
        sql__advance(p);
        if (p->cur.type == SQLTOK_COMMA) { sql__advance(p); continue; }
        break;
    }
    if (p->cur.type != SQLTOK_RPAREN) return sql__err("expected ')' closing the INSERT column list");
    sql__advance(p);

    if (!sql__kw_is(&p->cur, "VALUES")) return sql__err("expected VALUES after the column list for '%s'", table_name);
    sql__advance(p);
    if (p->cur.type != SQLTOK_LPAREN) return sql__err("expected '(' after VALUES");
    sql__advance(p);

    SqlToken value_toks[KDB_SQL_MAX_COLUMNS];
    uint32_t nvals = 0;
    for (;;) {
        if (nvals >= KDB_SQL_MAX_COLUMNS) return sql__err("too many values (max %d)", KDB_SQL_MAX_COLUMNS);
        value_toks[nvals++] = p->cur;
        sql__advance(p);
        if (p->cur.type == SQLTOK_COMMA) { sql__advance(p); continue; }
        break;
    }
    if (p->cur.type != SQLTOK_RPAREN) return sql__err("expected ')' closing the VALUES list");
    sql__advance(p);

    if (ncols != nvals)
        return sql__err("column count (%u) doesn't match value count (%u) for '%s'", ncols, nvals, table_name);

    KdbField fields[KDB_SQL_MAX_COLUMNS + 1];
    for (uint32_t i = 0; i < ncols; i++) {
        if (!sql__value_to_field(col_names[i], &value_toks[i], &fields[i]))
            return sql__err("unsupported value for column '%s'", col_names[i]);
    }
    fields[ncols] = kdb_field_end();

    KdbStatus st = kdb_add(db, table_name, fields);
    if (st == KDB_OK && affected_out) *affected_out = 1;
    return st;
}

/* Recursively frees a single field's owned memory (name, plus whatever the
 * value owns: string/blob data, or array/object children). Safe on a
 * partially-built field and safe when .name is NULL (array elements). */
static void sql__free_field(KdbField *f) {
    if (!f) return;
    free((void *)f->name);
    if (f->type == KDB_TYPE_STRING) {
        free((void *)f->v.as_string);
    } else if (f->type == KDB_TYPE_BLOB) {
        free((void *)f->v.as_blob.data);
    } else if (f->type == KDB_TYPE_ARRAY) {
        for (size_t i = 0; i < f->v.as_array.count; i++)
            sql__free_field((KdbField *)&f->v.as_array.items[i]);
        free((void *)f->v.as_array.items);
    } else if (f->type == KDB_TYPE_OBJECT) {
        if (f->v.as_object) {
            for (const KdbField *sub = f->v.as_object; sub->name != NULL; sub++)
                sql__free_field((KdbField *)sub);
            free((void *)f->v.as_object);
        }
    }
}

/* Deep-copies a field's value (owned string/blob/array/object memory) into
 * dst, which must already have dst->type set to src->type. Used whenever a
 * value from an input row needs to outlive that row (projection,
 * aggregation). Returns 0 on OOM (dst is left in a state safe to pass to
 * sql__free_field either way). */
static int sql__copy_field_value(KdbField *dst, const KdbField *src) {
    switch (src->type) {
        case KDB_TYPE_INT:    dst->v.as_int   = src->v.as_int;   return 1;
        case KDB_TYPE_FLOAT:  dst->v.as_float = src->v.as_float; return 1;
        case KDB_TYPE_BOOL:   dst->v.as_bool  = src->v.as_bool;  return 1;
        case KDB_TYPE_STRING:
            dst->v.as_string = src->v.as_string ? strdup(src->v.as_string) : NULL;
            return 1;
        case KDB_TYPE_BLOB:
            if (src->v.as_blob.len > 0 && src->v.as_blob.data) {
                void *copy = malloc(src->v.as_blob.len);
                if (!copy) return 0;
                memcpy(copy, src->v.as_blob.data, src->v.as_blob.len);
                dst->v.as_blob.data = copy;
            } else {
                dst->v.as_blob.data = NULL;
            }
            dst->v.as_blob.len = src->v.as_blob.len;
            return 1;
        case KDB_TYPE_ARRAY: {
            size_t count = src->v.as_array.count;
            dst->v.as_array.items = NULL;
            dst->v.as_array.count = 0;
            if (count == 0) return 1;
            KdbField *items = (KdbField *)calloc(count, sizeof(KdbField));
            if (!items) return 0;
            dst->v.as_array.items = items;
            dst->v.as_array.count = count;
            for (size_t i = 0; i < count; i++) {
                items[i].name = NULL;
                items[i].type = src->v.as_array.items[i].type;
                if (!sql__copy_field_value(&items[i], &src->v.as_array.items[i])) return 0;
            }
            return 1;
        }
        case KDB_TYPE_OBJECT: {
            uint32_t count = 0;
            if (src->v.as_object) while (src->v.as_object[count].name != NULL) count++;
            KdbField *fields = (KdbField *)calloc((size_t)count + 1, sizeof(KdbField));
            if (!fields) return 0;
            dst->v.as_object = fields;
            for (uint32_t i = 0; i < count; i++) {
                fields[i].name = strdup(src->v.as_object[i].name);
                if (!fields[i].name) return 0;
                fields[i].type = src->v.as_object[i].type;
                if (!sql__copy_field_value(&fields[i], &src->v.as_object[i])) return 0;
            }
            return 1;
        }
        default:
            memset(&dst->v, 0, sizeof(dst->v));
            return 1;
    }
}

static void sql__free_row_fields(KdbRow *row) {
    if (!row || !row->fields) return;
    for (uint32_t i = 0; i < row->field_count; i++)
        sql__free_field(&row->fields[i]);
    free(row->fields);
    row->fields      = NULL;
    row->field_count = 0;
}

typedef enum { SQL_AGG_NONE, SQL_AGG_COUNT, SQL_AGG_SUM, SQL_AGG_AVG, SQL_AGG_MIN, SQL_AGG_MAX } SqlAggFn;

typedef struct {
    SqlAggFn fn;                          /* SQL_AGG_NONE = plain column, not an aggregate */
    char     arg_col[KDB_SQL_IDENT_BUF];  /* column name, or "*" for COUNT(*) */
    char     alias[KDB_SQL_IDENT_BUF];    /* output field name */
} SqlSelectItem;

static int sql__agg_fn_from_ident(const char *s, SqlAggFn *out) {
    if (strcasecmp(s, "COUNT") == 0) { *out = SQL_AGG_COUNT; return 1; }
    if (strcasecmp(s, "SUM")   == 0) { *out = SQL_AGG_SUM;   return 1; }
    if (strcasecmp(s, "AVG")   == 0) { *out = SQL_AGG_AVG;   return 1; }
    if (strcasecmp(s, "MIN")   == 0) { *out = SQL_AGG_MIN;   return 1; }
    if (strcasecmp(s, "MAX")   == 0) { *out = SQL_AGG_MAX;   return 1; }
    return 0;
}

/* INT/FLOAT/BOOL are all numeric for aggregate purposes, same "bool is 0/1"
 * convention the comparison engine already uses. */
static int sql__field_to_double(const KdbField *f, double *out) {
    if (!f) return 0;
    switch (f->type) {
        case KDB_TYPE_INT:   *out = (double)f->v.as_int;  return 1;
        case KDB_TYPE_FLOAT: *out = f->v.as_float;         return 1;
        case KDB_TYPE_BOOL:  *out = (double)f->v.as_bool;  return 1;
        default: return 0;
    }
}

static int sql__field_equal(const KdbField *a, const KdbField *b) {
    if (!a || !b) return a == b;
    double da, db;
    if (sql__field_to_double(a, &da) && sql__field_to_double(b, &db)) return da == db;
    if (a->type == KDB_TYPE_STRING && b->type == KDB_TYPE_STRING)
        return strcmp(a->v.as_string ? a->v.as_string : "", b->v.as_string ? b->v.as_string : "") == 0;
    return 0;
}

/* strcmp-style ordering for MIN/MAX; incomparable pairs sort as equal
 * (won't replace the current extremum) rather than erroring mid-query. */
static int sql__field_cmp(const KdbField *a, const KdbField *b) {
    double da, db;
    if (sql__field_to_double(a, &da) && sql__field_to_double(b, &db)) {
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }
    if (a->type == KDB_TYPE_STRING && b->type == KDB_TYPE_STRING)
        return strcmp(a->v.as_string ? a->v.as_string : "", b->v.as_string ? b->v.as_string : "");
    return 0;
}

#define SQL_CMP_INCOMPARABLE (-2)

/* Same idea as sql__field_cmp, but against a raw filter-value string
 * instead of another field -- and distinguishes "genuinely incomparable"
 * (SQL_CMP_INCOMPARABLE) from "compares equal", since a filter like
 * age__gt=abc on an int column must not match. */
static int sql__field_cmp_text(const KdbField *f, const char *text) {
    if (!f) return SQL_CMP_INCOMPARABLE;
    double fd;
    if (sql__field_to_double(f, &fd)) {
        char *end = NULL;
        double td = strtod(text, &end);
        if (end == text) return SQL_CMP_INCOMPARABLE;
        if (fd < td) return -1;
        if (fd > td) return 1;
        return 0;
    }
    if (f->type == KDB_TYPE_STRING)
        return strcmp(f->v.as_string ? f->v.as_string : "", text);
    return SQL_CMP_INCOMPARABLE;
}

typedef enum {
    SQL_ROP_EQ, SQL_ROP_NEQ, SQL_ROP_GT, SQL_ROP_GTE, SQL_ROP_LT, SQL_ROP_LTE,
    SQL_ROP_BETWEEN, SQL_ROP_IN, SQL_ROP_CONTAINS, SQL_ROP_STARTSWITH,
    SQL_ROP_ENDSWITH, SQL_ROP_ISNULL, SQL_ROP_ISNOTNULL
} SqlRowOp;

typedef struct {
    char     col[KDB_SQL_IDENT_BUF];
    SqlRowOp op;
    char     value[KDB_SQL_TOK_MAX];  /* raw text; comma-separated for IN/BETWEEN */
    int      is_or_start;             /* filter string was "OR:"-prefixed */
} SqlRowCond;

/* Parses one filter string in the exact shape sql__parse_condition() emits
 * ("col__op=value", "col=value", "col__isnull", optionally "OR:"-prefixed)
 * back into a structured condition -- so HAVING and post-JOIN WHERE can
 * evaluate the same filter strings sql__parse_where() already builds
 * directly against an in-memory KdbRow, instead of a stored table. Returns
 * 0 on a malformed string (shouldn't happen, these are always our own
 * output, but fail closed rather than assert). */
static int sql__parse_row_cond(const char *filter, SqlRowCond *out) {
    memset(out, 0, sizeof(*out));
    if (strncmp(filter, "OR:", 3) == 0) { out->is_or_start = 1; filter += 3; }

    const char *eq = strchr(filter, '=');
    const char *dunder = strstr(filter, "__");
    if (dunder && dunder != filter && (!eq || dunder < eq)) {
        size_t col_len = (size_t)(dunder - filter);
        if (col_len >= sizeof(out->col)) col_len = sizeof(out->col) - 1;
        memcpy(out->col, filter, col_len);
        out->col[col_len] = '\0';

        const char *op_start = dunder + 2;
        const char *op_end   = eq ? eq : (filter + strlen(filter));
        char op_buf[32];
        size_t op_len = (size_t)(op_end - op_start);
        if (op_len >= sizeof(op_buf)) op_len = sizeof(op_buf) - 1;
        memcpy(op_buf, op_start, op_len);
        op_buf[op_len] = '\0';

        if      (strcmp(op_buf, "eq")         == 0) out->op = SQL_ROP_EQ;
        else if (strcmp(op_buf, "neq")        == 0) out->op = SQL_ROP_NEQ;
        else if (strcmp(op_buf, "gt")         == 0) out->op = SQL_ROP_GT;
        else if (strcmp(op_buf, "gte")        == 0) out->op = SQL_ROP_GTE;
        else if (strcmp(op_buf, "lt")         == 0) out->op = SQL_ROP_LT;
        else if (strcmp(op_buf, "lte")        == 0) out->op = SQL_ROP_LTE;
        else if (strcmp(op_buf, "between")    == 0) out->op = SQL_ROP_BETWEEN;
        else if (strcmp(op_buf, "in")         == 0) out->op = SQL_ROP_IN;
        else if (strcmp(op_buf, "contains")   == 0) out->op = SQL_ROP_CONTAINS;
        else if (strcmp(op_buf, "startswith") == 0) out->op = SQL_ROP_STARTSWITH;
        else if (strcmp(op_buf, "endswith")   == 0) out->op = SQL_ROP_ENDSWITH;
        else if (strcmp(op_buf, "isnull")     == 0) out->op = SQL_ROP_ISNULL;
        else if (strcmp(op_buf, "isnotnull")  == 0) out->op = SQL_ROP_ISNOTNULL;
        else return 0;

        if (eq) snprintf(out->value, sizeof(out->value), "%s", eq + 1);
        return 1;
    }

    if (!eq) return 0;
    size_t col_len = (size_t)(eq - filter);
    if (col_len >= sizeof(out->col)) col_len = sizeof(out->col) - 1;
    memcpy(out->col, filter, col_len);
    out->col[col_len] = '\0';
    out->op = SQL_ROP_EQ;
    snprintf(out->value, sizeof(out->value), "%s", eq + 1);
    return 1;
}

static int sql__row_cond_matches(const KdbRow *row, const SqlRowCond *c) {
    const KdbField *f = kdb_row_get(row, c->col);

    if (c->op == SQL_ROP_ISNULL)    return !f || f->type == KDB_TYPE_NULL;
    if (c->op == SQL_ROP_ISNOTNULL) return f && f->type != KDB_TYPE_NULL;
    if (!f) return 0;

    switch (c->op) {
        case SQL_ROP_EQ:  { int r = sql__field_cmp_text(f, c->value); return r != SQL_CMP_INCOMPARABLE && r == 0; }
        case SQL_ROP_NEQ: { int r = sql__field_cmp_text(f, c->value); return r != SQL_CMP_INCOMPARABLE && r != 0; }
        case SQL_ROP_GT:  { int r = sql__field_cmp_text(f, c->value); return r != SQL_CMP_INCOMPARABLE && r > 0; }
        case SQL_ROP_GTE: { int r = sql__field_cmp_text(f, c->value); return r != SQL_CMP_INCOMPARABLE && r >= 0; }
        case SQL_ROP_LT:  { int r = sql__field_cmp_text(f, c->value); return r != SQL_CMP_INCOMPARABLE && r < 0; }
        case SQL_ROP_LTE: { int r = sql__field_cmp_text(f, c->value); return r != SQL_CMP_INCOMPARABLE && r <= 0; }
        case SQL_ROP_CONTAINS:
            return f->type == KDB_TYPE_STRING && f->v.as_string && strstr(f->v.as_string, c->value) != NULL;
        case SQL_ROP_STARTSWITH:
            return f->type == KDB_TYPE_STRING && f->v.as_string &&
                   strncmp(f->v.as_string, c->value, strlen(c->value)) == 0;
        case SQL_ROP_ENDSWITH: {
            if (f->type != KDB_TYPE_STRING || !f->v.as_string) return 0;
            size_t flen = strlen(f->v.as_string), slen = strlen(c->value);
            if (slen > flen) return 0;
            return strcmp(f->v.as_string + (flen - slen), c->value) == 0;
        }
        case SQL_ROP_BETWEEN: {
            const char *comma = strchr(c->value, ',');
            if (!comma) return 0;
            char lo[KDB_SQL_IDENT_BUF];
            size_t lo_len = (size_t)(comma - c->value);
            if (lo_len >= sizeof(lo)) lo_len = sizeof(lo) - 1;
            memcpy(lo, c->value, lo_len);
            lo[lo_len] = '\0';
            int cl = sql__field_cmp_text(f, lo);
            int ch = sql__field_cmp_text(f, comma + 1);
            return cl != SQL_CMP_INCOMPARABLE && ch != SQL_CMP_INCOMPARABLE && cl >= 0 && ch <= 0;
        }
        case SQL_ROP_IN: {
            const char *p = c->value;
            while (*p) {
                const char *comma = strchr(p, ',');
                size_t tok_len = comma ? (size_t)(comma - p) : strlen(p);
                char tok[KDB_SQL_IDENT_BUF];
                if (tok_len >= sizeof(tok)) tok_len = sizeof(tok) - 1;
                memcpy(tok, p, tok_len);
                tok[tok_len] = '\0';
                int r = sql__field_cmp_text(f, tok);
                if (r != SQL_CMP_INCOMPARABLE && r == 0) return 1;
                if (!comma) break;
                p = comma + 1;
            }
            return 0;
        }
        default: return 0;
    }
}

/* Same AND-within-group / OR-across-groups semantics sql__parse_where()'s
 * "OR:" convention encodes (and that the storage-layer query engine also
 * implements): a row matches if ANY group's conditions are ALL true. */
static int sql__row_matches_filters(const KdbRow *row, char *const *filters, int nfilt) {
    if (nfilt == 0) return 1;
    int group_ok = 1;
    int result = 0;
    for (int i = 0; i < nfilt; i++) {
        SqlRowCond c;
        if (!sql__parse_row_cond(filters[i], &c)) return 0;
        if (c.is_or_start && i > 0) {
            if (group_ok) result = 1;
            group_ok = 1;
        }
        if (!sql__row_cond_matches(row, &c)) group_ok = 0;
    }
    if (group_ok) result = 1;
    return result;
}

/* In-place filter: drops rows that don't match, freeing their field
 * memory, preserving order of the rows that stay. Used for HAVING and
 * post-JOIN WHERE, both of which need to filter an already-materialized
 * KdbRows rather than fetch from a stored table. */
static void sql__filter_rows(KdbRows *rows, char *const *filters, int nfilt) {
    if (!rows || nfilt == 0) return;
    size_t kept = 0;
    for (size_t i = 0; i < rows->count; i++) {
        if (sql__row_matches_filters(&rows->rows[i], filters, nfilt)) {
            if (kept != i) rows->rows[kept] = rows->rows[i];
            kept++;
        } else {
            sql__free_row_fields(&rows->rows[i]);
        }
    }
    rows->count = kept;
}

#define KDB_SQL_MAX_GROUPS 512

typedef struct {
    const KdbField *key_ref;   /* group-by column's value, pointing into the source rows; NULL = missing/null bucket */
    size_t          row_count;
    double          sum[KDB_SQL_MAX_COLUMNS];
    size_t          count_nonnull[KDB_SQL_MAX_COLUMNS];
    const KdbField *min_ref[KDB_SQL_MAX_COLUMNS];
    const KdbField *max_ref[KDB_SQL_MAX_COLUMNS];
} SqlGroupAcc;

/* One row per group (or one summary row if there's no GROUP BY), one field
 * per SELECT item. 'all' must stay alive for the whole call -- group key
 * and MIN/MAX tracking hold pointers into its row data until the final
 * copy at the end. */
static KdbStatus sql__compute_aggregates(KdbRows *all, SqlSelectItem *items, uint32_t nitems,
                                         int has_group_by, const char *group_col,
                                         KdbRows **out) {
    SqlGroupAcc *groups = (SqlGroupAcc *)calloc(KDB_SQL_MAX_GROUPS, sizeof(SqlGroupAcc));
    if (!groups) { kdb_err_oom("aggregate groups"); return KDB_ERR_OOM; }
    uint32_t ngroups = 0;

    for (size_t r = 0; r < all->count; r++) {
        KdbRow *row = &all->rows[r];
        const KdbField *key_ref = has_group_by ? kdb_row_get(row, group_col) : NULL;

        uint32_t gi;
        if (!has_group_by) {
            gi = 0;
            if (ngroups == 0) ngroups = 1;
        } else {
            int found = 0;
            for (gi = 0; gi < ngroups; gi++) {
                if (sql__field_equal(groups[gi].key_ref, key_ref)) { found = 1; break; }
            }
            if (!found) {
                if (ngroups >= KDB_SQL_MAX_GROUPS) {
                    free(groups);
                    kdb_set_error(KDB_ERR_SQL_SYNTAX, "SQL error: too many distinct groups (max %d)", KDB_SQL_MAX_GROUPS);
                    return KDB_ERR_SQL_SYNTAX;
                }
                gi = ngroups++;
                groups[gi].key_ref = key_ref;
            }
        }

        SqlGroupAcc *g = &groups[gi];
        g->row_count++;

        for (uint32_t it = 0; it < nitems; it++) {
            if (items[it].fn == SQL_AGG_NONE) continue;
            if (items[it].fn == SQL_AGG_COUNT && strcmp(items[it].arg_col, "*") == 0) continue;

            const KdbField *f = kdb_row_get(row, items[it].arg_col);
            if (!f) continue;

            switch (items[it].fn) {
                case SQL_AGG_COUNT:
                    g->count_nonnull[it]++;
                    break;
                case SQL_AGG_SUM:
                case SQL_AGG_AVG: {
                    double v;
                    if (sql__field_to_double(f, &v)) {
                        g->sum[it] += v;
                        g->count_nonnull[it]++;
                    }
                    break;
                }
                case SQL_AGG_MIN:
                    if (!g->min_ref[it] || sql__field_cmp(f, g->min_ref[it]) < 0) g->min_ref[it] = f;
                    break;
                case SQL_AGG_MAX:
                    if (!g->max_ref[it] || sql__field_cmp(f, g->max_ref[it]) > 0) g->max_ref[it] = f;
                    break;
                default: break;
            }
        }
    }

    if (!has_group_by && ngroups == 0) ngroups = 1; /* no matching rows: still emit one summary row */

    KdbRows *result = (KdbRows *)calloc(1, sizeof(KdbRows));
    if (!result) { free(groups); kdb_err_oom("aggregate result"); return KDB_ERR_OOM; }
    result->rows = (KdbRow *)calloc(ngroups, sizeof(KdbRow));
    if (!result->rows) { free(result); free(groups); kdb_err_oom("aggregate result rows"); return KDB_ERR_OOM; }
    result->count = ngroups;

    for (uint32_t gi = 0; gi < ngroups; gi++) {
        SqlGroupAcc *g = &groups[gi];
        KdbRow *orow = &result->rows[gi];

        KdbField *fields = (KdbField *)calloc(nitems, sizeof(KdbField));
        if (!fields) { kdb_rows_free(result); free(groups); kdb_err_oom("aggregate row fields"); return KDB_ERR_OOM; }

        for (uint32_t it = 0; it < nitems; it++) {
            KdbField *of = &fields[it];
            of->name = strdup(items[it].alias);
            if (!of->name) {
                for (uint32_t k = 0; k < it; k++) sql__free_field(&fields[k]);
                free(fields);
                kdb_rows_free(result);
                free(groups);
                kdb_err_oom("aggregate field name");
                return KDB_ERR_OOM;
            }

            int copy_ok = 1;
            switch (items[it].fn) {
                case SQL_AGG_NONE:
                    if (g->key_ref) { of->type = g->key_ref->type; copy_ok = sql__copy_field_value(of, g->key_ref); }
                    else            { of->type = KDB_TYPE_NULL; }
                    break;
                case SQL_AGG_COUNT:
                    of->type = KDB_TYPE_INT;
                    of->v.as_int = (int64_t)(strcmp(items[it].arg_col, "*") == 0 ? g->row_count : g->count_nonnull[it]);
                    break;
                case SQL_AGG_SUM:
                    of->type = KDB_TYPE_FLOAT;
                    of->v.as_float = g->sum[it];
                    break;
                case SQL_AGG_AVG:
                    of->type = KDB_TYPE_FLOAT;
                    of->v.as_float = g->count_nonnull[it] > 0 ? g->sum[it] / (double)g->count_nonnull[it] : 0.0;
                    break;
                case SQL_AGG_MIN:
                    if (g->min_ref[it]) { of->type = g->min_ref[it]->type; copy_ok = sql__copy_field_value(of, g->min_ref[it]); }
                    else                { of->type = KDB_TYPE_NULL; }
                    break;
                case SQL_AGG_MAX:
                    if (g->max_ref[it]) { of->type = g->max_ref[it]->type; copy_ok = sql__copy_field_value(of, g->max_ref[it]); }
                    else                { of->type = KDB_TYPE_NULL; }
                    break;
            }
            if (!copy_ok) {
                for (uint32_t k = 0; k <= it; k++) sql__free_field(&fields[k]);
                free(fields);
                kdb_rows_free(result);
                free(groups);
                kdb_err_oom("aggregate field value");
                return KDB_ERR_OOM;
            }
        }

        orow->fields      = fields;
        orow->field_count = nitems;
    }

    free(groups);
    *out = result;
    return KDB_OK;
}

static const char *sql__sort_col = NULL;
static int         sql__sort_asc = 1;

static int sql__row_cmp(const void *a, const void *b) {
    const KdbRow *ra = (const KdbRow *)a;
    const KdbRow *rb = (const KdbRow *)b;
    const KdbField *fa = kdb_row_get(ra, sql__sort_col);
    const KdbField *fb = kdb_row_get(rb, sql__sort_col);
    if (!fa && !fb) return 0;
    if (!fa) return sql__sort_asc ? -1 : 1;
    if (!fb) return sql__sort_asc ? 1 : -1;
    int cmp = sql__field_cmp(fa, fb);
    return sql__sort_asc ? cmp : -cmp;
}

static void sql__sort_rows(KdbRows *rows, const char *col, int ascending) {
    if (!rows || !col || rows->count == 0) return;
    sql__sort_col = col;
    sql__sort_asc = ascending;
    qsort(rows->rows, rows->count, sizeof(KdbRow), sql__row_cmp);
}

static void sql__limit_rows(KdbRows *rows, size_t offset, size_t limit) {
    if (!rows) return;
    if (offset > 0) {
        if (offset >= rows->count) {
            for (size_t i = 0; i < rows->count; i++) sql__free_row_fields(&rows->rows[i]);
            rows->count = 0;
            return;
        }
        for (size_t i = 0; i < offset; i++) sql__free_row_fields(&rows->rows[i]);
        memmove(rows->rows, rows->rows + offset, (rows->count - offset) * sizeof(KdbRow));
        rows->count -= offset;
    }
    if (limit > 0 && rows->count > limit) {
        for (size_t i = limit; i < rows->count; i++) sql__free_row_fields(&rows->rows[i]);
        rows->count = limit;
    }
}

/* Full-row equality (same field count, same names in the same order, same
 * values) -- used by DISTINCT and UNION (not UNION ALL). Rows compared here
 * have already gone through the same projection, so name order lining up
 * is a safe assumption, not a limitation. */
static int sql__row_equal(const KdbRow *a, const KdbRow *b) {
    if (a->field_count != b->field_count) return 0;
    for (uint32_t i = 0; i < a->field_count; i++) {
        const KdbField *fa = &a->fields[i];
        const KdbField *fb = &b->fields[i];
        if (strcmp(fa->name ? fa->name : "", fb->name ? fb->name : "") != 0) return 0;
        if (fa->type != fb->type) return 0;
        if (fa->type == KDB_TYPE_NULL) continue; /* both null (type check above already matched): equal */
        /* sql__field_equal covers numeric (incl. bool) and string; blob,
         * array, and object always compare unequal here -- good enough for
         * DISTINCT/UNION on the row shapes SQL SELECT actually produces,
         * deep-equality on nested values isn't worth the plumbing for it. */
        if (!sql__field_equal(fa, fb)) return 0;
    }
    return 1;
}

/* Removes rows that exactly duplicate an earlier row (freeing the
 * duplicate's field memory), preserving first-occurrence order. O(n^2)
 * comparisons, which is fine at the row counts this engine targets. */
static void sql__dedupe_rows(KdbRows *rows) {
    if (!rows || rows->count == 0) return;
    size_t kept = 0;
    for (size_t i = 0; i < rows->count; i++) {
        int dup = 0;
        for (size_t j = 0; j < kept; j++) {
            if (sql__row_equal(&rows->rows[i], &rows->rows[j])) { dup = 1; break; }
        }
        if (dup) {
            sql__free_row_fields(&rows->rows[i]);
        } else {
            if (kept != i) rows->rows[kept] = rows->rows[i];
            kept++;
        }
    }
    rows->count = kept;
}

static KdbStatus sql__project_rows(KdbRows *rows, char proj_cols[][KDB_SQL_IDENT_BUF], uint32_t nproj) {
    for (size_t r = 0; r < rows->count; r++) {
        KdbRow *row = &rows->rows[r];
        KdbField *new_fields = (KdbField *)calloc(nproj > 0 ? nproj : 1, sizeof(KdbField));
        if (!new_fields) return KDB_ERR_OOM;

        uint32_t kept = 0;
        for (uint32_t i = 0; i < nproj; i++) {
            const KdbField *src = kdb_row_get(row, proj_cols[i]);
            if (!src) continue;

            KdbField *dst = &new_fields[kept];
            dst->name = strdup(proj_cols[i]);
            dst->type = src->type;
            if (!dst->name || !sql__copy_field_value(dst, src)) {
                for (uint32_t k = 0; k <= kept; k++) sql__free_field(&new_fields[k]);
                free(new_fields);
                return KDB_ERR_OOM;
            }
            kept++;
        }

        sql__free_row_fields(row);
        row->fields      = new_fields;
        row->field_count = kept;
    }
    return KDB_OK;
}

#define KDB_SQL_MAX_JOIN_COND 8

typedef struct {
    char left[KDB_SQL_IDENT_BUF];
    char right[KDB_SQL_IDENT_BUF];
} SqlJoinCond;

/* ON is a conjunction of col = col equalities only -- no OR, no comparing
 * to a literal. That's what WHERE (applied after the join, over the
 * qualified combined rows) is for. Both sides are expected to already be
 * table-qualified ("alias.col"), same as everywhere else after a JOIN. */
static int sql__parse_join_on(SqlParser *p, SqlJoinCond conds[KDB_SQL_MAX_JOIN_COND], int *count_out) {
    int n = 0;
    for (;;) {
        const char *lc;
        if (!sql__ident_text(&p->cur, &lc)) { sql__err("expected a column reference in ON"); return 0; }
        char left[KDB_SQL_IDENT_BUF];
        snprintf(left, sizeof(left), "%.255s", lc);
        sql__advance(p);

        if (p->cur.type != SQLTOK_EQ) {
            sql__err("ON only supports col = col equality (unexpected token after '%s')", left);
            return 0;
        }
        sql__advance(p);

        const char *rc;
        if (!sql__ident_text(&p->cur, &rc)) { sql__err("expected a column reference after '=' in ON"); return 0; }
        if (n >= KDB_SQL_MAX_JOIN_COND) { sql__err("too many ON conditions (max %d)", KDB_SQL_MAX_JOIN_COND); return 0; }
        snprintf(conds[n].left,  sizeof(conds[n].left),  "%s",    left);
        snprintf(conds[n].right, sizeof(conds[n].right), "%.255s", rc);
        n++;
        sql__advance(p);

        if (sql__kw_is(&p->cur, "AND")) { sql__advance(p); continue; }
        break;
    }
    *count_out = n;
    return 1;
}

/* GCC's -Wformat-truncation can't prove qname's "%s.%s" is safe here --
 * qual/name are runtime char* parameters, not compile-time sizeof()'d
 * arrays at the point of the snprintf call, even though every real caller
 * passes a KDB_SQL_IDENT_BUF-bounded string. Same well-known limitation
 * as kdb__tx_backup_path/kdb__tx_marker_path in kumdb.c. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/* Appends one qualified copy of every field in src (plus synthetic
 * "<qual>.id"/"<qual>.created_at"/"<qual>.updated_at" pseudo-fields, since
 * those are common join keys -- e.g. ON orders.user_id = users.id -- but
 * aren't ordinarily part of a row's field list) into dst, growing dst's
 * field array. Returns 0 on OOM (dst is left with whatever prefix already
 * got appended, field_count only ever counts fully-built fields, so it's
 * still safe to free). */
static int sql__append_qualified_fields(KdbRow *dst, const KdbRow *src, const char *qual) {
    uint32_t extra = src->field_count + 3;
    KdbField *grown = (KdbField *)realloc(dst->fields, (dst->field_count + extra) * sizeof(KdbField));
    if (!grown) return 0;
    dst->fields = grown;

    for (uint32_t i = 0; i < src->field_count; i++) {
        char qname[KDB_SQL_IDENT_BUF];
        snprintf(qname, sizeof(qname), "%s.%s", qual, src->fields[i].name);
        KdbField f;
        f.name = strdup(qname);
        f.type = src->fields[i].type;
        if (!f.name) return 0;
        if (!sql__copy_field_value(&f, &src->fields[i])) { free((void *)f.name); return 0; }
        dst->fields[dst->field_count++] = f;
    }

    static const char *pseudo_names[3] = { "id", "created_at", "updated_at" };
    uint64_t pseudo_vals[3] = { src->id, src->created_at, src->updated_at };
    for (int i = 0; i < 3; i++) {
        char qname[KDB_SQL_IDENT_BUF];
        snprintf(qname, sizeof(qname), "%s.%s", qual, pseudo_names[i]);
        char *nm = strdup(qname);
        if (!nm) return 0;
        dst->fields[dst->field_count].name = nm;
        dst->fields[dst->field_count].type = KDB_TYPE_INT;
        dst->fields[dst->field_count].v.as_int = (int64_t)pseudo_vals[i];
        dst->field_count++;
    }
    return 1;
}

/* LEFT JOIN padding for an unmatched left-side row: every right-side
 * column (named from its schema, since there's no actual matching row to
 * name them from) plus the id/created_at/updated_at pseudo-fields, all
 * NULL -- a real LEFT JOIN gives NULL for every right-side column,
 * including its key, when nothing matched. Same OOM-safety shape as
 * sql__append_qualified_fields. */
static int sql__append_qualified_nulls(KdbRow *dst, const KdbColumnInfo *schema, uint32_t schema_count, const char *qual) {
    uint32_t extra = schema_count + 3;
    KdbField *grown = (KdbField *)realloc(dst->fields, (dst->field_count + extra) * sizeof(KdbField));
    if (!grown) return 0;
    dst->fields = grown;

    for (uint32_t i = 0; i < schema_count; i++) {
        char qname[KDB_SQL_IDENT_BUF];
        snprintf(qname, sizeof(qname), "%s.%s", qual, schema[i].name);
        char *nm = strdup(qname);
        if (!nm) return 0;
        dst->fields[dst->field_count].name = nm;
        dst->fields[dst->field_count].type = KDB_TYPE_NULL;
        dst->field_count++;
    }
    static const char *pseudo_names[3] = { "id", "created_at", "updated_at" };
    for (int i = 0; i < 3; i++) {
        char qname[KDB_SQL_IDENT_BUF];
        snprintf(qname, sizeof(qname), "%s.%s", qual, pseudo_names[i]);
        char *nm = strdup(qname);
        if (!nm) return 0;
        dst->fields[dst->field_count].name = nm;
        dst->fields[dst->field_count].type = KDB_TYPE_NULL;
        dst->field_count++;
    }
    return 1;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Nested-loop join of two tables into qualified combined rows -- every
 * column from both sides is renamed "<qual>.col" (qual is the table's own
 * name, or its alias if one was given), so a shared column name on both
 * sides ("name" on both employees and managers, say) never collides.
 * There's no unqualified fallback; every column reference after a JOIN
 * (SELECT list, WHERE, ORDER BY) needs the "<qual>." prefix.
 *
 * INNER drops rows with no ON match; LEFT keeps every left-side row,
 * padding an unmatched one with NULLs for every right-side column. Fetches
 * both tables in full (no filter pushdown into the join itself) -- WHERE
 * is applied by the caller afterward, over the combined rows. */
static KdbStatus sql__build_joined_rows(KumDB *db, const char *table1, const char *alias1,
                                        const char *table2, const char *alias2, int is_left,
                                        const SqlJoinCond *conds, int ncond, KdbRows **rows_out) {
    KdbRows *r1 = kdb_find_ex(db, table1, NULL, NULL);
    if (!r1) return kdb_last_status();
    KdbRows *r2 = kdb_find_ex(db, table2, NULL, NULL);
    if (!r2) { kdb_rows_free(r1); return kdb_last_status(); }

    KdbColumnInfo schema2[KDB_MAX_COLUMNS];
    uint32_t schema2_count = 0;
    if (is_left && kdb_get_schema(db, table2, schema2, KDB_MAX_COLUMNS, &schema2_count) != KDB_OK) {
        kdb_rows_free(r1);
        kdb_rows_free(r2);
        return kdb_last_status();
    }

    KdbRows *out = (KdbRows *)calloc(1, sizeof(KdbRows));
    if (!out) { kdb_rows_free(r1); kdb_rows_free(r2); kdb_err_oom("joined rows"); return KDB_ERR_OOM; }

#define KDB_SQL_JOIN_FAIL(row, msg) do { \
        sql__free_row_fields(&(row)); \
        kdb_rows_free(r1); kdb_rows_free(r2); kdb_rows_free(out); \
        kdb_err_oom(msg); \
        return KDB_ERR_OOM; \
    } while (0)

    for (size_t i = 0; i < r1->count; i++) {
        int matched = 0;
        for (size_t j = 0; j < r2->count; j++) {
            KdbRow combo;
            memset(&combo, 0, sizeof(combo));
            if (!sql__append_qualified_fields(&combo, &r1->rows[i], alias1) ||
                !sql__append_qualified_fields(&combo, &r2->rows[j], alias2))
                KDB_SQL_JOIN_FAIL(combo, "joined row");

            int on_ok = 1;
            for (int c = 0; c < ncond; c++) {
                const KdbField *lf = kdb_row_get(&combo, conds[c].left);
                const KdbField *rf = kdb_row_get(&combo, conds[c].right);
                if (!lf || !rf || !sql__field_equal(lf, rf)) { on_ok = 0; break; }
            }

            if (!on_ok) { sql__free_row_fields(&combo); continue; }
            matched = 1;

            KdbRow *grown = (KdbRow *)realloc(out->rows, (out->count + 1) * sizeof(KdbRow));
            if (!grown) KDB_SQL_JOIN_FAIL(combo, "joined rows grow");
            out->rows = grown;
            out->rows[out->count++] = combo;
        }

        if (is_left && !matched) {
            KdbRow combo;
            memset(&combo, 0, sizeof(combo));
            if (!sql__append_qualified_fields(&combo, &r1->rows[i], alias1) ||
                !sql__append_qualified_nulls(&combo, schema2, schema2_count, alias2))
                KDB_SQL_JOIN_FAIL(combo, "left-join padded row");

            KdbRow *grown = (KdbRow *)realloc(out->rows, (out->count + 1) * sizeof(KdbRow));
            if (!grown) KDB_SQL_JOIN_FAIL(combo, "joined rows grow");
            out->rows = grown;
            out->rows[out->count++] = combo;
        }
    }

#undef KDB_SQL_JOIN_FAIL

    kdb_rows_free(r1);
    kdb_rows_free(r2);
    *rows_out = out;
    return KDB_OK;
}

/* Parses and executes one SELECT's core -- everything from the column list
 * through HAVING -- and returns raw rows with no ORDER BY/LIMIT applied
 * (DISTINCT, if present, still runs: it's part of this SELECT's own
 * projection, not something a later UNION could reorder around). Used both
 * standalone (a plain SELECT) and as one arm of a UNION chain, where
 * ORDER BY/LIMIT can only meaningfully apply once, to the combined result,
 * not to an individual arm -- so parsing them is the caller's job. */
static KdbStatus sql__exec_select_core(SqlParser *p, KumDB *db, KdbRows **rows_out) {
    sql__advance(p); /* SELECT */

    int distinct = 0;
    if (sql__kw_is(&p->cur, "DISTINCT")) { distinct = 1; sql__advance(p); }

    int project_all = 0;
    SqlSelectItem items[KDB_SQL_MAX_COLUMNS];
    uint32_t nitems = 0;
    int has_aggregate = 0;

    if (p->cur.type == SQLTOK_STAR) {
        project_all = 1;
        sql__advance(p);
    } else {
        for (;;) {
            const char *cname;
            if (!sql__ident_text(&p->cur, &cname))
                return sql__err("expected a column name, aggregate function, or '*' after SELECT");
            char first_ident[KDB_SQL_IDENT_BUF];
            snprintf(first_ident, sizeof(first_ident), "%.255s", cname);
            sql__advance(p);

            SqlSelectItem item;
            memset(&item, 0, sizeof(item));

            SqlAggFn fn;
            if (sql__agg_fn_from_ident(first_ident, &fn) && p->cur.type == SQLTOK_LPAREN) {
                sql__advance(p);
                if (p->cur.type == SQLTOK_STAR) {
                    if (fn != SQL_AGG_COUNT) return sql__err("only COUNT(*) is supported, not %s(*)", first_ident);
                    snprintf(item.arg_col, sizeof(item.arg_col), "*");
                    sql__advance(p);
                } else {
                    const char *acol;
                    if (!sql__ident_text(&p->cur, &acol))
                        return sql__err("expected a column name or '*' inside %s(...)", first_ident);
                    snprintf(item.arg_col, sizeof(item.arg_col), "%.255s", acol);
                    sql__advance(p);
                }
                if (p->cur.type != SQLTOK_RPAREN) return sql__err("expected ')' closing %s(...)", first_ident);
                sql__advance(p);
                item.fn = fn;
                has_aggregate = 1;
                snprintf(item.alias, sizeof(item.alias), "%.100s(%.100s)", first_ident, item.arg_col);
            } else {
                item.fn = SQL_AGG_NONE;
                snprintf(item.arg_col, sizeof(item.arg_col), "%s", first_ident);
                snprintf(item.alias, sizeof(item.alias), "%s", first_ident);
            }

            if (sql__kw_is(&p->cur, "AS")) {
                sql__advance(p);
                const char *aliasIdent;
                if (!sql__ident_text(&p->cur, &aliasIdent)) return sql__err("expected an alias name after AS");
                snprintf(item.alias, sizeof(item.alias), "%.255s", aliasIdent);
                sql__advance(p);
            }

            if (nitems >= KDB_SQL_MAX_COLUMNS) return sql__err("too many selected columns (max %d)", KDB_SQL_MAX_COLUMNS);
            items[nitems++] = item;

            if (p->cur.type == SQLTOK_COMMA) { sql__advance(p); continue; }
            break;
        }
    }

    if (!sql__kw_is(&p->cur, "FROM")) return sql__err("expected FROM after the SELECT column list");
    sql__advance(p);

    const char *tname;
    if (!sql__ident_text(&p->cur, &tname)) return sql__err("expected a table name after FROM");
    char table_name[KDB_SQL_IDENT_BUF];
    snprintf(table_name, sizeof(table_name), "%.255s", tname);
    sql__advance(p);

    char alias1[KDB_SQL_IDENT_BUF];
    snprintf(alias1, sizeof(alias1), "%s", table_name);
    if (sql__kw_is(&p->cur, "AS")) {
        sql__advance(p);
        const char *aname;
        if (!sql__ident_text(&p->cur, &aname)) return sql__err("expected an alias after AS");
        snprintf(alias1, sizeof(alias1), "%.255s", aname);
        sql__advance(p);
    }

    int  has_join = 0, is_left = 0;
    char table2[KDB_SQL_IDENT_BUF] = "";
    char alias2[KDB_SQL_IDENT_BUF] = "";
    SqlJoinCond join_conds[KDB_SQL_MAX_JOIN_COND];
    int  njoin_cond = 0;

    if (sql__kw_is(&p->cur, "JOIN") || sql__kw_is(&p->cur, "INNER") || sql__kw_is(&p->cur, "LEFT")) {
        if (sql__kw_is(&p->cur, "LEFT")) {
            is_left = 1;
            sql__advance(p);
            if (sql__kw_is(&p->cur, "OUTER")) sql__advance(p);
        } else if (sql__kw_is(&p->cur, "INNER")) {
            sql__advance(p);
        }
        if (!sql__kw_is(&p->cur, "JOIN")) return sql__err("expected JOIN after %s", is_left ? "LEFT" : "INNER");
        sql__advance(p);

        const char *t2name;
        if (!sql__ident_text(&p->cur, &t2name)) return sql__err("expected a table name after JOIN");
        snprintf(table2, sizeof(table2), "%.255s", t2name);
        snprintf(alias2, sizeof(alias2), "%s", table2);
        sql__advance(p);

        if (sql__kw_is(&p->cur, "AS")) {
            sql__advance(p);
            const char *aname;
            if (!sql__ident_text(&p->cur, &aname)) return sql__err("expected an alias after AS");
            snprintf(alias2, sizeof(alias2), "%.255s", aname);
            sql__advance(p);
        }

        if (!sql__kw_is(&p->cur, "ON")) return sql__err("expected ON after JOIN %s", table2);
        sql__advance(p);
        if (!sql__parse_join_on(p, join_conds, &njoin_cond)) return kdb_last_status();

        if (strcmp(alias1, alias2) == 0)
            return sql__err("JOIN needs distinct table names or aliases -- both sides are '%s'", alias1);

        has_join = 1;
    }

    char *filters[KDB_SQL_MAX_COND];
    int   nfilt = 0;
    if (!sql__parse_where(p, db, filters, &nfilt)) return kdb_last_status();

    char group_col[KDB_SQL_IDENT_BUF] = "";
    int  has_group_by = 0;
    if (sql__kw_is(&p->cur, "GROUP")) {
        sql__advance(p);
        if (!sql__kw_is(&p->cur, "BY")) { sql__free_filters(filters, nfilt); return sql__err("expected BY after GROUP"); }
        sql__advance(p);
        const char *gcol;
        if (!sql__ident_text(&p->cur, &gcol)) { sql__free_filters(filters, nfilt); return sql__err("expected a column name after GROUP BY"); }
        snprintf(group_col, sizeof(group_col), "%.255s", gcol);
        has_group_by = 1;
        sql__advance(p);
    }

    if (has_join && (has_aggregate || has_group_by)) {
        sql__free_filters(filters, nfilt);
        return sql__err("JOIN doesn't support GROUP BY or aggregate functions yet");
    }

    if (project_all && has_group_by) {
        sql__free_filters(filters, nfilt);
        return sql__err("can't use '*' with GROUP BY -- list the columns/aggregates you want");
    }
    if (!project_all && (has_aggregate || has_group_by)) {
        for (uint32_t i = 0; i < nitems; i++) {
            if (items[i].fn == SQL_AGG_NONE &&
                (!has_group_by || strcmp(items[i].arg_col, group_col) != 0)) {
                sql__free_filters(filters, nfilt);
                return sql__err("column '%s' must appear in GROUP BY or be used inside an aggregate function",
                                items[i].arg_col);
            }
        }
    }

    char *having_filters[KDB_SQL_MAX_COND];
    int   nhaving = 0;
    if (sql__kw_is(&p->cur, "HAVING")) {
        if (!has_aggregate && !has_group_by) {
            sql__free_filters(filters, nfilt);
            return sql__err("HAVING requires GROUP BY or an aggregate function -- use WHERE to filter plain columns");
        }
        if (!sql__parse_having(p, db, having_filters, &nhaving)) { sql__free_filters(filters, nfilt); return kdb_last_status(); }
    }

    const char *filter_ptrs[KDB_SQL_MAX_COND + 1];
    for (int i = 0; i < nfilt; i++) filter_ptrs[i] = filters[i];
    filter_ptrs[nfilt] = NULL;

    if (has_aggregate || has_group_by) {
        KdbRows *all = kdb_find_ex(db, table_name, nfilt > 0 ? filter_ptrs : NULL, NULL);
        sql__free_filters(filters, nfilt);
        if (!all) { sql__free_filters(having_filters, nhaving); return kdb_last_status(); }

        KdbRows *agg = NULL;
        KdbStatus ast = sql__compute_aggregates(all, items, nitems, has_group_by, group_col, &agg);
        kdb_rows_free(all);
        if (ast != KDB_OK) { sql__free_filters(having_filters, nhaving); return ast; }

        if (nhaving > 0) sql__filter_rows(agg, having_filters, nhaving);
        sql__free_filters(having_filters, nhaving);
        if (distinct) sql__dedupe_rows(agg);

        if (rows_out) *rows_out = agg;
        else           kdb_rows_free(agg);
        return KDB_OK;
    }

    KdbRows *rows = NULL;
    if (has_join) {
        KdbStatus jst = sql__build_joined_rows(db, table_name, alias1, table2, alias2, is_left,
                                               join_conds, njoin_cond, &rows);
        if (jst != KDB_OK) { sql__free_filters(filters, nfilt); return jst; }
        if (nfilt > 0) sql__filter_rows(rows, filters, nfilt);
        sql__free_filters(filters, nfilt);
    } else {
        rows = kdb_find_ex(db, table_name, nfilt > 0 ? filter_ptrs : NULL, NULL);
        sql__free_filters(filters, nfilt);
        if (!rows) return kdb_last_status();
    }

    if (!project_all) {
        char proj_cols[KDB_SQL_MAX_COLUMNS][KDB_SQL_IDENT_BUF];
        for (uint32_t i = 0; i < nitems; i++)
            snprintf(proj_cols[i], sizeof(proj_cols[0]), "%s", items[i].arg_col);
        KdbStatus pst = sql__project_rows(rows, proj_cols, nitems);
        if (pst != KDB_OK) { kdb_rows_free(rows); return pst; }
    }

    if (distinct) sql__dedupe_rows(rows);

    if (rows_out) *rows_out = rows;
    else           kdb_rows_free(rows);
    return KDB_OK;
}

/* Renames every row's fields to match tmpl's names positionally (frees the
 * row's own name strings first). Used so a UNION's combined output has one
 * consistent set of column names regardless of which arm's alias a given
 * row actually came from -- same as real SQL, which names a UNION's output
 * columns after its first SELECT. Caller has already checked field counts
 * match. Returns 0 on OOM (rows may be left partially renamed either way,
 * safe to free). */
static int sql__rename_rows_like(KdbRows *rows, const KdbRow *tmpl) {
    for (size_t r = 0; r < rows->count; r++) {
        KdbRow *row = &rows->rows[r];
        for (uint32_t i = 0; i < row->field_count; i++) {
            char *new_name = strdup(tmpl->fields[i].name);
            if (!new_name) return 0;
            free((void *)row->fields[i].name);
            row->fields[i].name = new_name;
        }
    }
    return 1;
}

/* Appends every row of src into dst (dst takes ownership, src's row array
 * is freed but individual rows are not -- they now live in dst). Returns
 * 0 on OOM (dst is left with whatever prefix it already had, still safe to
 * use/free; src is left untouched so the caller can still free it). */
static int sql__append_rows(KdbRows *dst, KdbRows *src) {
    if (src->count == 0) { free(src->rows); return 1; }
    KdbRow *grown = (KdbRow *)realloc(dst->rows, (dst->count + src->count) * sizeof(KdbRow));
    if (!grown) return 0;
    dst->rows = grown;
    memcpy(dst->rows + dst->count, src->rows, src->count * sizeof(KdbRow));
    dst->count += src->count;
    free(src->rows);
    return 1;
}

/* SELECT, plus an optional chain of UNION/UNION ALL SELECT arms, plus one
 * final ORDER BY/LIMIT applying to the combined result -- same grammar
 * real SQL uses (ORDER BY/LIMIT can only appear once, after the last arm).
 * Mixing UNION and UNION ALL in the same chain isn't supported (which one
 * binds first is a real ambiguity without parenthesized subqueries); pick
 * one for the whole statement. Column names in the output come from the
 * first arm that returned at least one row. */
static KdbStatus sql__exec_select_stmt(SqlParser *p, KumDB *db, KdbRows **rows_out) {
    KdbRows *acc = NULL;
    KdbStatus st = sql__exec_select_core(p, db, &acc);
    if (st != KDB_OK) return st;

    int seen_all = -1; /* -1 = no UNION yet, 0 = plain UNION seen, 1 = UNION ALL seen */
    long acc_shape = acc->count > 0 ? (long)acc->rows[0].field_count : -1;

    while (sql__kw_is(&p->cur, "UNION")) {
        sql__advance(p);
        int is_all = 0;
        if (sql__kw_is(&p->cur, "ALL")) { is_all = 1; sql__advance(p); }

        if (seen_all != -1 && seen_all != is_all) {
            kdb_rows_free(acc);
            return sql__err("can't mix UNION and UNION ALL in the same statement");
        }
        seen_all = is_all;

        if (!sql__kw_is(&p->cur, "SELECT")) { kdb_rows_free(acc); return sql__err("expected SELECT after UNION [ALL]"); }

        KdbRows *next = NULL;
        st = sql__exec_select_core(p, db, &next);
        if (st != KDB_OK) { kdb_rows_free(acc); return st; }

        if (next->count > 0) {
            long next_shape = (long)next->rows[0].field_count;
            if (acc_shape != -1 && next_shape != acc_shape) {
                kdb_rows_free(acc);
                kdb_rows_free(next);
                return sql__err("UNION arms must select the same number of columns");
            }
            if (acc_shape == -1) {
                acc_shape = next_shape;
            } else if (!sql__rename_rows_like(next, &acc->rows[0])) {
                kdb_rows_free(acc);
                kdb_rows_free(next);
                kdb_err_oom("UNION row rename");
                return KDB_ERR_OOM;
            }
        }

        if (!sql__append_rows(acc, next)) {
            kdb_rows_free(acc);
            free(next);
            kdb_err_oom("UNION row append");
            return KDB_ERR_OOM;
        }
        free(next);

        if (!is_all) sql__dedupe_rows(acc);
    }

    KdbFindOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.ascending = 1;
    char order_col[KDB_SQL_IDENT_BUF];
    order_col[0] = '\0';

    if (sql__kw_is(&p->cur, "ORDER")) {
        sql__advance(p);
        if (!sql__kw_is(&p->cur, "BY")) { kdb_rows_free(acc); return sql__err("expected BY after ORDER"); }
        sql__advance(p);
        const char *ocol;
        if (!sql__ident_text(&p->cur, &ocol)) { kdb_rows_free(acc); return sql__err("expected a column name after ORDER BY"); }
        snprintf(order_col, sizeof(order_col), "%.255s", ocol);
        opts.order_by = order_col;
        sql__advance(p);
        if (sql__kw_is(&p->cur, "ASC"))       { sql__advance(p); opts.ascending = 1; }
        else if (sql__kw_is(&p->cur, "DESC")) { sql__advance(p); opts.ascending = 0; }
    }

    if (sql__kw_is(&p->cur, "LIMIT")) {
        sql__advance(p);
        if (p->cur.type != SQLTOK_NUMBER) { kdb_rows_free(acc); return sql__err("expected a number after LIMIT"); }
        opts.limit = (size_t)atoll(p->cur.text);
        sql__advance(p);
        if (sql__kw_is(&p->cur, "OFFSET")) {
            sql__advance(p);
            if (p->cur.type != SQLTOK_NUMBER) { kdb_rows_free(acc); return sql__err("expected a number after OFFSET"); }
            opts.offset = (size_t)atoll(p->cur.text);
            sql__advance(p);
        }
    }

    if (opts.order_by) sql__sort_rows(acc, opts.order_by, opts.ascending);
    if (opts.offset > 0 || opts.limit > 0) sql__limit_rows(acc, opts.offset, opts.limit);

    if (rows_out) *rows_out = acc;
    else           kdb_rows_free(acc);
    return KDB_OK;
}

static KdbStatus sql__exec_update(SqlParser *p, KumDB *db, size_t *affected_out) {
    sql__advance(p); /* UPDATE */
    const char *tname;
    if (!sql__ident_text(&p->cur, &tname)) return sql__err("expected a table name after UPDATE");
    char table_name[KDB_SQL_IDENT_BUF];
    snprintf(table_name, sizeof(table_name), "%.255s", tname);
    sql__advance(p);

    if (!sql__kw_is(&p->cur, "SET")) return sql__err("expected SET after UPDATE %s", table_name);
    sql__advance(p);

    char set_names[KDB_SQL_MAX_COLUMNS][KDB_SQL_IDENT_BUF];
    SqlToken set_vals[KDB_SQL_MAX_COLUMNS];
    uint32_t nset = 0;

    for (;;) {
        const char *cname;
        if (!sql__ident_text(&p->cur, &cname)) return sql__err("expected a column name after SET");
        if (nset >= KDB_SQL_MAX_COLUMNS) return sql__err("too many SET assignments (max %d)", KDB_SQL_MAX_COLUMNS);
        snprintf(set_names[nset], sizeof(set_names[0]), "%.255s", cname);
        sql__advance(p);
        if (p->cur.type != SQLTOK_EQ) return sql__err("expected '=' after column '%s' in SET", set_names[nset]);
        sql__advance(p);
        set_vals[nset] = p->cur;
        sql__advance(p);
        nset++;
        if (p->cur.type == SQLTOK_COMMA) { sql__advance(p); continue; }
        break;
    }

    char *filters[KDB_SQL_MAX_COND];
    int   nfilt = 0;
    if (!sql__parse_where(p, db, filters, &nfilt)) return kdb_last_status();

    KdbField patch[KDB_SQL_MAX_COLUMNS + 1];
    for (uint32_t i = 0; i < nset; i++) {
        if (!sql__value_to_field(set_names[i], &set_vals[i], &patch[i])) {
            sql__free_filters(filters, nfilt);
            return sql__err("unsupported value for column '%s' in SET", set_names[i]);
        }
    }
    patch[nset] = kdb_field_end();

    const char *filter_ptrs[KDB_SQL_MAX_COND + 1];
    for (int i = 0; i < nfilt; i++) filter_ptrs[i] = filters[i];
    filter_ptrs[nfilt] = NULL;

    size_t updated = 0;
    KdbStatus st = kdb_update(db, table_name, nfilt > 0 ? filter_ptrs : NULL, patch, &updated);
    sql__free_filters(filters, nfilt);
    if (st == KDB_OK && affected_out) *affected_out = updated;
    return st;
}

static KdbStatus sql__exec_delete(SqlParser *p, KumDB *db, size_t *affected_out) {
    sql__advance(p); /* DELETE */
    if (!sql__kw_is(&p->cur, "FROM")) return sql__err("expected FROM after DELETE");
    sql__advance(p);
    const char *tname;
    if (!sql__ident_text(&p->cur, &tname)) return sql__err("expected a table name after DELETE FROM");
    char table_name[KDB_SQL_IDENT_BUF];
    snprintf(table_name, sizeof(table_name), "%.255s", tname);
    sql__advance(p);

    char *filters[KDB_SQL_MAX_COND];
    int   nfilt = 0;
    if (!sql__parse_where(p, db, filters, &nfilt)) return kdb_last_status();

    const char *filter_ptrs[KDB_SQL_MAX_COND + 1];
    for (int i = 0; i < nfilt; i++) filter_ptrs[i] = filters[i];
    filter_ptrs[nfilt] = NULL;

    size_t deleted = 0;
    KdbStatus st = kdb_delete(db, table_name, nfilt > 0 ? filter_ptrs : NULL, &deleted);
    sql__free_filters(filters, nfilt);
    if (st == KDB_OK && affected_out) *affected_out = deleted;
    return st;
}

KdbStatus kdb_exec_sql(KumDB *db, const char *sql, KdbRows **rows_out, size_t *affected_out) {
    if (!db || !sql) {
        kdb_err_null_arg("db/sql", "kdb_exec_sql");
        return KDB_ERR_BAD_ARG;
    }

    SqlParser p;
    sql__init(&p, sql);

    if (p.cur.type == SQLTOK_EOF) return sql__err("empty SQL statement");
    if (p.cur.type == SQLTOK_ERROR) return sql__err("couldn't even tokenize this -- check for an unterminated string or a stray character");

    KdbStatus st;
    if      (sql__kw_is(&p.cur, "CREATE")) st = sql__exec_create_table(&p, db);
    else if (sql__kw_is(&p.cur, "ALTER"))  st = sql__exec_alter_table(&p, db);
    else if (sql__kw_is(&p.cur, "DROP"))   st = sql__exec_drop_table(&p, db);
    else if (sql__kw_is(&p.cur, "INSERT")) st = sql__exec_insert(&p, db, affected_out);
    else if (sql__kw_is(&p.cur, "SELECT")) st = sql__exec_select_stmt(&p, db, rows_out);
    else if (sql__kw_is(&p.cur, "UPDATE")) st = sql__exec_update(&p, db, affected_out);
    else if (sql__kw_is(&p.cur, "DELETE")) st = sql__exec_delete(&p, db, affected_out);
    else return sql__err("unrecognized statement -- expected CREATE, ALTER, DROP, INSERT, SELECT, UPDATE, or DELETE");

    if (st != KDB_OK) return st;

    if (p.cur.type == SQLTOK_SEMI) sql__advance(&p);
    if (p.cur.type != SQLTOK_EOF)
        return sql__err("unexpected trailing content after the statement -- one statement per call");

    return KDB_OK;
}
