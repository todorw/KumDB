#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../include/kumdb.h"
#include "../include/types.h"
#include "../include/sql.h"

#define CLI_VERSION  "1.0.0"
#define MAX_LINE     4096
#define MAX_TOKENS   128
#define MAX_FIELDS   64

static KumDB *db = NULL;

static void print_banner(void) {
    printf("KumDB CLI v%s  (engine v%s)\n", CLI_VERSION, kdb_version());
    printf("Type 'help' for commands.\n\n");
}

static void print_help(void) {
    printf("NoSQL commands:\n");
    printf("  open <dir>                         Open a database directory\n");
    printf("  close                              Close the current database\n");
    printf("  tables                             List all tables\n");
    printf("  schema <table>                     Show schema for a table\n");
    printf("  add <table> <k=v> [k=v ...]        Insert a record (value can be @path for a blob)\n");
    printf("  find <table> [k=v ...] [order_by=col] [order=asc|desc] [limit=N] [offset=N]\n");
    printf("                                      Find records (empty filters = all)\n");
    printf("  findbyid <table> <id>              Find a single record by its id\n");
    printf("  count <table> [k=v ...]            Count matching records\n");
    printf("  delete <table> <k=v> [k=v ...]     Delete matching records\n");
    printf("  update <table> where <k=v> [...] set <k=v> [...]  Update records\n");
    printf("  import <table> <file>              Bulk-insert k=v lines from a file\n");
    printf("  drop <table>                       Drop a table\n");
    printf("  compact <table>                    Compact a table\n");
    printf("\nSQL:\n");
    printf("  sql <statement>                    Run one SQL statement against the open db\n");
    printf("                                      e.g. sql SELECT * FROM users WHERE age > 21\n");
    printf("                                      Same engine as the commands above -- see README\n");
    printf("                                      for what's supported (no JOIN/subqueries/OR).\n");
    printf("\nOther:\n");
    printf("  version                            Show CLI/engine version\n");
    printf("  help                               Show this help\n");
    printf("  quit / exit                        Exit\n\n");
}

static void ensure_db(void) {
    if (!db) printf("No database open. Use: open <dir>\n");
}

static int tokenize(char *line, char **tokens, int max) {
    int count = 0;
    char *p = line;
    while (*p && count < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        tokens[count++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
    return count;
}

static void cmd_open(int argc, char **argv) {
    if (argc < 2) { printf("Usage: open <dir>\n"); return; }
    if (db) { kdb_close(db); db = NULL; }
    db = kdb_open(argv[1]);
    if (!db) printf("Error: %s\n", kdb_last_error());
    else     printf("Opened database at '%s'\n", argv[1]);
}

static void cmd_close(void) {
    if (!db) { printf("No database open.\n"); return; }
    kdb_close(db);
    db = NULL;
    printf("Database closed.\n");
}

static void cmd_tables(void) {
    ensure_db(); if (!db) return;
    const char *names[256];
    size_t count = 0;
    if (kdb_list_tables(db, names, 256, &count) != KDB_OK) {
        printf("Error: %s\n", kdb_last_error());
        return;
    }
    if (count == 0) { printf("No tables.\n"); return; }
    for (size_t i = 0; i < count; i++) printf("  %s\n", names[i]);
}

static void cmd_schema(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 2) { printf("Usage: schema <table>\n"); return; }
    kdb_print_schema(db, argv[1], stdout);
}

/* value == "@path/to/file" loads the file's raw bytes as a blob field. */
static int load_blob_file(const char *path, void **buf_out, size_t *len_out) {
    FILE *bf = fopen(path, "rb");
    if (!bf) return 0;
    if (fseek(bf, 0, SEEK_END) != 0) { fclose(bf); return 0; }
    long blen = ftell(bf);
    if (blen < 0 || fseek(bf, 0, SEEK_SET) != 0) { fclose(bf); return 0; }

    void *buf = malloc((size_t)blen > 0 ? (size_t)blen : 1);
    if (!buf) { fclose(bf); return 0; }
    if (blen > 0 && fread(buf, 1, (size_t)blen, bf) != (size_t)blen) {
        fclose(bf); free(buf); return 0;
    }
    fclose(bf);
    *buf_out = buf;
    *len_out = (size_t)blen;
    return 1;
}

static void cmd_add(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 3) { printf("Usage: add <table> <key=value> [...]   (value can be @path for a blob)\n"); return; }

    KdbField fields[MAX_FIELDS + 1];
    int nf = 0;
    char names[MAX_FIELDS][128];
    char values[MAX_FIELDS][512];
    void *blob_bufs[MAX_FIELDS];
    int   nblobs = 0;

    for (int i = 2; i < argc && nf < MAX_FIELDS; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { printf("Bad field '%s' — use key=value\n", argv[i]); goto cleanup; }
        *eq = '\0';
        strncpy(names[nf],  argv[i], 127); names[nf][127]  = '\0';
        strncpy(values[nf], eq + 1,  511); values[nf][511] = '\0';

        if (values[nf][0] == '@') {
            void  *buf = NULL;
            size_t len = 0;
            if (!load_blob_file(values[nf] + 1, &buf, &len)) {
                printf("Couldn't read blob file '%s'\n", values[nf] + 1);
                goto cleanup;
            }
            blob_bufs[nblobs++] = buf;
            fields[nf] = kdb_field_blob(names[nf], buf, len);
        } else {
            KdbType t = kdb_type_infer(values[nf]);
            switch (t) {
                case KDB_TYPE_INT:    fields[nf] = kdb_field_int   (names[nf], (int64_t)atoll(values[nf])); break;
                case KDB_TYPE_FLOAT:  fields[nf] = kdb_field_float (names[nf], atof(values[nf]));           break;
                case KDB_TYPE_BOOL:   fields[nf] = kdb_field_bool  (names[nf], strcasecmp(values[nf], "true") == 0 || strcmp(values[nf], "1") == 0); break;
                case KDB_TYPE_NULL:   fields[nf] = kdb_field_null  (names[nf]);                             break;
                default:              fields[nf] = kdb_field_string(names[nf], values[nf]);                 break;
            }
        }
        nf++;
    }
    fields[nf] = kdb_field_end();

    {
        KdbStatus st = kdb_add(db, argv[1], fields);
        if (st != KDB_OK) printf("Error: %s\n", kdb_last_error());
        else              printf("Inserted.\n");
    }

cleanup:
    for (int i = 0; i < nblobs; i++) free(blob_bufs[i]);
}

