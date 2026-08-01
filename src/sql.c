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
        while (isalnum((unsigned char)s[j]) || s[j] == '_') j++;
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

static char *sql__parse_condition(SqlParser *p) {
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

    char *val_text = sql__value_text(&p->cur);
    if (!val_text) { sql__err("expected a value after the comparison operator on '%s'", col_buf); return NULL; }
    sql__advance(p);

    size_t need = strlen(col_buf) + strlen(suffix) + strlen(val_text) + 8;
    char *buf = malloc(need);
    if (buf) {
        if (suffix[0]) snprintf(buf, need, "%s__%s=%s", col_buf, suffix, val_text);
        else           snprintf(buf, need, "%s=%s", col_buf, val_text);
    }
    free(val_text);
    return buf;
}

/* returns 0 on error (error already set), 1 on success */
static int sql__parse_where(SqlParser *p, char *filters_buf[KDB_SQL_MAX_COND], int *count_out) {
    int n = 0;
    *count_out = 0;
    if (!sql__kw_is(&p->cur, "WHERE")) return 1;
    sql__advance(p);

    for (;;) {
        if (n >= KDB_SQL_MAX_COND) {
            sql__err("too many WHERE conditions (max %d)", KDB_SQL_MAX_COND);
            sql__free_filters(filters_buf, n);
            return 0;
        }
        char *f = sql__parse_condition(p);
        if (!f) { sql__free_filters(filters_buf, n); return 0; }
        filters_buf[n++] = f;

        if (sql__kw_is(&p->cur, "OR")) {
            sql__err("OR isn't supported -- KumDB's query engine is AND-only, same as the NoSQL filter API");
            sql__free_filters(filters_buf, n);
            return 0;
        }
        if (sql__kw_is(&p->cur, "AND")) { sql__advance(p); continue; }
        break;
    }
    *count_out = n;
    return 1;
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
        for (;;) {
            if (sql__kw_is(&p->cur, "NOT")) {
                sql__advance(p);
                if (!sql__kw_is(&p->cur, "NULL")) return sql__err("expected NULL after NOT for column '%s'", this_name);
                sql__advance(p);
                nullable = 0;
                continue;
            }
            if (sql__kw_is(&p->cur, "INDEX") || sql__kw_is(&p->cur, "INDEXED")) { sql__advance(p); indexed = 1; continue; }
            if (sql__kw_is(&p->cur, "UNIQUE")) { sql__advance(p); indexed = 1; continue; }
            if (sql__kw_is(&p->cur, "KEY"))    { sql__advance(p); indexed = 1; continue; }
            if (sql__kw_is(&p->cur, "PRIMARY")) {
                sql__advance(p);
                if (sql__kw_is(&p->cur, "KEY")) sql__advance(p);
                indexed = 1; nullable = 0;
                continue;
            }
            if (sql__kw_is(&p->cur, "DEFAULT")) {
                /* best-effort: accept and ignore a default value, don't enforce it */
                sql__advance(p);
                sql__advance(p);
                continue;
            }
            break;
        }

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

static KdbStatus sql__project_rows(KdbRows *rows, char proj_cols[][KDB_SQL_IDENT_BUF], uint32_t nproj) {
    for (size_t r = 0; r < rows->count; r++) {
        KdbRow *row = &rows->rows[r];
        KdbField *new_fields = (KdbField *)calloc(nproj > 0 ? nproj : 1, sizeof(KdbField));
        if (!new_fields) return KDB_ERR_OOM;

        uint32_t kept = 0;
        for (uint32_t i = 0; i < nproj; i++) {
            const KdbField *src = kdb_row_get(row, proj_cols[i]);
            if (!src) continue;

            char *name_copy = strdup(proj_cols[i]);
            if (!name_copy) {
                for (uint32_t k = 0; k < kept; k++) {
                    free((void *)new_fields[k].name);
                    if (new_fields[k].type == KDB_TYPE_STRING) free((void *)new_fields[k].v.as_string);
                    else if (new_fields[k].type == KDB_TYPE_BLOB) free((void *)new_fields[k].v.as_blob.data);
                }
                free(new_fields);
                return KDB_ERR_OOM;
            }

            KdbField dst;
            dst.name = name_copy;
            dst.type = src->type;
            switch (src->type) {
                case KDB_TYPE_INT:    dst.v.as_int   = src->v.as_int;   break;
                case KDB_TYPE_FLOAT:  dst.v.as_float = src->v.as_float; break;
                case KDB_TYPE_BOOL:   dst.v.as_bool  = src->v.as_bool;  break;
                case KDB_TYPE_STRING:
                    dst.v.as_string = src->v.as_string ? strdup(src->v.as_string) : NULL;
                    break;
                case KDB_TYPE_BLOB:
                    if (src->v.as_blob.len > 0 && src->v.as_blob.data) {
                        void *copy = malloc(src->v.as_blob.len);
                        if (copy) memcpy(copy, src->v.as_blob.data, src->v.as_blob.len);
                        dst.v.as_blob.data = copy;
                    } else {
                        dst.v.as_blob.data = NULL;
                    }
                    dst.v.as_blob.len = src->v.as_blob.len;
                    break;
                default:
                    memset(&dst.v, 0, sizeof(dst.v));
                    break;
            }
            new_fields[kept++] = dst;
        }

        for (uint32_t i = 0; i < row->field_count; i++) {
            free((void *)row->fields[i].name);
            if (row->fields[i].type == KDB_TYPE_STRING) free((void *)row->fields[i].v.as_string);
            else if (row->fields[i].type == KDB_TYPE_BLOB) free((void *)row->fields[i].v.as_blob.data);
        }
        free(row->fields);

        row->fields      = new_fields;
        row->field_count = kept;
    }
    return KDB_OK;
}

static KdbStatus sql__exec_select(SqlParser *p, KumDB *db, KdbRows **rows_out) {
    sql__advance(p); /* SELECT */

    int project_all = 0;
    char proj_cols[KDB_SQL_MAX_COLUMNS][KDB_SQL_IDENT_BUF];
    uint32_t nproj = 0;

    if (p->cur.type == SQLTOK_STAR) {
        project_all = 1;
        sql__advance(p);
    } else {
        for (;;) {
            const char *cname;
            if (!sql__ident_text(&p->cur, &cname)) return sql__err("expected a column name or '*' after SELECT");
            if (nproj >= KDB_SQL_MAX_COLUMNS) return sql__err("too many selected columns (max %d)", KDB_SQL_MAX_COLUMNS);
            snprintf(proj_cols[nproj++], sizeof(proj_cols[0]), "%.255s", cname);
            sql__advance(p);
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

    char *filters[KDB_SQL_MAX_COND];
    int   nfilt = 0;
    if (!sql__parse_where(p, filters, &nfilt)) return kdb_last_status();

    KdbFindOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.ascending = 1;
    char order_col[KDB_SQL_IDENT_BUF];
    order_col[0] = '\0';

    if (sql__kw_is(&p->cur, "ORDER")) {
        sql__advance(p);
        if (!sql__kw_is(&p->cur, "BY")) { sql__free_filters(filters, nfilt); return sql__err("expected BY after ORDER"); }
        sql__advance(p);
        const char *ocol;
        if (!sql__ident_text(&p->cur, &ocol)) { sql__free_filters(filters, nfilt); return sql__err("expected a column name after ORDER BY"); }
        snprintf(order_col, sizeof(order_col), "%.255s", ocol);
        opts.order_by = order_col;
        sql__advance(p);
        if (sql__kw_is(&p->cur, "ASC"))       { sql__advance(p); opts.ascending = 1; }
        else if (sql__kw_is(&p->cur, "DESC")) { sql__advance(p); opts.ascending = 0; }
    }

    if (sql__kw_is(&p->cur, "LIMIT")) {
        sql__advance(p);
        if (p->cur.type != SQLTOK_NUMBER) { sql__free_filters(filters, nfilt); return sql__err("expected a number after LIMIT"); }
        opts.limit = (size_t)atoll(p->cur.text);
        sql__advance(p);
        if (sql__kw_is(&p->cur, "OFFSET")) {
            sql__advance(p);
            if (p->cur.type != SQLTOK_NUMBER) { sql__free_filters(filters, nfilt); return sql__err("expected a number after OFFSET"); }
            opts.offset = (size_t)atoll(p->cur.text);
            sql__advance(p);
        }
    }

    const char *filter_ptrs[KDB_SQL_MAX_COND + 1];
    for (int i = 0; i < nfilt; i++) filter_ptrs[i] = filters[i];
    filter_ptrs[nfilt] = NULL;

    KdbRows *rows = kdb_find_ex(db, table_name, nfilt > 0 ? filter_ptrs : NULL, &opts);
    sql__free_filters(filters, nfilt);
    if (!rows) return kdb_last_status();

    if (!project_all) {
        KdbStatus pst = sql__project_rows(rows, proj_cols, nproj);
        if (pst != KDB_OK) { kdb_rows_free(rows); return pst; }
    }

    if (rows_out) *rows_out = rows;
    else           kdb_rows_free(rows);
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
    if (!sql__parse_where(p, filters, &nfilt)) return kdb_last_status();

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
    if (!sql__parse_where(p, filters, &nfilt)) return kdb_last_status();

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
    else if (sql__kw_is(&p.cur, "DROP"))   st = sql__exec_drop_table(&p, db);
    else if (sql__kw_is(&p.cur, "INSERT")) st = sql__exec_insert(&p, db, affected_out);
    else if (sql__kw_is(&p.cur, "SELECT")) st = sql__exec_select(&p, db, rows_out);
    else if (sql__kw_is(&p.cur, "UPDATE")) st = sql__exec_update(&p, db, affected_out);
    else if (sql__kw_is(&p.cur, "DELETE")) st = sql__exec_delete(&p, db, affected_out);
    else return sql__err("unrecognized statement -- expected CREATE, DROP, INSERT, SELECT, UPDATE, or DELETE");

    if (st != KDB_OK) return st;

    if (p.cur.type == SQLTOK_SEMI) sql__advance(&p);
    if (p.cur.type != SQLTOK_EOF)
        return sql__err("unexpected trailing content after the statement -- one statement per call");

    return KDB_OK;
}
