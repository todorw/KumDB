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

/* Words that can legitimately follow a FROM/JOIN target, and so are
 * ambiguous with a bare (no-AS) alias otherwise -- "FROM t WHERE ..." must
 * parse WHERE as the WHERE clause, not as t's alias. This lexer doesn't
 * distinguish keyword tokens from identifier tokens at all (both are just
 * SQLTOK_IDENT; sql__kw_is is a text comparison, not a token-type check),
 * so bare aliasing needs its own explicit reserved-word check -- scoped to
 * exactly the keywords that can appear in this position, not a general
 * reserved-word list. */
static int sql__is_clause_keyword(const char *text) {
    static const char *kws[] = {
        "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT", "UNION",
        "JOIN", "INNER", "LEFT", "ON", "AS", NULL
    };
    for (int i = 0; kws[i]; i++)
        if (strcasecmp(text, kws[i]) == 0) return 1;
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

/* A parsed WHERE/HAVING condition, as a tree instead of a flat list --
 * lets "(a=1 OR b=2) AND c=3" mean what it looks like, instead of standard
 * SQL's AND-binds-tighter-than-OR flattening being the only option. Leaves
 * reuse sql__parse_condition()'s filter-string format unchanged (no "OR:"
 * prefix at this level -- that convention only matters for the flattened
 * form below, which the storage engine's OR'd-AND-groups model needs).
 * SQL_COND_EXISTS/SQL_COND_NOT_EXISTS are a different kind of leaf: their
 * leaf_filter holds the inner SELECT's raw source text (same convention
 * CREATE VIEW stores a view's query as -- see sql__exec_create_view),
 * re-parsed and re-run once per outer row by sql__cond_tree_matches(),
 * with correlated column references substituted in first. */
typedef enum { SQL_COND_LEAF, SQL_COND_AND, SQL_COND_OR, SQL_COND_EXISTS, SQL_COND_NOT_EXISTS } SqlCondKind;

typedef struct SqlCondNode {
    SqlCondKind          kind;
    char                 *leaf_filter;   /* SQL_COND_LEAF: "col__op=val"; SQL_COND_[NOT_]EXISTS: inner SELECT text */
    struct SqlCondNode   *left;          /* AND/OR only */
    struct SqlCondNode   *right;         /* AND/OR only */
} SqlCondNode;

static void sql__free_cond_node(SqlCondNode *n) {
    if (!n) return;
    free(n->leaf_filter);
    sql__free_cond_node(n->left);
    sql__free_cond_node(n->right);
    free(n);
}

/* HAVING filters the aggregated output (SUM(...)  AS total-style aliases),
 * which EXISTS's outer-row correlation model (built around a real table's
 * own alias/columns) doesn't have a sensible meaning against -- rejected
 * at parse time instead of silently correlating against nonsense. */
static int sql__cond_tree_has_exists(const SqlCondNode *n) {
    if (!n) return 0;
    if (n->kind == SQL_COND_EXISTS || n->kind == SQL_COND_NOT_EXISTS) return 1;
    return sql__cond_tree_has_exists(n->left) || sql__cond_tree_has_exists(n->right);
}

#define KDB_SQL_MAX_COND_DEPTH 16

/* A non-JOIN row's own fields are always stored bare ("name", never
 * "u.name") -- only a JOIN's combined row gets alias-qualified field names
 * (see sql__append_qualified_fields). So a condition that qualifies its
 * own table with the query's own alias -- "FROM users u WHERE u.name=...",
 * or EXISTS's inner query qualifying its own FROM target the same way,
 * which is the natural style to write once an alias is in scope at all --
 * would otherwise silently never match. Strips exactly that one prefix
 * back to the bare column name sql__row_cond_matches actually understands;
 * a reference to any OTHER alias is left alone (and, with no JOIN to make
 * it meaningful, will simply never match -- same as always). Modifies f in
 * place (only ever shrinks it) and returns it; alias may be NULL (a JOIN
 * is present, so no query-adopted unqualifying happens -- every reference
 * must stay exactly as qualified, same as before this existed). */
static char *sql__strip_own_alias(char *f, const char *alias) {
    if (!alias) return f;
    size_t alen = strlen(alias);
    if (strncmp(f, alias, alen) != 0 || f[alen] != '.') return f;
    memmove(f, f + alen + 1, strlen(f + alen + 1) + 1);
    return f;
}

static SqlCondNode *sql__parse_or_expr(SqlParser *p, KumDB *db, const char *unqualify_alias, int *used_parens, int *leaf_count, int depth);

/* Advances p past a parenthesized SELECT without executing or semantically
 * validating it -- EXISTS/NOT EXISTS's inner query can reference an outer
 * alias that isn't a real table of its own, so it can't be parsed and run
 * for real up front the way CREATE VIEW's body is. This only needs to find
 * where the statement ends, using the same lexer everything else here
 * does (so it can't be fooled by a stray '(' or ')' inside a string
 * literal). p must be positioned at the first token after the '(' the
 * caller already consumed (i.e. at "SELECT"). Leaves p positioned at the
 * matching ')' itself, not consumed. Returns 0 on unexpected EOF (error
 * already set). */
static int sql__skip_parenthesized_select(SqlParser *p) {
    int depth = 1;
    for (;;) {
        if (p->cur.type == SQLTOK_EOF) { sql__err("unexpected end of input inside EXISTS (...)"); return 0; }
        if (p->cur.type == SQLTOK_LPAREN) depth++;
        else if (p->cur.type == SQLTOK_RPAREN) {
            depth--;
            if (depth == 0) return 1;
        }
        sql__advance(p);
    }
}

/* EXISTS (SELECT ...) | NOT EXISTS (SELECT ...) | '(' expr ')' |
 * leaf-condition. Bumps *used_parens the moment a '(' is seen anywhere in
 * the clause, or an EXISTS/NOT EXISTS leaf is parsed -- callers use that
 * to tell whether the tree still has the classic "OR'd AND-groups" shape
 * (the flat parenless, EXISTS-less case, guaranteed by the grammar below)
 * or needs real tree evaluation (parens can't flatten; EXISTS has no flat
 * filter-string form at all). */
static SqlCondNode *sql__parse_cond_primary(SqlParser *p, KumDB *db, const char *unqualify_alias, int *used_parens, int *leaf_count, int depth) {
    int negate_exists = 0;
    if (sql__kw_is(&p->cur, "NOT")) {
        sql__advance(p);
        if (!sql__kw_is(&p->cur, "EXISTS")) { sql__err("expected EXISTS after NOT"); return NULL; }
        negate_exists = 1;
    }
    if (sql__kw_is(&p->cur, "EXISTS")) {
        sql__advance(p);
        if (p->cur.type != SQLTOK_LPAREN) { sql__err("expected '(' after EXISTS"); return NULL; }
        sql__advance(p);
        if (!sql__kw_is(&p->cur, "SELECT")) { sql__err("expected a SELECT statement inside EXISTS (...)"); return NULL; }
        size_t start = p->lx.pos - strlen(p->cur.text);
        if (!sql__skip_parenthesized_select(p)) return NULL;
        size_t end = p->lx.pos - 1; /* p->cur is the ')' itself (empty token text, pos is 1 past it) */
        while (end > start && isspace((unsigned char)p->lx.src[end - 1])) end--;
        sql__advance(p); /* consume ')' */

        size_t len = end - start;
        if (len == 0) { sql__err("EXISTS (...) query is empty"); return NULL; }
        if (len >= KDB_MAX_STRING_LEN) len = KDB_MAX_STRING_LEN - 1;
        char *inner_sql = malloc(len + 1);
        if (!inner_sql) { kdb_err_oom("EXISTS subquery text"); return NULL; }
        memcpy(inner_sql, p->lx.src + start, len);
        inner_sql[len] = '\0';

        SqlCondNode *n = KDB_ALLOC(SqlCondNode);
        if (!n) { kdb_err_oom("condition tree node"); free(inner_sql); return NULL; }
        n->kind = negate_exists ? SQL_COND_NOT_EXISTS : SQL_COND_EXISTS;
        n->leaf_filter = inner_sql;
        *used_parens = 1; /* EXISTS has no flat filter-string form -- forces tree evaluation, same as real parens */
        (*leaf_count)++;
        return n;
    }

    if (p->cur.type == SQLTOK_LPAREN) {
        if (depth + 1 > KDB_SQL_MAX_COND_DEPTH) {
            sql__err("condition nested too deeply (max depth %d)", KDB_SQL_MAX_COND_DEPTH);
            return NULL;
        }
        sql__advance(p);
        *used_parens = 1;
        SqlCondNode *inner = sql__parse_or_expr(p, db, unqualify_alias, used_parens, leaf_count, depth + 1);
        if (!inner) return NULL;
        if (p->cur.type != SQLTOK_RPAREN) {
            sql__err("expected ')' closing a parenthesized condition");
            sql__free_cond_node(inner);
            return NULL;
        }
        sql__advance(p);
        return inner;
    }

    if (*leaf_count >= KDB_SQL_MAX_COND) {
        sql__err("too many conditions (max %d)", KDB_SQL_MAX_COND);
        return NULL;
    }
    char *f = sql__parse_condition(p, db);
    if (!f) return NULL;
    f = sql__strip_own_alias(f, unqualify_alias);
    SqlCondNode *n = KDB_ALLOC(SqlCondNode);
    if (!n) { kdb_err_oom("condition tree node"); free(f); return NULL; }
    n->kind = SQL_COND_LEAF;
    n->leaf_filter = f;
    (*leaf_count)++;
    return n;
}

/* AND binds tighter than OR, same as standard SQL. */
static SqlCondNode *sql__parse_and_expr(SqlParser *p, KumDB *db, const char *unqualify_alias, int *used_parens, int *leaf_count, int depth) {
    SqlCondNode *left = sql__parse_cond_primary(p, db, unqualify_alias, used_parens, leaf_count, depth);
    if (!left) return NULL;
    while (sql__kw_is(&p->cur, "AND")) {
        sql__advance(p);
        SqlCondNode *right = sql__parse_cond_primary(p, db, unqualify_alias, used_parens, leaf_count, depth);
        if (!right) { sql__free_cond_node(left); return NULL; }
        SqlCondNode *n = KDB_ALLOC(SqlCondNode);
        if (!n) { kdb_err_oom("condition tree node"); sql__free_cond_node(left); sql__free_cond_node(right); return NULL; }
        n->kind = SQL_COND_AND;
        n->left = left;
        n->right = right;
        left = n;
    }
    return left;
}

static SqlCondNode *sql__parse_or_expr(SqlParser *p, KumDB *db, const char *unqualify_alias, int *used_parens, int *leaf_count, int depth) {
    SqlCondNode *left = sql__parse_and_expr(p, db, unqualify_alias, used_parens, leaf_count, depth);
    if (!left) return NULL;
    while (sql__kw_is(&p->cur, "OR")) {
        sql__advance(p);
        SqlCondNode *right = sql__parse_and_expr(p, db, unqualify_alias, used_parens, leaf_count, depth);
        if (!right) { sql__free_cond_node(left); return NULL; }
        SqlCondNode *n = KDB_ALLOC(SqlCondNode);
        if (!n) { kdb_err_oom("condition tree node"); sql__free_cond_node(left); sql__free_cond_node(right); return NULL; }
        n->kind = SQL_COND_OR;
        n->left = left;
        n->right = right;
        left = n;
    }
    return left;
}

/* unqualify_alias is the query's own FROM alias when it has no JOIN (see
 * sql__strip_own_alias), or NULL when it does (a JOIN needs every
 * reference to stay exactly as qualified -- there's more than one table
 * it could mean). *tree_out comes back NULL if there's no WHERE/HAVING
 * clause at all (not an error). *used_parens comes back 0 iff the clause
 * never used '(' -- in that case the grammar above guarantees the tree is
 * exactly one left-nested AND-chain per OR-branch, the same shape the old
 * flat parser produced, so sql__cond_flatten() below can losslessly turn
 * it back into the "OR:"-prefixed filter-string array the storage engine
 * understands and the fast pushdown path (no full-table fetch) keeps
 * working unchanged. Shared by WHERE and HAVING -- the keyword itself is
 * consumed by the caller. Returns 0 on error (error already set), 1 on
 * success. */
static int sql__parse_cond_clause(SqlParser *p, KumDB *db, const char *unqualify_alias, SqlCondNode **tree_out, int *used_parens) {
    int leaf_count = 0;
    *used_parens = 0;
    SqlCondNode *t = sql__parse_or_expr(p, db, unqualify_alias, used_parens, &leaf_count, 0);
    if (!t) { *tree_out = NULL; return 0; }
    *tree_out = t;
    return 1;
}

/* unqualify_alias: see sql__parse_cond_clause. Returns 0 on error (error
 * already set), 1 on success. */
static int sql__parse_where_expr(SqlParser *p, KumDB *db, const char *unqualify_alias, SqlCondNode **tree_out, int *used_parens) {
    *tree_out = NULL;
    *used_parens = 0;
    if (!sql__kw_is(&p->cur, "WHERE")) return 1;
    sql__advance(p);
    return sql__parse_cond_clause(p, db, unqualify_alias, tree_out, used_parens);
}

/* unqualify_alias: see sql__parse_cond_clause. Returns 0 on error (error
 * already set), 1 on success. */
static int sql__parse_having_expr(SqlParser *p, KumDB *db, const char *unqualify_alias, SqlCondNode **tree_out, int *used_parens) {
    *tree_out = NULL;
    *used_parens = 0;
    if (!sql__kw_is(&p->cur, "HAVING")) return 1;
    sql__advance(p);
    return sql__parse_cond_clause(p, db, unqualify_alias, tree_out, used_parens);
}

static int sql__cond_flatten_leaf(const SqlCondNode *node, char *filters_buf[KDB_SQL_MAX_COND], int *n, int is_or_start) {
    if (*n >= KDB_SQL_MAX_COND) { sql__err("too many conditions (max %d)", KDB_SQL_MAX_COND); return 0; }
    const char *src = node->leaf_filter;
    size_t need = strlen(src) + (is_or_start ? 4 : 1);
    char *f = malloc(need);
    if (!f) { kdb_err_oom("flattened filter string"); return 0; }
    if (is_or_start) snprintf(f, need, "OR:%s", src);
    else             snprintf(f, need, "%s", src);
    filters_buf[(*n)++] = f;
    return 1;
}

/* node is guaranteed (by the no-parens invariant) to be either a single
 * leaf or a left-nested AND-chain of leaves -- i.e. one whole OR-group. */
static int sql__cond_flatten_and_chain(const SqlCondNode *node, char *filters_buf[KDB_SQL_MAX_COND], int *n, int is_or_start) {
    if (node->kind == SQL_COND_AND) {
        if (!sql__cond_flatten_and_chain(node->left, filters_buf, n, is_or_start)) return 0;
        return sql__cond_flatten_leaf(node->right, filters_buf, n, 0);
    }
    return sql__cond_flatten_leaf(node, filters_buf, n, is_or_start);
}

static int sql__cond_flatten_or(const SqlCondNode *node, char *filters_buf[KDB_SQL_MAX_COND], int *n) {
    if (node->kind == SQL_COND_OR) {
        if (!sql__cond_flatten_or(node->left, filters_buf, n)) return 0;
        return sql__cond_flatten_and_chain(node->right, filters_buf, n, 1);
    }
    return sql__cond_flatten_and_chain(node, filters_buf, n, 0);
}

/* Only valid to call when the tree was parsed with used_parens == 0. On
 * success, filters_buf and n (out) are byte-for-byte what the old flat
 * parser would have produced for the same clause. Returns 0 on error
 * (error already set, n left however far it got -- caller should still
 * free what's there via sql__free_filters). */
static int sql__cond_flatten(const SqlCondNode *node, char *filters_buf[KDB_SQL_MAX_COND], int *n) {
    *n = 0;
    if (!node) return 1;
    return sql__cond_flatten_or(node, filters_buf, n);
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

/* Views are stored as rows in a reserved internal table -- reuses the
 * engine's existing durability/atomicity for free instead of inventing a
 * second on-disk format just for view definitions. Not meant to be a real
 * user table name; don't create one. */
#define KDB_VIEWS_TABLE "__kumdb_views__"

/* True if a view named 'name' is registered -- doesn't distinguish "no
 * views table yet" from "views table exists but no matching row", both
 * just mean "not a view". */
static int sql__view_exists(KumDB *db, const char *name) {
    if (!kdb_table_exists(db, KDB_VIEWS_TABLE)) return 0;
    char filter_buf[KDB_SQL_IDENT_BUF + 8];
    snprintf(filter_buf, sizeof(filter_buf), "name=%s", name);
    const char *filters[] = { filter_buf, NULL };
    return kdb_count(db, KDB_VIEWS_TABLE, filters) > 0;
}

/* Copies a view's stored SELECT text into out. out_size is expected to be
 * KDB_MAX_STRING_LEN (the storage layer's own string-field cap, so a
 * stored view's query can never legitimately be longer than that anyway)
 * -- a fixed caller-owned buffer instead of a heap pointer, on purpose:
 * sql__exec_select_core has a lot of early-return paths already, and this
 * way there's nothing to free on any of them. Returns 1 if 'name' is a
 * registered view, 0 otherwise (not an error either way). */
static int sql__lookup_view(KumDB *db, const char *name, char *out, size_t out_size) {
    if (!kdb_table_exists(db, KDB_VIEWS_TABLE)) return 0;
    char filter_buf[KDB_SQL_IDENT_BUF + 8];
    snprintf(filter_buf, sizeof(filter_buf), "name=%s", name);
    const char *filters[] = { filter_buf, NULL };
    KdbRow *row = kdb_find_one(db, KDB_VIEWS_TABLE, filters);
    if (!row) return 0;
    const char *query = NULL;
    int found = 0;
    if (kdb_row_get_string(row, "query", &query) == KDB_OK && query) {
        snprintf(out, out_size, "%s", query);
        found = 1;
    }
    kdb_row_free(row);
    return found;
}

/* CREATE VIEW name AS SELECT ... -- validates the underlying SELECT by
 * actually running it (so a typo or a reference to a table that doesn't
 * exist yet fails at CREATE VIEW time, not at first use), then stores its
 * raw source text (not a parsed/serialized form -- just remembers where
 * "SELECT" started in the original string and re-parses fresh on every
 * use). p is positioned right after "CREATE VIEW" (both already
 * consumed by the caller). */
static KdbStatus sql__exec_create_view(SqlParser *p, KumDB *db) {
    const char *vname;
    if (!sql__ident_text(&p->cur, &vname)) return sql__err("expected a view name after CREATE VIEW");
    char view_name[KDB_SQL_IDENT_BUF];
    snprintf(view_name, sizeof(view_name), "%.255s", vname);
    sql__advance(p);

    if (strcmp(view_name, KDB_VIEWS_TABLE) == 0)
        return sql__err("'%s' is reserved for KumDB's internal view registry", view_name);
    if (kdb_table_exists(db, view_name))
        return sql__err("'%s' is already a table -- can't create a view with the same name", view_name);
    if (sql__view_exists(db, view_name))
        return sql__err("view '%s' already exists -- DROP VIEW it first", view_name);

    if (!sql__kw_is(&p->cur, "AS")) return sql__err("expected AS after CREATE VIEW %s", view_name);
    sql__advance(p);

    if (!sql__kw_is(&p->cur, "SELECT"))
        return sql__err("expected a SELECT statement after CREATE VIEW %s AS", view_name);

    size_t select_start = p->lx.pos - strlen(p->cur.text);

    KdbStatus vst = sql__exec_select_stmt(p, db, NULL);
    if (vst != KDB_OK) return vst;

    if (p->cur.type != SQLTOK_SEMI && p->cur.type != SQLTOK_EOF)
        return sql__err("unexpected trailing content after CREATE VIEW %s's SELECT", view_name);

    size_t select_end = (p->cur.type == SQLTOK_SEMI) ? (p->lx.pos - 1) : p->lx.pos;
    size_t len = select_end > select_start ? select_end - select_start : 0;
    while (len > 0 && isspace((unsigned char)p->lx.src[select_start + len - 1])) len--;
    if (len == 0) return sql__err("CREATE VIEW %s's query is empty", view_name);

    char query_text[KDB_MAX_STRING_LEN];
    if (len >= sizeof(query_text)) len = sizeof(query_text) - 1;
    memcpy(query_text, p->lx.src + select_start, len);
    query_text[len] = '\0';

    KdbField fields[] = {
        kdb_field_string("name",  view_name),
        kdb_field_string("query", query_text),
        kdb_field_end()
    };
    return kdb_add(db, KDB_VIEWS_TABLE, fields);
}

/* DROP VIEW name. p is positioned right after "DROP VIEW" (both already
 * consumed by the caller). */
static KdbStatus sql__exec_drop_view(SqlParser *p, KumDB *db) {
    const char *vname;
    if (!sql__ident_text(&p->cur, &vname)) return sql__err("expected a view name after DROP VIEW");
    char view_name[KDB_SQL_IDENT_BUF];
    snprintf(view_name, sizeof(view_name), "%.255s", vname);
    sql__advance(p);

    if (!sql__view_exists(db, view_name)) return sql__err("no such view '%s'", view_name);

    char filter_buf[KDB_SQL_IDENT_BUF + 8];
    snprintf(filter_buf, sizeof(filter_buf), "name=%s", view_name);
    const char *filters[] = { filter_buf, NULL };
    size_t deleted = 0;
    return kdb_delete(db, KDB_VIEWS_TABLE, filters, &deleted);
}

static KdbStatus sql__exec_create_table(SqlParser *p, KumDB *db) {
    sql__advance(p); /* CREATE */
    if (sql__kw_is(&p->cur, "VIEW")) {
        sql__advance(p); /* VIEW */
        return sql__exec_create_view(p, db);
    }
    if (!sql__kw_is(&p->cur, "TABLE")) return sql__err("expected TABLE or VIEW after CREATE");
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
    if (sql__kw_is(&p->cur, "VIEW")) {
        sql__advance(p); /* VIEW */
        return sql__exec_drop_view(p, db);
    }
    if (!sql__kw_is(&p->cur, "TABLE")) return sql__err("expected TABLE or VIEW after DROP");
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

#define KDB_SQL_MAX_CASE_BRANCHES 4
#define KDB_SQL_CASE_VAL_BUF      512

/* A CASE branch's value is a literal, resolved once at parse time -- not a
 * heap pointer, so a SqlSelectItem carrying one stays plain-old-data and
 * every early return in sql__exec_select_core (there are a lot of them)
 * stays automatically safe with no new cleanup path to audit. */
typedef struct {
    char         cond_filter[KDB_SQL_IDENT_BUF]; /* WHEN condition, filter-string form (reuses sql__parse_condition) */
    KdbFieldType then_type;
    int64_t      then_int;
    double       then_float;
    int          then_bool;
    char         then_string[KDB_SQL_CASE_VAL_BUF];
} SqlCaseBranch;

typedef struct {
    SqlAggFn fn;                          /* SQL_AGG_NONE = plain column or CASE, not an aggregate */
    char     arg_col[KDB_SQL_IDENT_BUF];  /* column name, or "*" for COUNT(*); unused for CASE */
    char     alias[KDB_SQL_IDENT_BUF];    /* output field name */

    int           is_case;
    SqlCaseBranch case_branches[KDB_SQL_MAX_CASE_BRANCHES];
    int           n_case_branches;
    int           has_else;
    KdbFieldType  else_type;
    int64_t       else_int;
    double        else_float;
    int           else_bool;
    char          else_string[KDB_SQL_CASE_VAL_BUF];
} SqlSelectItem;

static int sql__agg_fn_from_ident(const char *s, SqlAggFn *out) {
    if (strcasecmp(s, "COUNT") == 0) { *out = SQL_AGG_COUNT; return 1; }
    if (strcasecmp(s, "SUM")   == 0) { *out = SQL_AGG_SUM;   return 1; }
    if (strcasecmp(s, "AVG")   == 0) { *out = SQL_AGG_AVG;   return 1; }
    if (strcasecmp(s, "MIN")   == 0) { *out = SQL_AGG_MIN;   return 1; }
    if (strcasecmp(s, "MAX")   == 0) { *out = SQL_AGG_MAX;   return 1; }
    return 0;
}

/* Resolves a literal token (number/string/true/false/null) into a typed
 * value stored inline, same literal shapes sql__value_to_field accepts.
 * Returns 0 for anything that isn't a literal (column refs aren't valid
 * CASE THEN/ELSE values -- keeps evaluation a pure per-row computation,
 * no second column lookup to resolve). */
static int sql__case_value_from_token(const SqlToken *t, KdbFieldType *type_out,
                                      int64_t *int_out, double *float_out, int *bool_out,
                                      char *str_out, size_t str_out_size) {
    switch (t->type) {
        case SQLTOK_NUMBER:
            if (strchr(t->text, '.')) { *type_out = KDB_TYPE_FLOAT; *float_out = atof(t->text); }
            else                      { *type_out = KDB_TYPE_INT;   *int_out = atoll(t->text); }
            return 1;
        case SQLTOK_STRING:
            *type_out = KDB_TYPE_STRING;
            /* CASE THEN/ELSE string values are capped well under a raw
             * token's max length (KDB_SQL_CASE_VAL_BUF vs KDB_SQL_TOK_MAX)
             * -- same silent-truncation convention this file already uses
             * for table/column names, not expected to matter for a CASE
             * label in practice. GCC can't prove that's safe here since
             * str_out_size is a runtime parameter, not a sizeof() at the
             * call site -- same well-known -Wformat-truncation limitation
             * as kdb__tx_backup_path in kumdb.c. */
            {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
                snprintf(str_out, str_out_size, "%s", t->text);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
            }
            return 1;
        case SQLTOK_IDENT:
            if (strcasecmp(t->text, "true")  == 0) { *type_out = KDB_TYPE_BOOL; *bool_out = 1; return 1; }
            if (strcasecmp(t->text, "false") == 0) { *type_out = KDB_TYPE_BOOL; *bool_out = 0; return 1; }
            if (strcasecmp(t->text, "null")  == 0) { *type_out = KDB_TYPE_NULL; return 1; }
            return 0;
        default:
            return 0;
    }
}

/* CASE WHEN cond THEN val [WHEN cond THEN val ...] [ELSE val] END. Each
 * WHEN condition is a single WHERE-style condition (reuses
 * sql__parse_condition -- same operators: =, BETWEEN, IN, LIKE, IS NULL,
 * etc), no AND/OR within one WHEN. p is positioned at "CASE". */
static int sql__parse_case_item(SqlParser *p, KumDB *db, SqlSelectItem *item) {
    sql__advance(p); /* CASE */
    item->is_case = 1;
    item->fn = SQL_AGG_NONE;

    if (!sql__kw_is(&p->cur, "WHEN")) { sql__err("expected WHEN after CASE"); return 0; }

    while (sql__kw_is(&p->cur, "WHEN")) {
        sql__advance(p);
        if (item->n_case_branches >= KDB_SQL_MAX_CASE_BRANCHES) {
            sql__err("too many WHEN branches in CASE (max %d)", KDB_SQL_MAX_CASE_BRANCHES);
            return 0;
        }
        SqlCaseBranch *br = &item->case_branches[item->n_case_branches];

        char *cond = sql__parse_condition(p, db);
        if (!cond) return 0;
        snprintf(br->cond_filter, sizeof(br->cond_filter), "%s", cond);
        free(cond);

        if (!sql__kw_is(&p->cur, "THEN")) { sql__err("expected THEN after a CASE WHEN condition"); return 0; }
        sql__advance(p);

        if (!sql__case_value_from_token(&p->cur, &br->then_type, &br->then_int, &br->then_float,
                                        &br->then_bool, br->then_string, sizeof(br->then_string))) {
            sql__err("expected a literal value after THEN");
            return 0;
        }
        sql__advance(p);
        item->n_case_branches++;
    }

    if (sql__kw_is(&p->cur, "ELSE")) {
        sql__advance(p);
        item->has_else = 1;
        if (!sql__case_value_from_token(&p->cur, &item->else_type, &item->else_int, &item->else_float,
                                        &item->else_bool, item->else_string, sizeof(item->else_string))) {
            sql__err("expected a literal value after ELSE");
            return 0;
        }
        sql__advance(p);
    }

    if (!sql__kw_is(&p->cur, "END")) { sql__err("expected END closing CASE"); return 0; }
    sql__advance(p);

    snprintf(item->alias, sizeof(item->alias), "case");
    return 1;
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
 * back into a structured condition -- the leaf-level building block both
 * sql__cond_tree_matches() (in-memory tree evaluation) and
 * sql__cond_flatten()'s output (pushed into kdb_find_ex/kdb_update/
 * kdb_delete) are made of. Returns 0 on a malformed string (shouldn't
 * happen, these are always our own output, but fail closed rather than
 * assert). */
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

/* A qualified reference inside EXISTS/NOT EXISTS's inner query that
 * doesn't resolve to a real column of the outer row (field missing, or a
 * type -- NULL/BLOB/ARRAY/OBJECT -- with no SQL literal syntax) gets
 * substituted with this instead of a real value. It can't equal anything
 * a real comparison would produce, so the substituted condition simply
 * never matches -- approximates real SQL's "comparing against NULL is
 * never true" behavior without needing three-valued logic. */
#define SQL_EXISTS_NO_MATCH_SENTINEL "'\x01__kumdb_no_match__\x01'"

/* col is the part of "alias.col" after the dot. Tries a direct field
 * lookup first (covers both a plain outer row's own columns and, after a
 * JOIN, the already-qualified "alias.col" pseudo/real columns the combined
 * row carries -- see sql__append_qualified_fields), then falls back to the
 * three pseudo-columns a plain (non-JOIN) row keeps outside its field list
 * entirely. Returns 0 if col isn't a column of this row at all. */
static int sql__lookup_outer_field(const KdbRow *row, const char *col, KdbField *out) {
    const KdbField *f = kdb_row_get(row, col);
    if (f) { *out = *f; return 1; }
    out->name = NULL;
    out->type = KDB_TYPE_INT;
    if      (strcmp(col, "id")         == 0) { out->v.as_int = (int64_t)row->id;         return 1; }
    else if (strcmp(col, "created_at") == 0) { out->v.as_int = (int64_t)row->created_at; return 1; }
    else if (strcmp(col, "updated_at") == 0) { out->v.as_int = (int64_t)row->updated_at; return 1; }
    return 0;
}

/* Renders f as inline SQL literal syntax (quoted for strings, with '' for
 * an embedded quote -- the lexer's own escaping convention, see
 * sql__lex_next's string case) -- NOT the same shape sql__field_to_filter_text
 * produces, which is bare unquoted text meant for a "col__op=value" filter
 * string, not something the SQL lexer could re-parse as a value token on
 * its own. Returns 0 for a type with no literal syntax (NULL/BLOB/ARRAY/
 * OBJECT) -- caller substitutes SQL_EXISTS_NO_MATCH_SENTINEL instead. */
static int sql__render_sql_literal(const KdbField *f, char *buf, size_t buf_size) {
    switch (f->type) {
        case KDB_TYPE_INT:   snprintf(buf, buf_size, "%lld", (long long)f->v.as_int); return 1;
        case KDB_TYPE_FLOAT: snprintf(buf, buf_size, "%g", f->v.as_float); return 1;
        case KDB_TYPE_BOOL:  snprintf(buf, buf_size, "%s", f->v.as_bool ? "true" : "false"); return 1;
        case KDB_TYPE_STRING: {
            size_t pos = 0;
            if (pos + 1 < buf_size) buf[pos++] = '\'';
            for (const char *s = f->v.as_string ? f->v.as_string : ""; *s && pos + 3 < buf_size; s++) {
                if (*s == '\'') { buf[pos++] = '\''; buf[pos++] = '\''; }
                else buf[pos++] = *s;
            }
            if (pos + 1 < buf_size) buf[pos++] = '\'';
            buf[pos < buf_size ? pos : buf_size - 1] = '\0';
            return 1;
        }
        default: return 0;
    }
}

/* Re-lexes inner_sql (EXISTS/NOT EXISTS's inner SELECT, verbatim source
 * text) and rebuilds it token by token, replacing every identifier of the
 * exact shape "<outer_alias>.<col>" with a literal rendering of that
 * column's value in outer_row -- this is how correlation works here: real
 * SQL scoping would thread a row context through evaluation, but every
 * other part of this SQL layer already works by building/re-parsing plain
 * query text (CREATE VIEW's stored query is the same idea), so correlated
 * EXISTS does the same thing: substitute the outer reference away, then
 * run the (now fully self-contained) inner SELECT exactly like any other.
 * Returns a heap string the caller owns, or NULL on OOM (error already
 * set) -- can't otherwise fail, an unresolvable/untyped reference just
 * becomes SQL_EXISTS_NO_MATCH_SENTINEL rather than an error. */
static char *sql__substitute_outer_refs(const char *inner_sql, const char *outer_alias, const KdbRow *outer_row) {
    size_t alias_len = strlen(outer_alias);
    size_t cap = strlen(inner_sql) + 256;
    char *out = malloc(cap);
    if (!out) { kdb_err_oom("substituted EXISTS subquery text"); return NULL; }
    size_t len = 0;
    int first = 1;

    SqlLexer lx; lx.src = inner_sql; lx.pos = 0;
    for (;;) {
        SqlToken t = sql__lex_next(&lx);
        if (t.type == SQLTOK_EOF) break;

        char piece[KDB_MAX_STRING_LEN * 2 + 16];
        if (t.type == SQLTOK_IDENT) {
            const char *dot = strchr(t.text, '.');
            if (dot && (size_t)(dot - t.text) == alias_len && strncmp(t.text, outer_alias, alias_len) == 0) {
                KdbField fld;
                if (!sql__lookup_outer_field(outer_row, dot + 1, &fld) || !sql__render_sql_literal(&fld, piece, sizeof(piece)))
                    snprintf(piece, sizeof(piece), "%s", SQL_EXISTS_NO_MATCH_SENTINEL);
            } else {
                snprintf(piece, sizeof(piece), "%s", t.text);
            }
        } else {
            switch (t.type) {
                case SQLTOK_NUMBER: snprintf(piece, sizeof(piece), "%s", t.text); break;
                case SQLTOK_STRING: {
                    size_t p = 0;
                    if (p + 1 < sizeof(piece)) piece[p++] = '\'';
                    for (const char *s = t.text; *s && p + 3 < sizeof(piece); s++) {
                        if (*s == '\'') { piece[p++] = '\''; piece[p++] = '\''; }
                        else piece[p++] = *s;
                    }
                    if (p + 1 < sizeof(piece)) piece[p++] = '\'';
                    piece[p < sizeof(piece) ? p : sizeof(piece) - 1] = '\0';
                    break;
                }
                case SQLTOK_LPAREN: snprintf(piece, sizeof(piece), "("); break;
                case SQLTOK_RPAREN: snprintf(piece, sizeof(piece), ")"); break;
                case SQLTOK_COMMA:  snprintf(piece, sizeof(piece), ","); break;
                case SQLTOK_STAR:   snprintf(piece, sizeof(piece), "*"); break;
                case SQLTOK_SEMI:   snprintf(piece, sizeof(piece), ";"); break;
                case SQLTOK_EQ:     snprintf(piece, sizeof(piece), "="); break;
                case SQLTOK_NEQ:    snprintf(piece, sizeof(piece), "!="); break;
                case SQLTOK_LT:     snprintf(piece, sizeof(piece), "<"); break;
                case SQLTOK_LTE:    snprintf(piece, sizeof(piece), "<="); break;
                case SQLTOK_GT:     snprintf(piece, sizeof(piece), ">"); break;
                case SQLTOK_GTE:    snprintf(piece, sizeof(piece), ">="); break;
                default: piece[0] = '\0'; break;
            }
        }

        size_t plen = strlen(piece);
        size_t need = len + (first ? 0 : 1) + plen + 1;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char *grown = realloc(out, cap);
            if (!grown) { kdb_err_oom("substituted EXISTS subquery text"); free(out); return NULL; }
            out = grown;
        }
        if (!first) out[len++] = ' ';
        memcpy(out + len, piece, plen);
        len += plen;
        out[len] = '\0';
        first = 0;
    }
    return out;
}

/* Runs an EXISTS/NOT EXISTS leaf for one outer row: substitute its
 * correlated references, run the resulting self-contained SELECT fresh
 * (same "no caching" philosophy views use), and check whether it returned
 * any rows. A subquery execution error fails closed (condition doesn't
 * match) rather than aborting the whole outer query -- consistent with
 * sql__parse_row_cond's own "fail closed rather than assert" precedent for
 * this file. */
static int sql__eval_exists(KumDB *db, const char *outer_alias, const KdbRow *outer_row, const char *inner_sql, int negate) {
    char *sub = sql__substitute_outer_refs(inner_sql, outer_alias, outer_row);
    if (!sub) return 0;
    SqlParser vp;
    sql__init(&vp, sub);
    KdbRows *inner = NULL;
    KdbStatus st = sql__exec_select_stmt(&vp, db, &inner);
    free(sub);
    if (st != KDB_OK) { if (inner) kdb_rows_free(inner); return 0; }
    int has_rows = inner && inner->count > 0;
    if (inner) kdb_rows_free(inner);
    return negate ? !has_rows : has_rows;
}

/* Recursive evaluation of a parsed WHERE/HAVING condition tree against one
 * in-memory row -- the general form sql__cond_flatten()'s flat "OR:"-group
 * shape is a restricted special case of. A NULL tree (no WHERE/HAVING
 * clause at all) matches everything. db/outer_alias are only used by
 * SQL_COND_[NOT_]EXISTS leaves (outer_alias is the row's own table alias,
 * for resolving "alias.col" correlated references -- see
 * sql__substitute_outer_refs); pass NULL/whatever when a tree is known not
 * to contain one (e.g. HAVING, where EXISTS is rejected at parse time). */
static int sql__cond_tree_matches(KumDB *db, const char *outer_alias, const KdbRow *row, const SqlCondNode *node) {
    if (!node) return 1;
    switch (node->kind) {
        case SQL_COND_LEAF: {
            SqlRowCond c;
            if (!sql__parse_row_cond(node->leaf_filter, &c)) return 0;
            return sql__row_cond_matches(row, &c);
        }
        case SQL_COND_AND: return sql__cond_tree_matches(db, outer_alias, row, node->left) && sql__cond_tree_matches(db, outer_alias, row, node->right);
        case SQL_COND_OR:  return sql__cond_tree_matches(db, outer_alias, row, node->left) || sql__cond_tree_matches(db, outer_alias, row, node->right);
        case SQL_COND_EXISTS:     return sql__eval_exists(db, outer_alias, row, node->leaf_filter, 0);
        case SQL_COND_NOT_EXISTS: return sql__eval_exists(db, outer_alias, row, node->leaf_filter, 1);
        default: return 0;
    }
}

/* In-place filter: drops rows that don't match, freeing their field
 * memory, preserving order of the rows that stay. Used for HAVING and
 * post-JOIN/view/parenthesized WHERE, all of which need to filter an
 * already-materialized KdbRows rather than fetch from a stored table. */
static void sql__filter_rows_tree(KumDB *db, const char *outer_alias, KdbRows *rows, const SqlCondNode *tree) {
    if (!rows || !tree) return;
    size_t kept = 0;
    for (size_t i = 0; i < rows->count; i++) {
        if (sql__cond_tree_matches(db, outer_alias, &rows->rows[i], tree)) {
            if (kept != i) rows->rows[kept] = rows->rows[i];
            kept++;
        } else {
            sql__free_row_fields(&rows->rows[i]);
        }
    }
    rows->count = kept;
}

/* Evaluates one already-parsed CASE item against a specific row -- first
 * matching WHEN wins (same short-circuit order as real SQL), ELSE (or
 * NULL if there's no ELSE) otherwise. Writes straight into *out, which the
 * caller owns (out->name is NOT set here, same convention sql__project_rows
 * uses for every other item). */
static int sql__eval_case_item(const SqlSelectItem *item, const KdbRow *row, KdbField *out) {
    for (int i = 0; i < item->n_case_branches; i++) {
        const SqlCaseBranch *br = &item->case_branches[i];
        SqlRowCond cond;
        if (!sql__parse_row_cond(br->cond_filter, &cond)) continue;
        if (!sql__row_cond_matches(row, &cond)) continue;

        out->type = br->then_type;
        switch (br->then_type) {
            case KDB_TYPE_INT:    out->v.as_int    = br->then_int;    break;
            case KDB_TYPE_FLOAT:  out->v.as_float  = br->then_float;  break;
            case KDB_TYPE_BOOL:   out->v.as_bool   = br->then_bool;   break;
            case KDB_TYPE_STRING: out->v.as_string = strdup(br->then_string); return out->v.as_string != NULL;
            default: break; /* NULL */
        }
        return 1;
    }

    if (item->has_else) {
        out->type = item->else_type;
        switch (item->else_type) {
            case KDB_TYPE_INT:    out->v.as_int    = item->else_int;    break;
            case KDB_TYPE_FLOAT:  out->v.as_float  = item->else_float;  break;
            case KDB_TYPE_BOOL:   out->v.as_bool   = item->else_bool;   break;
            case KDB_TYPE_STRING: out->v.as_string = strdup(item->else_string); return out->v.as_string != NULL;
            default: break; /* NULL */
        }
        return 1;
    }

    out->type = KDB_TYPE_NULL;
    return 1;
}

#define KDB_SQL_MAX_GROUPS     512
#define KDB_SQL_MAX_GROUP_COLS 8

typedef struct {
    /* one group-by column's value per slot, pointing into the source
     * rows; NULL = that column was missing/null on this group's rows.
     * Only the first ngroup_cols slots are used. */
    const KdbField *key_refs[KDB_SQL_MAX_GROUP_COLS];
    size_t          row_count;
    double          sum[KDB_SQL_MAX_COLUMNS];
    size_t          count_nonnull[KDB_SQL_MAX_COLUMNS];
    const KdbField *min_ref[KDB_SQL_MAX_COLUMNS];
    const KdbField *max_ref[KDB_SQL_MAX_COLUMNS];
} SqlGroupAcc;

/* One row per group (or one summary row if there's no GROUP BY), one field
 * per SELECT item. 'all' must stay alive for the whole call -- group keys
 * and MIN/MAX tracking hold pointers into its row data until the final
 * copy at the end. */
static KdbStatus sql__compute_aggregates(KdbRows *all, SqlSelectItem *items, uint32_t nitems,
                                         char group_cols[][KDB_SQL_IDENT_BUF], int ngroup_cols,
                                         KdbRows **out) {
    SqlGroupAcc *groups = (SqlGroupAcc *)calloc(KDB_SQL_MAX_GROUPS, sizeof(SqlGroupAcc));
    if (!groups) { kdb_err_oom("aggregate groups"); return KDB_ERR_OOM; }
    uint32_t ngroups = 0;

    for (size_t r = 0; r < all->count; r++) {
        KdbRow *row = &all->rows[r];
        const KdbField *key_refs[KDB_SQL_MAX_GROUP_COLS];
        for (int k = 0; k < ngroup_cols; k++) key_refs[k] = kdb_row_get(row, group_cols[k]);

        uint32_t gi;
        if (ngroup_cols == 0) {
            gi = 0;
            if (ngroups == 0) ngroups = 1;
        } else {
            int found = 0;
            for (gi = 0; gi < ngroups; gi++) {
                int all_match = 1;
                for (int k = 0; k < ngroup_cols; k++) {
                    if (!sql__field_equal(groups[gi].key_refs[k], key_refs[k])) { all_match = 0; break; }
                }
                if (all_match) { found = 1; break; }
            }
            if (!found) {
                if (ngroups >= KDB_SQL_MAX_GROUPS) {
                    free(groups);
                    kdb_set_error(KDB_ERR_SQL_SYNTAX, "SQL error: too many distinct groups (max %d)", KDB_SQL_MAX_GROUPS);
                    return KDB_ERR_SQL_SYNTAX;
                }
                gi = ngroups++;
                for (int k = 0; k < ngroup_cols; k++) groups[gi].key_refs[k] = key_refs[k];
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

    if (ngroup_cols == 0 && ngroups == 0) ngroups = 1; /* no matching rows: still emit one summary row */

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
                case SQL_AGG_NONE: {
                    const KdbField *key_ref = NULL;
                    for (int k = 0; k < ngroup_cols; k++) {
                        if (strcmp(items[it].arg_col, group_cols[k]) == 0) { key_ref = g->key_refs[k]; break; }
                    }
                    if (key_ref) { of->type = key_ref->type; copy_ok = sql__copy_field_value(of, key_ref); }
                    else         { of->type = KDB_TYPE_NULL; }
                    break;
                }
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

/* Projects each row down to the SELECT list -- a plain column looked up
 * by items[i].arg_col, or a CASE item evaluated fresh per row. Either way
 * the output field is named items[i].alias (defaults to arg_col when
 * there's no explicit AS, set at parse time). A plain column with no
 * match on a given row is silently dropped from that row's output, same
 * as always -- CASE always produces a value (NULL at worst), so it never
 * drops. */
static KdbStatus sql__project_rows(KdbRows *rows, SqlSelectItem *items, uint32_t nitems) {
    for (size_t r = 0; r < rows->count; r++) {
        KdbRow *row = &rows->rows[r];
        KdbField *new_fields = (KdbField *)calloc(nitems > 0 ? nitems : 1, sizeof(KdbField));
        if (!new_fields) return KDB_ERR_OOM;

        uint32_t kept = 0;
        for (uint32_t i = 0; i < nitems; i++) {
            if (items[i].is_case) {
                KdbField tmp;
                memset(&tmp, 0, sizeof(tmp));
                if (!sql__eval_case_item(&items[i], row, &tmp)) {
                    for (uint32_t k = 0; k < kept; k++) sql__free_field(&new_fields[k]);
                    free(new_fields);
                    return KDB_ERR_OOM;
                }
                KdbField *dst = &new_fields[kept];
                dst->name = strdup(items[i].alias);
                if (!dst->name) {
                    sql__free_field(&tmp);
                    for (uint32_t k = 0; k < kept; k++) sql__free_field(&new_fields[k]);
                    free(new_fields);
                    return KDB_ERR_OOM;
                }
                dst->type = tmp.type;
                dst->v    = tmp.v; /* ownership of any strdup'd string transfers here */
                kept++;
                continue;
            }

            const KdbField *src = kdb_row_get(row, items[i].arg_col);
            if (!src) continue;

            KdbField *dst = &new_fields[kept];
            dst->name = strdup(items[i].alias);
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
#define KDB_SQL_MAX_JOINS     4

typedef struct {
    char left[KDB_SQL_IDENT_BUF];
    char right[KDB_SQL_IDENT_BUF];
} SqlJoinCond;

/* One JOIN clause in a chain: FROM t1 JOIN t2 ON ... JOIN t3 ON ... each
 * become one SqlJoinClause, joined against the accumulated result of
 * everything before it (see sql__build_joined_rows_multi). */
typedef struct {
    char        table[KDB_SQL_IDENT_BUF];
    char        alias[KDB_SQL_IDENT_BUF];
    int         is_left;
    SqlJoinCond conds[KDB_SQL_MAX_JOIN_COND];
    int         ncond;
} SqlJoinClause;

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

/* Deep-copies a row's fields into dst (id/created_at/updated_at aren't
 * copied -- irrelevant for a synthesized combined row, which never has one
 * single meaningful id/timestamp anyway). field_count only ever counts
 * fully-built fields, so a mid-copy OOM failure still leaves dst safe to
 * free. Returns 0 on OOM. */
static int sql__copy_row(KdbRow *dst, const KdbRow *src) {
    memset(dst, 0, sizeof(*dst));
    if (src->field_count == 0) return 1;
    dst->fields = (KdbField *)calloc(src->field_count, sizeof(KdbField));
    if (!dst->fields) return 0;
    for (uint32_t i = 0; i < src->field_count; i++) {
        char *nm = strdup(src->fields[i].name);
        if (!nm) return 0;
        KdbField f;
        f.name = nm;
        f.type = src->fields[i].type;
        if (!sql__copy_field_value(&f, &src->fields[i])) { free(nm); return 0; }
        dst->fields[dst->field_count++] = f;
    }
    return 1;
}

/* Joins table1 against a chain of JOIN clauses (JOIN t2 ON ... JOIN t3
 * ON ..., etc), each one matched against the accumulated combined rows
 * from everything before it -- so "JOIN t3 ON t1.x = t3.y" and
 * "JOIN t3 ON t2.x = t3.y" both work, t3 can reference any earlier alias
 * in the chain, not just the immediately preceding one. Every column from
 * every table is renamed "<qual>.col" (qual is the table's own name, or
 * its alias if one was given), so a shared column name across any two
 * sides never collides. There's no unqualified fallback; every column
 * reference after a JOIN (SELECT list, ON, WHERE, ORDER BY) needs the
 * "<qual>." prefix.
 *
 * INNER (the default) drops rows with no ON match; LEFT keeps every row
 * from the accumulated-so-far side, padding an unmatched one with NULLs
 * for that step's table. Fetches every table in full (no filter pushdown
 * into the join itself) -- WHERE is applied by the caller afterward, over
 * the final combined rows. */
static KdbStatus sql__build_joined_rows_multi(KumDB *db, const char *table1, const char *alias1,
                                              const SqlJoinClause *joins, int njoins, KdbRows **rows_out) {
    KdbRows *r1 = kdb_find_ex(db, table1, NULL, NULL);
    if (!r1) return kdb_last_status();

    KdbRows *combined = (KdbRows *)calloc(1, sizeof(KdbRows));
    if (!combined) { kdb_rows_free(r1); kdb_err_oom("joined rows"); return KDB_ERR_OOM; }
    if (r1->count > 0) {
        combined->rows = (KdbRow *)calloc(r1->count, sizeof(KdbRow));
        if (!combined->rows) { kdb_rows_free(r1); free(combined); kdb_err_oom("joined rows"); return KDB_ERR_OOM; }
    }
    for (size_t i = 0; i < r1->count; i++) {
        KdbRow combo;
        memset(&combo, 0, sizeof(combo));
        if (!sql__append_qualified_fields(&combo, &r1->rows[i], alias1)) {
            sql__free_row_fields(&combo);
            kdb_rows_free(r1);
            kdb_rows_free(combined);
            kdb_err_oom("joined row");
            return KDB_ERR_OOM;
        }
        combined->rows[combined->count++] = combo;
    }
    kdb_rows_free(r1);

    for (int ji = 0; ji < njoins; ji++) {
        const SqlJoinClause *jc = &joins[ji];

        KdbRows *rN = kdb_find_ex(db, jc->table, NULL, NULL);
        if (!rN) { kdb_rows_free(combined); return kdb_last_status(); }

        KdbColumnInfo schemaN[KDB_MAX_COLUMNS];
        uint32_t schemaN_count = 0;
        if (jc->is_left && kdb_get_schema(db, jc->table, schemaN, KDB_MAX_COLUMNS, &schemaN_count) != KDB_OK) {
            kdb_rows_free(rN);
            kdb_rows_free(combined);
            return kdb_last_status();
        }

        KdbRows *next = (KdbRows *)calloc(1, sizeof(KdbRows));
        if (!next) { kdb_rows_free(rN); kdb_rows_free(combined); kdb_err_oom("joined rows"); return KDB_ERR_OOM; }

#define KDB_SQL_JOIN_FAIL(row, msg) do { \
            sql__free_row_fields(&(row)); \
            kdb_rows_free(rN); kdb_rows_free(combined); kdb_rows_free(next); \
            kdb_err_oom(msg); \
            return KDB_ERR_OOM; \
        } while (0)

        for (size_t i = 0; i < combined->count; i++) {
            int matched = 0;
            for (size_t j = 0; j < rN->count; j++) {
                KdbRow combo;
                if (!sql__copy_row(&combo, &combined->rows[i]) ||
                    !sql__append_qualified_fields(&combo, &rN->rows[j], jc->alias))
                    KDB_SQL_JOIN_FAIL(combo, "joined row");

                int on_ok = 1;
                for (int c = 0; c < jc->ncond; c++) {
                    const KdbField *lf = kdb_row_get(&combo, jc->conds[c].left);
                    const KdbField *rf = kdb_row_get(&combo, jc->conds[c].right);
                    if (!lf || !rf || !sql__field_equal(lf, rf)) { on_ok = 0; break; }
                }

                if (!on_ok) { sql__free_row_fields(&combo); continue; }
                matched = 1;

                KdbRow *grown = (KdbRow *)realloc(next->rows, (next->count + 1) * sizeof(KdbRow));
                if (!grown) KDB_SQL_JOIN_FAIL(combo, "joined rows grow");
                next->rows = grown;
                next->rows[next->count++] = combo;
            }

            if (jc->is_left && !matched) {
                KdbRow combo;
                if (!sql__copy_row(&combo, &combined->rows[i]) ||
                    !sql__append_qualified_nulls(&combo, schemaN, schemaN_count, jc->alias))
                    KDB_SQL_JOIN_FAIL(combo, "left-join padded row");

                KdbRow *grown = (KdbRow *)realloc(next->rows, (next->count + 1) * sizeof(KdbRow));
                if (!grown) KDB_SQL_JOIN_FAIL(combo, "joined rows grow");
                next->rows = grown;
                next->rows[next->count++] = combo;
            }
        }

#undef KDB_SQL_JOIN_FAIL

        kdb_rows_free(rN);
        kdb_rows_free(combined);
        combined = next;
    }

    *rows_out = combined;
    return KDB_OK;
}

/* Fetches the FROM target's rows before WHERE is applied -- from a real
 * table (the common case: filters get pushed straight into kdb_find_ex,
 * *needs_filtering comes back 0), a JOIN chain, a view (executing its
 * stored SELECT as a subquery), or a table whose WHERE used parens (the
 * flat filter_ptrs array can't represent real nesting, only the tree can).
 * The latter three have no way to push a filter into the fetch itself, so
 * they come back fully materialized and *needs_filtering is set -- the
 * caller applies WHERE itself afterward via sql__filter_rows_tree, same
 * pattern either way. */
static KdbStatus sql__fetch_base_rows(KumDB *db, const char *table_name, const char *alias1,
                                      int has_join, const SqlJoinClause *joins, int njoins,
                                      int from_is_view, const char *view_query,
                                      const char **filter_ptrs, int nfilt, int where_used_parens,
                                      KdbRows **out, int *needs_filtering) {
    if (has_join) {
        *needs_filtering = 1;
        return sql__build_joined_rows_multi(db, table_name, alias1, joins, njoins, out);
    }
    if (from_is_view) {
        *needs_filtering = 1;
        SqlParser vp;
        sql__init(&vp, view_query);
        return sql__exec_select_stmt(&vp, db, out);
    }
    if (where_used_parens) {
        *needs_filtering = 1;
        KdbRows *r = kdb_find_ex(db, table_name, NULL, NULL);
        if (!r) return kdb_last_status();
        *out = r;
        return KDB_OK;
    }
    *needs_filtering = 0;
    KdbRows *r = kdb_find_ex(db, table_name, nfilt > 0 ? filter_ptrs : NULL, NULL);
    if (!r) return kdb_last_status();
    *out = r;
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
    int has_case = 0;

    if (p->cur.type == SQLTOK_STAR) {
        project_all = 1;
        sql__advance(p);
    } else {
        for (;;) {
            SqlSelectItem item;
            memset(&item, 0, sizeof(item));

            if (sql__kw_is(&p->cur, "CASE")) {
                if (!sql__parse_case_item(p, db, &item)) return kdb_last_status();
                has_case = 1;
            } else {
                const char *cname;
                if (!sql__ident_text(&p->cur, &cname))
                    return sql__err("expected a column name, aggregate function, CASE, or '*' after SELECT");
                char first_ident[KDB_SQL_IDENT_BUF];
                snprintf(first_ident, sizeof(first_ident), "%.255s", cname);
                sql__advance(p);

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

    /* A view shadows nothing -- it's only even considered when no real
     * table has this name. Fixed-size buffer, not a heap pointer: this
     * function has a lot of early returns already, and a stack buffer
     * unwinds for free on every one of them. Sized to
     * KDB_MAX_STRING_LEN, the storage layer's own string-field cap, since
     * that's the real bound on how long a stored view's query can be. */
    char view_query[KDB_MAX_STRING_LEN];
    int  from_is_view = !kdb_table_exists(db, table_name) &&
                         sql__lookup_view(db, table_name, view_query, sizeof(view_query));

    char alias1[KDB_SQL_IDENT_BUF];
    snprintf(alias1, sizeof(alias1), "%s", table_name);
    if (sql__kw_is(&p->cur, "AS")) {
        sql__advance(p);
        const char *aname;
        if (!sql__ident_text(&p->cur, &aname)) return sql__err("expected an alias after AS");
        snprintf(alias1, sizeof(alias1), "%.255s", aname);
        sql__advance(p);
    } else if (p->cur.type == SQLTOK_IDENT && !sql__is_clause_keyword(p->cur.text)) {
        snprintf(alias1, sizeof(alias1), "%.255s", p->cur.text);
        sql__advance(p);
    }

    SqlJoinClause joins[KDB_SQL_MAX_JOINS];
    int njoins = 0;

    char used_aliases[KDB_SQL_MAX_JOINS + 1][KDB_SQL_IDENT_BUF];
    int  nused_aliases = 1;
    snprintf(used_aliases[0], sizeof(used_aliases[0]), "%s", alias1);

    while (sql__kw_is(&p->cur, "JOIN") || sql__kw_is(&p->cur, "INNER") || sql__kw_is(&p->cur, "LEFT")) {
        if (njoins >= KDB_SQL_MAX_JOINS) return sql__err("too many JOINs (max %d)", KDB_SQL_MAX_JOINS);
        SqlJoinClause *jc = &joins[njoins];
        memset(jc, 0, sizeof(*jc));

        if (sql__kw_is(&p->cur, "LEFT")) {
            jc->is_left = 1;
            sql__advance(p);
            if (sql__kw_is(&p->cur, "OUTER")) sql__advance(p);
        } else if (sql__kw_is(&p->cur, "INNER")) {
            sql__advance(p);
        }
        if (!sql__kw_is(&p->cur, "JOIN")) return sql__err("expected JOIN after %s", jc->is_left ? "LEFT" : "INNER");
        sql__advance(p);

        const char *tNname;
        if (!sql__ident_text(&p->cur, &tNname)) return sql__err("expected a table name after JOIN");
        /* Both copied straight from tNname, not from each other -- some
         * snprintf implementations (mingw's) flag -Wrestrict on a same-
         * struct src/dst copy like table->alias, even though the two
         * fields don't actually overlap. */
        snprintf(jc->table, sizeof(jc->table), "%.255s", tNname);
        snprintf(jc->alias, sizeof(jc->alias), "%.255s", tNname);
        sql__advance(p);

        if (!kdb_table_exists(db, jc->table) && sql__view_exists(db, jc->table))
            return sql__err("'%s' is a view -- using a view as a JOIN target isn't supported yet", jc->table);

        if (sql__kw_is(&p->cur, "AS")) {
            sql__advance(p);
            const char *aname;
            if (!sql__ident_text(&p->cur, &aname)) return sql__err("expected an alias after AS");
            snprintf(jc->alias, sizeof(jc->alias), "%.255s", aname);
            sql__advance(p);
        } else if (p->cur.type == SQLTOK_IDENT && !sql__is_clause_keyword(p->cur.text)) {
            snprintf(jc->alias, sizeof(jc->alias), "%.255s", p->cur.text);
            sql__advance(p);
        }

        if (!sql__kw_is(&p->cur, "ON")) return sql__err("expected ON after JOIN %s", jc->table);
        sql__advance(p);
        if (!sql__parse_join_on(p, jc->conds, &jc->ncond)) return kdb_last_status();

        for (int u = 0; u < nused_aliases; u++) {
            if (strcmp(used_aliases[u], jc->alias) == 0)
                return sql__err("JOIN needs distinct table names or aliases -- '%s' is used more than once", jc->alias);
        }
        snprintf(used_aliases[nused_aliases++], sizeof(used_aliases[0]), "%s", jc->alias);

        njoins++;
    }
    int has_join = njoins > 0;

    if (from_is_view && has_join)
        return sql__err("'%s' is a view -- JOINing a view isn't supported yet", table_name);

    SqlCondNode *where_tree = NULL;
    int where_used_parens = 0;
    if (!sql__parse_where_expr(p, db, has_join ? NULL : alias1, &where_tree, &where_used_parens)) return kdb_last_status();

    char group_cols[KDB_SQL_MAX_GROUP_COLS][KDB_SQL_IDENT_BUF];
    int  ngroup_cols = 0;
    if (sql__kw_is(&p->cur, "GROUP")) {
        sql__advance(p);
        if (!sql__kw_is(&p->cur, "BY")) { sql__free_cond_node(where_tree); return sql__err("expected BY after GROUP"); }
        sql__advance(p);
        for (;;) {
            const char *gcol;
            if (!sql__ident_text(&p->cur, &gcol)) { sql__free_cond_node(where_tree); return sql__err("expected a column name after GROUP BY"); }
            if (ngroup_cols >= KDB_SQL_MAX_GROUP_COLS) { sql__free_cond_node(where_tree); return sql__err("too many GROUP BY columns (max %d)", KDB_SQL_MAX_GROUP_COLS); }
            snprintf(group_cols[ngroup_cols++], sizeof(group_cols[0]), "%.255s", gcol);
            sql__advance(p);
            if (p->cur.type == SQLTOK_COMMA) { sql__advance(p); continue; }
            break;
        }
    }
    int has_group_by = ngroup_cols > 0;

    if (has_join && (has_aggregate || has_group_by)) {
        sql__free_cond_node(where_tree);
        return sql__err("JOIN doesn't support GROUP BY or aggregate functions yet");
    }

    if (has_case && (has_aggregate || has_group_by)) {
        sql__free_cond_node(where_tree);
        return sql__err("CASE doesn't support GROUP BY or aggregate functions yet");
    }

    if (project_all && has_group_by) {
        sql__free_cond_node(where_tree);
        return sql__err("can't use '*' with GROUP BY -- list the columns/aggregates you want");
    }
    if (!project_all && (has_aggregate || has_group_by)) {
        for (uint32_t i = 0; i < nitems; i++) {
            if (items[i].fn != SQL_AGG_NONE) continue;
            int in_group_by = 0;
            for (int k = 0; k < ngroup_cols; k++) {
                if (strcmp(items[i].arg_col, group_cols[k]) == 0) { in_group_by = 1; break; }
            }
            if (!in_group_by) {
                sql__free_cond_node(where_tree);
                return sql__err("column '%s' must appear in GROUP BY or be used inside an aggregate function",
                                items[i].arg_col);
            }
        }
    }

    SqlCondNode *having_tree = NULL;
    int having_used_parens = 0;
    if (sql__kw_is(&p->cur, "HAVING")) {
        if (!has_aggregate && !has_group_by) {
            sql__free_cond_node(where_tree);
            return sql__err("HAVING requires GROUP BY or an aggregate function -- use WHERE to filter plain columns");
        }
        /* HAVING conditions reference the SELECT list's own aliases (e.g.
         * "total" from SUM(amount) AS total), never table-qualified --
         * there's no query-alias unqualifying to do here. */
        if (!sql__parse_having_expr(p, db, NULL, &having_tree, &having_used_parens)) { sql__free_cond_node(where_tree); return kdb_last_status(); }
        if (sql__cond_tree_has_exists(having_tree)) {
            sql__free_cond_node(where_tree);
            sql__free_cond_node(having_tree);
            return sql__err("EXISTS/NOT EXISTS isn't supported in HAVING yet -- HAVING filters aggregated aliases, not a real row to correlate against");
        }
    }

    char *flat_filters[KDB_SQL_MAX_COND];
    int   nfilt = 0;
    if (where_tree && !where_used_parens) {
        if (!sql__cond_flatten(where_tree, flat_filters, &nfilt)) {
            sql__free_filters(flat_filters, nfilt);
            sql__free_cond_node(where_tree);
            sql__free_cond_node(having_tree);
            return kdb_last_status();
        }
    }
    const char *filter_ptrs[KDB_SQL_MAX_COND + 1];
    for (int i = 0; i < nfilt; i++) filter_ptrs[i] = flat_filters[i];
    filter_ptrs[nfilt] = NULL;

    if (has_aggregate || has_group_by) {
        KdbRows *all = NULL;
        int needs_filtering = 0;
        KdbStatus fst = sql__fetch_base_rows(db, table_name, alias1, has_join, joins, njoins,
                                             from_is_view, view_query, filter_ptrs, nfilt, where_used_parens,
                                             &all, &needs_filtering);
        sql__free_filters(flat_filters, nfilt);
        if (fst != KDB_OK) { sql__free_cond_node(where_tree); sql__free_cond_node(having_tree); return fst; }
        if (needs_filtering) sql__filter_rows_tree(db, alias1, all, where_tree);
        sql__free_cond_node(where_tree);

        KdbRows *agg = NULL;
        KdbStatus ast = sql__compute_aggregates(all, items, nitems, group_cols, ngroup_cols, &agg);
        kdb_rows_free(all);
        if (ast != KDB_OK) { sql__free_cond_node(having_tree); return ast; }

        sql__filter_rows_tree(db, alias1, agg, having_tree);
        sql__free_cond_node(having_tree);
        if (distinct) sql__dedupe_rows(agg);

        if (rows_out) *rows_out = agg;
        else           kdb_rows_free(agg);
        return KDB_OK;
    }

    KdbRows *rows = NULL;
    {
        int needs_filtering = 0;
        KdbStatus fst = sql__fetch_base_rows(db, table_name, alias1, has_join, joins, njoins,
                                             from_is_view, view_query, filter_ptrs, nfilt, where_used_parens,
                                             &rows, &needs_filtering);
        sql__free_filters(flat_filters, nfilt);
        if (fst != KDB_OK) { sql__free_cond_node(where_tree); return fst; }
        if (needs_filtering) sql__filter_rows_tree(db, alias1, rows, where_tree);
        sql__free_cond_node(where_tree);
    }

    if (!project_all) {
        KdbStatus pst = sql__project_rows(rows, items, nitems);
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

/* kdb_update()/kdb_delete() only understand the flat "OR:"-prefixed
 * OR'd-AND-groups filter model -- a WHERE that used parens can't be
 * expressed that way. Resolved here instead: fetch every row, filter with
 * the tree, then target the exact matching rows via the "id__in" filter
 * both already understand (id is always a valid filter column). Writes a
 * heap-allocated "id__in=1,2,..." string into *filter_out that the caller
 * owns and must free, or NULL if nothing matched (caller should skip the
 * update/delete call entirely in that case -- an empty IN list isn't a
 * meaningful filter). Returns 0 on error (error already set). */
static int sql__resolve_where_to_id_filter(KumDB *db, const char *table_name, const SqlCondNode *tree, char **filter_out) {
    *filter_out = NULL;
    KdbRows *all = kdb_find_ex(db, table_name, NULL, NULL);
    if (!all) return 0;
    sql__filter_rows_tree(db, table_name, all, tree); /* UPDATE/DELETE have no AS syntax -- table_name is the only alias EXISTS could correlate against */
    if (all->count == 0) { kdb_rows_free(all); return 1; }

    size_t need = 8 + all->count * 24;
    char *buf = malloc(need);
    if (!buf) { kdb_err_oom("id__in filter string"); kdb_rows_free(all); return 0; }
    size_t pos = (size_t)snprintf(buf, need, "id__in=");
    for (size_t i = 0; i < all->count; i++)
        pos += (size_t)snprintf(buf + pos, need - pos, "%s%llu", i > 0 ? "," : "", (unsigned long long)all->rows[i].id);
    kdb_rows_free(all);
    *filter_out = buf;
    return 1;
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

    SqlCondNode *where_tree = NULL;
    int used_parens = 0;
    if (!sql__parse_where_expr(p, db, table_name, &where_tree, &used_parens)) return kdb_last_status();

    KdbField patch[KDB_SQL_MAX_COLUMNS + 1];
    for (uint32_t i = 0; i < nset; i++) {
        if (!sql__value_to_field(set_names[i], &set_vals[i], &patch[i])) {
            sql__free_cond_node(where_tree);
            return sql__err("unsupported value for column '%s' in SET", set_names[i]);
        }
    }
    patch[nset] = kdb_field_end();

    size_t updated = 0;
    KdbStatus st;
    if (used_parens) {
        char *id_filter = NULL;
        int ok = sql__resolve_where_to_id_filter(db, table_name, where_tree, &id_filter);
        sql__free_cond_node(where_tree);
        if (!ok) return kdb_last_status();
        if (!id_filter) { if (affected_out) *affected_out = 0; return KDB_OK; }
        const char *fp[2] = { id_filter, NULL };
        st = kdb_update(db, table_name, fp, patch, &updated);
        free(id_filter);
    } else {
        char *flat_filters[KDB_SQL_MAX_COND];
        int   nfilt = 0;
        if (where_tree && !sql__cond_flatten(where_tree, flat_filters, &nfilt)) {
            sql__free_filters(flat_filters, nfilt);
            sql__free_cond_node(where_tree);
            return kdb_last_status();
        }
        sql__free_cond_node(where_tree);
        const char *filter_ptrs[KDB_SQL_MAX_COND + 1];
        for (int i = 0; i < nfilt; i++) filter_ptrs[i] = flat_filters[i];
        filter_ptrs[nfilt] = NULL;
        st = kdb_update(db, table_name, nfilt > 0 ? filter_ptrs : NULL, patch, &updated);
        sql__free_filters(flat_filters, nfilt);
    }
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

    SqlCondNode *where_tree = NULL;
    int used_parens = 0;
    if (!sql__parse_where_expr(p, db, table_name, &where_tree, &used_parens)) return kdb_last_status();

    size_t deleted = 0;
    KdbStatus st;
    if (used_parens) {
        char *id_filter = NULL;
        int ok = sql__resolve_where_to_id_filter(db, table_name, where_tree, &id_filter);
        sql__free_cond_node(where_tree);
        if (!ok) return kdb_last_status();
        if (!id_filter) { if (affected_out) *affected_out = 0; return KDB_OK; }
        const char *fp[2] = { id_filter, NULL };
        st = kdb_delete(db, table_name, fp, &deleted);
        free(id_filter);
    } else {
        char *flat_filters[KDB_SQL_MAX_COND];
        int   nfilt = 0;
        if (where_tree && !sql__cond_flatten(where_tree, flat_filters, &nfilt)) {
            sql__free_filters(flat_filters, nfilt);
            sql__free_cond_node(where_tree);
            return kdb_last_status();
        }
        sql__free_cond_node(where_tree);
        const char *filter_ptrs[KDB_SQL_MAX_COND + 1];
        for (int i = 0; i < nfilt; i++) filter_ptrs[i] = flat_filters[i];
        filter_ptrs[nfilt] = NULL;
        st = kdb_delete(db, table_name, nfilt > 0 ? filter_ptrs : NULL, &deleted);
        sql__free_filters(flat_filters, nfilt);
    }
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