static void cmd_find(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 2) {
        printf("Usage: find <table> [filter ...] [order_by=col] [order=asc|desc] [limit=N] [offset=N]\n");
        return;
    }

    const char *filters[MAX_FIELDS + 1];
    int nf = 0;
    KdbFindOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.ascending = 1;
    char order_col[128] = "";

    for (int i = 2; i < argc && nf < MAX_FIELDS; i++) {
        char *tok = argv[i];
        if (strncmp(tok, "order_by=", 9) == 0) {
            strncpy(order_col, tok + 9, sizeof(order_col) - 1);
            order_col[sizeof(order_col) - 1] = '\0';
            opts.order_by = order_col;
        } else if (strncmp(tok, "order=", 6) == 0) {
            opts.ascending = strcasecmp(tok + 6, "desc") != 0;
        } else if (strncmp(tok, "limit=", 6) == 0) {
            opts.limit = (size_t)atoll(tok + 6);
        } else if (strncmp(tok, "offset=", 7) == 0) {
            opts.offset = (size_t)atoll(tok + 7);
        } else {
            filters[nf++] = tok;
        }
    }
    filters[nf] = NULL;

    KdbRows *rows = kdb_find_ex(db, argv[1], nf > 0 ? filters : NULL, &opts);
    if (!rows) { printf("Error: %s\n", kdb_last_error()); return; }
    if (rows->count == 0) { printf("No results.\n"); kdb_rows_free(rows); return; }
    kdb_rows_print(rows, stdout);
    kdb_rows_free(rows);
}

static void cmd_count(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 2) { printf("Usage: count <table> [filter ...]\n"); return; }

    const char *filters[MAX_FIELDS + 1];
    int nf = 0;
    for (int i = 2; i < argc && nf < MAX_FIELDS; i++)
        filters[nf++] = argv[i];
    filters[nf] = NULL;

    int64_t n = kdb_count(db, argv[1], nf > 0 ? filters : NULL);
    if (n < 0) printf("Error: %s\n", kdb_last_error());
    else       printf("%lld\n", (long long)n);
}

static void cmd_findbyid(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 3) { printf("Usage: findbyid <table> <id>\n"); return; }

    uint64_t id = (uint64_t)strtoull(argv[2], NULL, 10);
    KdbRow *row = kdb_find_by_id(db, argv[1], id);
    if (!row) { printf("Error: %s\n", kdb_last_error()); return; }
    kdb_row_print(row, stdout);
    kdb_row_free(row);
}

