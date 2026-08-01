#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/kumdb.h"

typedef enum { FMT_PRETTY, FMT_CSV, FMT_JSON } OutputFmt;

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <data_dir> <table> [--csv | --json] [--limit N]\n", prog);
    fprintf(stderr, "  --csv      output as CSV\n");
    fprintf(stderr, "  --json     output as JSON array\n");
    fprintf(stderr, "  --limit N  cap output at N rows\n");
}

static void escape_csv(const char *s, FILE *fp) {
    int needs_quote = strchr(s, ',') || strchr(s, '"') || strchr(s, '\n');
    if (needs_quote) fputc('"', fp);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', fp);
        fputc(*p, fp);
    }
    if (needs_quote) fputc('"', fp);
}

static void escape_json_string(const char *s, FILE *fp) {
    fputc('"', fp);
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n",  fp); break;
            case '\r': fputs("\\r",  fp); break;
            case '\t': fputs("\\t",  fp); break;
            default:   fputc(*p, fp);     break;
        }
    }
    fputc('"', fp);
}

static void print_field_csv(const KdbField *f, FILE *fp) {
    char buf[64];
    switch (f->type) {
        case KDB_TYPE_INT:    fprintf(fp, "%lld",  (long long)f->v.as_int);   break;
        case KDB_TYPE_FLOAT:  fprintf(fp, "%g",    f->v.as_float);            break;
        case KDB_TYPE_BOOL:   fprintf(fp, "%s",    f->v.as_bool ? "true" : "false"); break;
        case KDB_TYPE_NULL:   break;
        case KDB_TYPE_STRING: escape_csv(f->v.as_string ? f->v.as_string : "", fp); break;
        case KDB_TYPE_ARRAY:
            snprintf(buf, sizeof(buf), "<array:%zu items>", f->v.as_array.count);
            escape_csv(buf, fp);
            break;
        case KDB_TYPE_OBJECT: {
            uint32_t n = 0;
            if (f->v.as_object) while (f->v.as_object[n].name != NULL) n++;
            snprintf(buf, sizeof(buf), "<object:%u fields>", n);
            escape_csv(buf, fp);
            break;
        }
        default:
            snprintf(buf, sizeof(buf), "<blob>");
            escape_csv(buf, fp);
            break;
    }
}

static void print_field_json(const KdbField *f, FILE *fp) {
    switch (f->type) {
        case KDB_TYPE_INT:    fprintf(fp, "%lld",  (long long)f->v.as_int);   break;
        case KDB_TYPE_FLOAT:  fprintf(fp, "%g",    f->v.as_float);            break;
        case KDB_TYPE_BOOL:   fprintf(fp, "%s",    f->v.as_bool ? "true" : "false"); break;
        case KDB_TYPE_NULL:   fprintf(fp, "null");                             break;
        case KDB_TYPE_STRING: escape_json_string(f->v.as_string ? f->v.as_string : "", fp); break;
        case KDB_TYPE_ARRAY:
            fprintf(fp, "[");
            for (size_t i = 0; i < f->v.as_array.count; i++) {
                if (i > 0) fprintf(fp, ",");
                print_field_json(&f->v.as_array.items[i], fp);
            }
            fprintf(fp, "]");
            break;
        case KDB_TYPE_OBJECT:
            fprintf(fp, "{");
            if (f->v.as_object) {
                int first = 1;
                for (const KdbField *sub = f->v.as_object; sub->name != NULL; sub++) {
                    if (!first) fprintf(fp, ",");
                    escape_json_string(sub->name, fp);
                    fprintf(fp, ":");
                    print_field_json(sub, fp);
                    first = 0;
                }
            }
            fprintf(fp, "}");
            break;
        default:              fprintf(fp, "\"<blob>\"");                       break;
    }
}

static void dump_pretty(KdbRows *rows) {
    if (rows->count == 0) { printf("(empty)\n"); return; }
    kdb_rows_print(rows, stdout);
}

static void dump_csv(KdbRows *rows) {
    if (rows->count == 0) return;

    printf("id");
    for (uint32_t j = 0; j < rows->rows[0].field_count; j++)
        printf(",%s", rows->rows[0].fields[j].name ? rows->rows[0].fields[j].name : "");
    printf("\n");

    for (size_t i = 0; i < rows->count; i++) {
        KdbRow *row = &rows->rows[i];
        printf("%llu", (unsigned long long)row->id);
        for (uint32_t j = 0; j < row->field_count; j++) {
            printf(",");
            print_field_csv(&row->fields[j], stdout);
        }
        printf("\n");
    }
}

static void dump_json(KdbRows *rows) {
    printf("[\n");
    for (size_t i = 0; i < rows->count; i++) {
        KdbRow *row = &rows->rows[i];
        printf("  {\"id\": %llu", (unsigned long long)row->id);
        for (uint32_t j = 0; j < row->field_count; j++) {
            const KdbField *f = &row->fields[j];
            printf(", ");
            escape_json_string(f->name ? f->name : "", stdout);
            printf(": ");
            print_field_json(f, stdout);
        }
        printf("}%s\n", i + 1 < rows->count ? "," : "");
    }
    printf("]\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *dir   = argv[1];
    const char *table = argv[2];
    OutputFmt   fmt   = FMT_PRETTY;
    int64_t     limit = -1;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--csv")  == 0) fmt = FMT_CSV;
        else if (strcmp(argv[i], "--json") == 0) fmt = FMT_JSON;
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoll(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    KumDB *db = kdb_open(dir);
    if (!db) {
        fprintf(stderr, "Failed to open '%s': %s\n", dir, kdb_last_error());
        return 1;
    }

    if (!kdb_table_exists(db, table)) {
        fprintf(stderr, "Table '%s' does not exist.\n", table);
        kdb_close(db);
        return 1;
    }

    KdbRows *rows = kdb_find(db, table, NULL);
    if (!rows) {
        fprintf(stderr, "Failed to read '%s': %s\n", table, kdb_last_error());
        kdb_close(db);
        return 1;
    }

    if (limit >= 0 && (size_t)limit < rows->count)
        rows->count = (size_t)limit;

    switch (fmt) {
        case FMT_CSV:    dump_csv   (rows); break;
        case FMT_JSON:   dump_json  (rows); break;
        default:         dump_pretty(rows); break;
    }

    kdb_rows_free(rows);
    kdb_close(db);
    return 0;
}