static void cmd_import(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 3) { printf("Usage: import <table> <file>   (one record per line, k=v k=v ...)\n"); return; }

    FILE *fp = fopen(argv[2], "r");
    if (!fp) { printf("Error: couldn't open '%s'\n", argv[2]); return; }

    char line[MAX_LINE];
    char *ltoks[MAX_TOKENS];
    size_t imported = 0, failed = 0, line_no = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        line[strcspn(line, "\n")] = '\0';

        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;

        int ntok = tokenize(line, ltoks, MAX_TOKENS);
        if (ntok == 0) continue;

        KdbField fields[MAX_FIELDS + 1];
        int nf = 0;
        char names[MAX_FIELDS][128];
        char values[MAX_FIELDS][512];
        int bad = 0;

        for (int i = 0; i < ntok && nf < MAX_FIELDS; i++) {
            char *eq = strchr(ltoks[i], '=');
            if (!eq) { printf("Line %zu: bad field '%s' — skipping\n", line_no, ltoks[i]); bad = 1; break; }
            *eq = '\0';
            strncpy(names[nf],  ltoks[i], 127); names[nf][127]  = '\0';
            strncpy(values[nf], eq + 1,  511); values[nf][511] = '\0';

            KdbType t = kdb_type_infer(values[nf]);
            switch (t) {
                case KDB_TYPE_INT:    fields[nf] = kdb_field_int   (names[nf], (int64_t)atoll(values[nf])); break;
                case KDB_TYPE_FLOAT:  fields[nf] = kdb_field_float (names[nf], atof(values[nf]));           break;
                case KDB_TYPE_BOOL:   fields[nf] = kdb_field_bool  (names[nf], strcasecmp(values[nf], "true") == 0 || strcmp(values[nf], "1") == 0); break;
                case KDB_TYPE_NULL:   fields[nf] = kdb_field_null  (names[nf]);                             break;
                default:              fields[nf] = kdb_field_string(names[nf], values[nf]);                 break;
            }
            nf++;
        }
        if (bad || nf == 0) { failed++; continue; }
        fields[nf] = kdb_field_end();

        if (kdb_add(db, argv[1], fields) == KDB_OK) imported++;
        else { printf("Line %zu: %s\n", line_no, kdb_last_error()); failed++; }
    }

    fclose(fp);
    printf("Imported %zu record(s), %zu failed.\n", imported, failed);
}

static void cmd_sql(const char *raw_stmt) {
    ensure_db(); if (!db) return;
    if (!raw_stmt || !*raw_stmt) { printf("Usage: sql <statement>\n"); return; }

    KdbRows *rows = NULL;
    size_t   affected = 0;
    KdbStatus st = kdb_exec_sql(db, raw_stmt, &rows, &affected);
    if (st != KDB_OK) { printf("Error: %s\n", kdb_last_error()); return; }

    if (rows) {
        if (rows->count == 0) printf("No results.\n");
        else                  kdb_rows_print(rows, stdout);
        kdb_rows_free(rows);
    } else {
        printf("OK. %zu row(s) affected.\n", affected);
    }
}

static void cmd_version(void) {
    printf("KumDB CLI v%s (engine v%s)\n", CLI_VERSION, kdb_version());
}

static void cmd_delete(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 3) { printf("Usage: delete <table> <filter> [...]\n"); return; }

    const char *filters[MAX_FIELDS + 1];
    int nf = 0;
    for (int i = 2; i < argc && nf < MAX_FIELDS; i++)
        filters[nf++] = argv[i];
    filters[nf] = NULL;

    size_t deleted = 0;
    KdbStatus st = kdb_delete(db, argv[1], filters, &deleted);
    if (st != KDB_OK) printf("Error: %s\n", kdb_last_error());
    else              printf("Deleted %zu record(s).\n", deleted);
}

static void cmd_update(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 5) {
        printf("Usage: update <table> where <k=v> [...] set <k=v> [...]\n");
        return;
    }

    const char *where_filters[MAX_FIELDS + 1];
    int nw = 0;
    char set_names[MAX_FIELDS][128];
    char set_values[MAX_FIELDS][512];
    KdbField set_fields[MAX_FIELDS + 1];
    int ns = 0;

    int in_set = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "where") == 0) { in_set = 0; continue; }
        if (strcmp(argv[i], "set")   == 0) { in_set = 1; continue; }
        if (!in_set) {
            if (nw < MAX_FIELDS) where_filters[nw++] = argv[i];
        } else {
            char *eq = strchr(argv[i], '=');
            if (!eq || ns >= MAX_FIELDS) continue;
            *eq = '\0';
            strncpy(set_names[ns],  argv[i], 127); set_names[ns][127]  = '\0';
            strncpy(set_values[ns], eq + 1,  511); set_values[ns][511] = '\0';
            KdbType t = kdb_type_infer(set_values[ns]);
            switch (t) {
                case KDB_TYPE_INT:   set_fields[ns] = kdb_field_int   (set_names[ns], (int64_t)atoll(set_values[ns])); break;
                case KDB_TYPE_FLOAT: set_fields[ns] = kdb_field_float (set_names[ns], atof(set_values[ns]));           break;
                case KDB_TYPE_BOOL:  set_fields[ns] = kdb_field_bool  (set_names[ns], strcasecmp(set_values[ns], "true") == 0); break;
                case KDB_TYPE_NULL:  set_fields[ns] = kdb_field_null  (set_names[ns]);                                 break;
                default:             set_fields[ns] = kdb_field_string(set_names[ns], set_values[ns]);                 break;
            }
            ns++;
        }
    }
    where_filters[nw] = NULL;
    set_fields[ns]    = kdb_field_end();

    if (ns == 0) { printf("No set fields specified.\n"); return; }

    size_t updated = 0;
    KdbStatus st = kdb_update(db, argv[1], nw > 0 ? where_filters : NULL, set_fields, &updated);
    if (st != KDB_OK) printf("Error: %s\n", kdb_last_error());
    else              printf("Updated %zu record(s).\n", updated);
}

static void cmd_drop(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 2) { printf("Usage: drop <table>\n"); return; }
    KdbStatus st = kdb_drop_table(db, argv[1]);
    if (st != KDB_OK) printf("Error: %s\n", kdb_last_error());
    else              printf("Dropped '%s'.\n", argv[1]);
}

static void cmd_compact(int argc, char **argv) {
    ensure_db(); if (!db) return;
    if (argc < 2) { printf("Usage: compact <table>\n"); return; }
    KdbStatus st = kdb_compact(db, argv[1]);
    if (st != KDB_OK) printf("Error: %s\n", kdb_last_error());
    else              printf("Compacted '%s'.\n", argv[1]);
}

int main(int argc, char **argv) {
    print_banner();

    if (argc >= 2) {
        char *open_argv[] = { "open", argv[1] };
        cmd_open(2, open_argv);
    }

    char line[MAX_LINE];
    char *tokens[MAX_TOKENS];

    while (1) {
        printf("kumdb> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        line[strcspn(line, "\n")] = '\0';

        char *trimmed = line;
        while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
        if (!*trimmed) continue;

        /* "sql <statement>" bypasses the k=v tokenizer entirely -- SQL text
         * has its own quoting/whitespace rules the tokenizer would mangle. */
        if (strncasecmp(trimmed, "sql", 3) == 0 &&
            (trimmed[3] == '\0' || isspace((unsigned char)trimmed[3]))) {
            const char *rest = trimmed + 3;
            while (*rest && isspace((unsigned char)*rest)) rest++;
            cmd_sql(rest);
            continue;
        }

        int ntok = tokenize(line, tokens, MAX_TOKENS);
        if (ntok == 0) continue;

        char *cmd = tokens[0];

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) break;
        else if (strcmp(cmd, "help")     == 0) print_help();
        else if (strcmp(cmd, "version")  == 0) cmd_version();
        else if (strcmp(cmd, "open")     == 0) cmd_open(ntok, tokens);
        else if (strcmp(cmd, "close")    == 0) cmd_close();
        else if (strcmp(cmd, "tables")   == 0) cmd_tables();
        else if (strcmp(cmd, "schema")   == 0) cmd_schema(ntok, tokens);
        else if (strcmp(cmd, "add")      == 0) cmd_add(ntok, tokens);
        else if (strcmp(cmd, "find")     == 0) cmd_find(ntok, tokens);
        else if (strcmp(cmd, "findbyid") == 0) cmd_findbyid(ntok, tokens);
        else if (strcmp(cmd, "count")    == 0) cmd_count(ntok, tokens);
        else if (strcmp(cmd, "delete")   == 0) cmd_delete(ntok, tokens);
        else if (strcmp(cmd, "update")   == 0) cmd_update(ntok, tokens);
        else if (strcmp(cmd, "import")   == 0) cmd_import(ntok, tokens);
        else if (strcmp(cmd, "drop")     == 0) cmd_drop(ntok, tokens);
        else if (strcmp(cmd, "compact")  == 0) cmd_compact(ntok, tokens);
        else printf("Unknown command '%s'. Type 'help'.\n", cmd);
    }

    if (db) kdb_close(db);
    printf("Bye.\n");
    return 0;
}