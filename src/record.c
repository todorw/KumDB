#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/record.h"
#include "../include/error.h"
#include "../include/types.h"


KdbRecord *kdb_record_new(uint32_t field_count) {
    KdbRecord *r = (KdbRecord *)calloc(1, sizeof(KdbRecord));
    if (!r) { kdb_err_oom("KdbRecord"); return NULL; }

    if (field_count > 0) {
        r->fields = (KdbRecordField *)calloc(field_count, sizeof(KdbRecordField));
        if (!r->fields) {
            kdb_err_oom("KdbRecordField array");
            free(r);
            return NULL;
        }
    }

    r->field_count = 0;           
    r->created_at  = (uint64_t)time(NULL);
    r->updated_at  = r->created_at;
    r->deleted     = 0;
    r->id          = 0;           
    return r;
}

KdbRecord *kdb_record_copy(const KdbRecord *src) {
    if (!src) return NULL;

    KdbRecord *dst = kdb_record_new(src->field_count);
    if (!dst) return NULL;

    dst->id         = src->id;
    dst->created_at = src->created_at;
    dst->updated_at = src->updated_at;
    dst->deleted    = src->deleted;

    for (uint32_t i = 0; i < src->field_count; i++) {
        KDB_STRLCPY(dst->fields[i].col_name,
                    src->fields[i].col_name,
                    KDB_MAX_NAME_LEN);
        if (kdb_value_copy(&src->fields[i].value, &dst->fields[i].value) != KDB_OK) {
            kdb_record_free(dst);
            return NULL;
        }
        dst->field_count++;
    }
    return dst;
}

void kdb_record_free(KdbRecord *r) {
    if (!r) return;
    if (r->fields) {
        for (uint32_t i = 0; i < r->field_count; i++)
            kdb_value_free(&r->fields[i].value);
        free(r->fields);
        r->fields = NULL;
    }
    free(r);
}

void kdb_record_free_array(KdbRecord *arr, size_t count) {
    if (!arr) return;
    for (size_t i = 0; i < count; i++) {
        if (arr[i].fields) {
            for (uint32_t j = 0; j < arr[i].field_count; j++)
                kdb_value_free(&arr[i].fields[j].value);
            free(arr[i].fields);
        }
    }
    free(arr);
}


KdbStatus kdb_record_set_field(KdbRecord      *r,
                               const char     *col_name,
                               const KdbValue *value) {
    if (!r || !col_name || !value) {
        kdb_err_null_arg("r/col_name/value", "kdb_record_set_field");
        return KDB_ERR_BAD_ARG;
    }

    
    for (uint32_t i = 0; i < r->field_count; i++) {
        if (strcmp(r->fields[i].col_name, col_name) == 0) {
            kdb_value_free(&r->fields[i].value);
            return kdb_value_copy(value, &r->fields[i].value);
        }
    }

    
    KdbRecordField *new_fields = realloc(r->fields,
                                   (r->field_count + 1) * sizeof(KdbRecordField));
    if (!new_fields) {
        kdb_err_oom("KdbRecordField resize");
        return KDB_ERR_OOM;
    }
    r->fields = new_fields;
    memset(&r->fields[r->field_count], 0, sizeof(KdbRecordField));
    KDB_STRLCPY(r->fields[r->field_count].col_name, col_name, KDB_MAX_NAME_LEN);

    KdbStatus st = kdb_value_copy(value, &r->fields[r->field_count].value);
    if (st != KDB_OK) return st;

    r->field_count++;
    r->updated_at = (uint64_t)time(NULL);
    return KDB_OK;
}

KdbStatus kdb_record_set_int(KdbRecord *r, const char *col, int64_t v) {
    KdbValue val; kdb_value_from_int(v, &val);
    return kdb_record_set_field(r, col, &val);
}

KdbStatus kdb_record_set_float(KdbRecord *r, const char *col, double v) {
    KdbValue val; kdb_value_from_float(v, &val);
    return kdb_record_set_field(r, col, &val);
}

KdbStatus kdb_record_set_bool(KdbRecord *r, const char *col, uint8_t v) {
    KdbValue val; kdb_value_from_bool(v, &val);
    return kdb_record_set_field(r, col, &val);
}

KdbStatus kdb_record_set_string(KdbRecord *r, const char *col, const char *v) {
    KdbValue val;
    KdbStatus st = kdb_value_from_string(v, KDB_TYPE_STRING, &val);
    if (st != KDB_OK) return st;
    st = kdb_record_set_field(r, col, &val);
    kdb_value_free(&val);
    return st;
}

KdbStatus kdb_record_set_null(KdbRecord *r, const char *col) {
    KdbValue val; kdb_value_from_null(&val);
    return kdb_record_set_field(r, col, &val);
}

KdbStatus kdb_record_set_blob(KdbRecord *r, const char *col, const void *data, size_t len) {
    KdbValue val;
    KdbStatus st = kdb_value_from_blob(data, len, &val);
    if (st != KDB_OK) return st;
    st = kdb_record_set_field(r, col, &val);
    kdb_value_free(&val);
    return st;
}

KdbStatus kdb_record_update_field(KdbRecord      *r,
                                  const char     *col_name,
                                  const KdbValue *new_value) {
    if (!r || !col_name || !new_value) {
        kdb_err_null_arg("r/col_name/new_value", "kdb_record_update_field");
        return KDB_ERR_BAD_ARG;
    }
    for (uint32_t i = 0; i < r->field_count; i++) {
        if (strcmp(r->fields[i].col_name, col_name) == 0) {
            kdb_value_free(&r->fields[i].value);
            KdbStatus st = kdb_value_copy(new_value, &r->fields[i].value);
            if (st == KDB_OK) r->updated_at = (uint64_t)time(NULL);
            return st;
        }
    }
    kdb_err_field_not_found(col_name, "record");
    return KDB_ERR_NOT_FOUND;
}


const KdbRecordField *kdb_record_get_field(const KdbRecord *r, const char *col_name) {
    if (!r || !col_name) return NULL;
    for (uint32_t i = 0; i < r->field_count; i++) {
        if (strcmp(r->fields[i].col_name, col_name) == 0)
            return &r->fields[i];
    }
    return NULL;
}

KdbStatus kdb_record_get_int(const KdbRecord *r, const char *col, int64_t *out) {
    const KdbRecordField *f = kdb_record_get_field(r, col);
    if (!f) { kdb_err_field_not_found(col, "record"); return KDB_ERR_NOT_FOUND; }
    if (f->value.type != KDB_TYPE_INT) {
        kdb_err_bad_type(col, KDB_TYPE_INT, f->value.type);
        return KDB_ERR_BAD_TYPE;
    }
    *out = f->value.v.as_int;
    return KDB_OK;
}

KdbStatus kdb_record_get_float(const KdbRecord *r, const char *col, double *out) {
    const KdbRecordField *f = kdb_record_get_field(r, col);
    if (!f) { kdb_err_field_not_found(col, "record"); return KDB_ERR_NOT_FOUND; }
    if (f->value.type == KDB_TYPE_INT) { *out = (double)f->value.v.as_int; return KDB_OK; }
    if (f->value.type != KDB_TYPE_FLOAT) {
        kdb_err_bad_type(col, KDB_TYPE_FLOAT, f->value.type);
        return KDB_ERR_BAD_TYPE;
    }
    *out = f->value.v.as_float;
    return KDB_OK;
}

KdbStatus kdb_record_get_bool(const KdbRecord *r, const char *col, uint8_t *out) {
    const KdbRecordField *f = kdb_record_get_field(r, col);
    if (!f) { kdb_err_field_not_found(col, "record"); return KDB_ERR_NOT_FOUND; }
    if (f->value.type != KDB_TYPE_BOOL) {
        kdb_err_bad_type(col, KDB_TYPE_BOOL, f->value.type);
        return KDB_ERR_BAD_TYPE;
    }
    *out = f->value.v.as_bool;
    return KDB_OK;
}

KdbStatus kdb_record_get_string(const KdbRecord *r, const char *col, const char **out) {
    const KdbRecordField *f = kdb_record_get_field(r, col);
    if (!f) { kdb_err_field_not_found(col, "record"); return KDB_ERR_NOT_FOUND; }
    if (f->value.type != KDB_TYPE_STRING) {
        kdb_err_bad_type(col, KDB_TYPE_STRING, f->value.type);
        return KDB_ERR_BAD_TYPE;
    }
    *out = f->value.v.as_string.data;
    return KDB_OK;
}

KdbStatus kdb_record_get_blob(const KdbRecord *r, const char *col, const uint8_t **data_out, size_t *len_out) {
    const KdbRecordField *f = kdb_record_get_field(r, col);
    if (!f) { kdb_err_field_not_found(col, "record"); return KDB_ERR_NOT_FOUND; }
    if (f->value.type != KDB_TYPE_BLOB) {
        kdb_err_bad_type(col, KDB_TYPE_BLOB, f->value.type);
        return KDB_ERR_BAD_TYPE;
    }
    *data_out = f->value.v.as_blob.data;
    *len_out  = f->value.v.as_blob.len;
    return KDB_OK;
}

int kdb_record_is_null(const KdbRecord *r, const char *col_name) {
    const KdbRecordField *f = kdb_record_get_field(r, col_name);
    return f && f->value.type == KDB_TYPE_NULL;
}

int kdb_record_has_field(const KdbRecord *r, const char *col_name) {
    return kdb_record_get_field(r, col_name) != NULL;
}


static void write_u8(uint8_t **p, uint8_t v) {
    **p = v; (*p)++;
}

static void write_u32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)(v);
    (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16);
    (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}

static void write_u64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; i++) { (*p)[i] = (uint8_t)(v >> (i * 8)); }
    *p += 8;
}

static void write_bytes(uint8_t **p, const void *src, size_t len) {
    memcpy(*p, src, len); *p += len;
}


static uint8_t read_u8(const uint8_t **p) {
    return *(*p)++;
}

static uint32_t read_u32(const uint8_t **p) {
    uint32_t v = (uint32_t)(*p)[0]
               | ((uint32_t)(*p)[1] << 8)
               | ((uint32_t)(*p)[2] << 16)
               | ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}

static uint64_t read_u64(const uint8_t **p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)(*p)[i] << (i * 8));
    *p += 8;
    return v;
}


size_t kdb_record_serial_size(const KdbRecord *r) {
    if (!r) return 0;
    

    size_t size = KDB_RECORD_FIXED_SIZE;

    for (uint32_t i = 0; i < r->field_count; i++) {
        
        size += KDB_FIELD_HEADER_SIZE;
        const KdbValue *v = &r->fields[i].value;
        switch (v->type) {
            case KDB_TYPE_INT:    size += 8; break;
            case KDB_TYPE_FLOAT:  size += 8; break;
            case KDB_TYPE_BOOL:   size += 1; break;
            case KDB_TYPE_STRING: size += 4 + v->v.as_string.len + 1; break;
            case KDB_TYPE_BLOB:   size += 4 + v->v.as_blob.len;       break;
            case KDB_TYPE_NULL:   break;
            default:              break;
        }
    }
    return size;
}

size_t kdb_record_serialize(const KdbRecord *r, uint8_t *buf, size_t buf_size) {
    if (!r || !buf) return 0;
    if (buf_size < kdb_record_serial_size(r)) return 0;

    uint8_t *p = buf;

    write_u64(&p, r->id);
    write_u64(&p, r->created_at);
    write_u64(&p, r->updated_at);
    write_u32(&p, r->field_count);
    write_u8 (&p, r->deleted);
    write_u8 (&p, 0); write_u8(&p, 0); write_u8(&p, 0); 

    for (uint32_t i = 0; i < r->field_count; i++) {
        const KdbRecordField *f = &r->fields[i];

        
        uint8_t name_buf[KDB_MAX_NAME_LEN];
        memset(name_buf, 0, KDB_MAX_NAME_LEN);
        snprintf((char *)name_buf, KDB_MAX_NAME_LEN, "%s", f->col_name);
        write_bytes(&p, name_buf, KDB_MAX_NAME_LEN);

        
        write_u8(&p, (uint8_t)f->value.type);
        write_u8(&p, 0); write_u8(&p, 0); write_u8(&p, 0);
        write_u8(&p, 0); write_u8(&p, 0); write_u8(&p, 0); write_u8(&p, 0);

        
        switch (f->value.type) {
            case KDB_TYPE_INT:
                write_u64(&p, (uint64_t)f->value.v.as_int);
                break;
            case KDB_TYPE_FLOAT: {
                uint64_t bits;
                memcpy(&bits, &f->value.v.as_float, 8);
                write_u64(&p, bits);
                break;
            }
            case KDB_TYPE_BOOL:
                write_u8(&p, f->value.v.as_bool);
                break;
            case KDB_TYPE_STRING: {
                uint32_t slen = (uint32_t)f->value.v.as_string.len;
                write_u32(&p, slen);
                if (slen > 0)
                    write_bytes(&p, f->value.v.as_string.data, slen);
                write_u8(&p, 0); 
                break;
            }
            case KDB_TYPE_BLOB: {
                uint32_t blen = (uint32_t)f->value.v.as_blob.len;
                write_u32(&p, blen);
                if (blen > 0)
                    write_bytes(&p, f->value.v.as_blob.data, blen);
                break;
            }
            case KDB_TYPE_NULL:
            default:
                break;
        }
    }

    return (size_t)(p - buf);
}

KdbRecord *kdb_record_deserialize(const uint8_t *buf,
                                  size_t         buf_size,
                                  size_t        *bytes_read) {
    if (!buf || buf_size < KDB_RECORD_FIXED_SIZE) {
        kdb_err_bad_arg("buf", "buffer too small to contain a record");
        return NULL;
    }

    const uint8_t *p     = buf;
    const uint8_t *p_end = buf + buf_size;

#define NEED(n) do { if ((size_t)(p_end - p) < (n)) { \
    kdb_err_io_corrupt("record buffer"); \
    kdb_record_free(r); return NULL; } } while (0)

    KdbRecord *r = (KdbRecord *)calloc(1, sizeof(KdbRecord));
    if (!r) { kdb_err_oom("KdbRecord deserialization"); return NULL; }

    NEED(32);
    r->id          = read_u64(&p);
    r->created_at  = read_u64(&p);
    r->updated_at  = read_u64(&p);
    r->field_count = read_u32(&p);
    r->deleted     = read_u8(&p);
    p += 3; 

    if (r->field_count > KDB_MAX_COLUMNS) {
        kdb_err_io_corrupt("record: field_count exceeds KDB_MAX_COLUMNS");
        kdb_record_free(r);
        return NULL;
    }

    r->fields = (KdbRecordField *)calloc(r->field_count, sizeof(KdbRecordField));
    if (!r->fields && r->field_count > 0) {
        kdb_err_oom("KdbRecordField array deserialization");
        kdb_record_free(r);
        return NULL;
    }

    for (uint32_t i = 0; i < r->field_count; i++) {
        KdbRecordField *f = &r->fields[i];

        NEED(KDB_MAX_NAME_LEN);
        memcpy(f->col_name, p, KDB_MAX_NAME_LEN);
        f->col_name[KDB_MAX_NAME_LEN - 1] = '\0';
        p += KDB_MAX_NAME_LEN;

        NEED(8);
        f->value.type = (KdbType)read_u8(&p);
        p += 7; 

        switch (f->value.type) {
            case KDB_TYPE_INT:
                NEED(8);
                f->value.v.as_int = (int64_t)read_u64(&p);
                break;
            case KDB_TYPE_FLOAT: {
                NEED(8);
                uint64_t bits = read_u64(&p);
                memcpy(&f->value.v.as_float, &bits, 8);
                break;
            }
            case KDB_TYPE_BOOL:
                NEED(1);
                f->value.v.as_bool = read_u8(&p);
                break;
            case KDB_TYPE_STRING: {
                NEED(4);
                uint32_t slen = read_u32(&p);
                if (slen > KDB_MAX_STRING_LEN) {
                    kdb_err_io_corrupt("record: string field too long");
                    kdb_record_free(r);
                    return NULL;
                }
                NEED(slen + 1);
                char *str = malloc(slen + 1);
                if (!str) { kdb_err_oom("string field"); kdb_record_free(r); return NULL; }
                memcpy(str, p, slen);
                str[slen] = '\0';
                p += slen + 1; 
                f->value.v.as_string.data = str;
                f->value.v.as_string.len  = slen;
                break;
            }
            case KDB_TYPE_BLOB: {
                NEED(4);
                uint32_t blen = read_u32(&p);
                NEED(blen);
                uint8_t *blob = malloc(blen);
                if (!blob && blen > 0) { kdb_err_oom("blob field"); kdb_record_free(r); return NULL; }
                if (blen > 0) memcpy(blob, p, blen);
                p += blen;
                f->value.v.as_blob.data = blob;
                f->value.v.as_blob.len  = blen;
                break;
            }
            case KDB_TYPE_NULL:
            default:
                break;
        }
    }

#undef NEED

    if (bytes_read) *bytes_read = (size_t)(p - buf);
    return r;
}


KdbStatus kdb_record_write(const KdbRecord *r, FILE *fp) {
    if (!r || !fp) {
        kdb_err_null_arg("r/fp", "kdb_record_write");
        return KDB_ERR_BAD_ARG;
    }

    size_t  size = kdb_record_serial_size(r);
    uint8_t *buf  = malloc(size);
    if (!buf) { kdb_err_oom("record write buffer"); return KDB_ERR_OOM; }

    size_t written_bytes = kdb_record_serialize(r, buf, size);
    if (written_bytes == 0) {
        free(buf);
        kdb_err_io("record", "serialize");
        return KDB_ERR_IO;
    }

    
    uint32_t sz32 = (uint32_t)written_bytes;
    if (fwrite(&sz32, 4, 1, fp) != 1) {
        free(buf); kdb_err_io("record", "write size prefix"); return KDB_ERR_IO;
    }
    if (fwrite(buf, 1, written_bytes, fp) != written_bytes) {
        free(buf); kdb_err_io("record", "fwrite"); return KDB_ERR_IO;
    }

    free(buf);
    return KDB_OK;
}

KdbRecord *kdb_record_read(FILE *fp) {
    if (!fp) return NULL;

    
    uint32_t sz32 = 0;
    if (fread(&sz32, 4, 1, fp) != 1) return NULL; 

    if (sz32 == 0 || sz32 > (KDB_MAX_COLUMNS * (KDB_FIELD_HEADER_SIZE + KDB_MAX_STRING_LEN) + KDB_RECORD_FIXED_SIZE)) {
        kdb_err_io_corrupt("record: implausible size prefix");
        return NULL;
    }

    uint8_t *buf = malloc(sz32);
    if (!buf) { kdb_err_oom("record read buffer"); return NULL; }

    if (fread(buf, 1, sz32, fp) != sz32) {
        free(buf);
        kdb_err_io("record", "fread");
        return NULL;
    }

    size_t bytes_read = 0;
    KdbRecord *r = kdb_record_deserialize(buf, sz32, &bytes_read);
    free(buf);
    return r;
}


void kdb_record_print(const KdbRecord *r, FILE *fp) {
    if (!r || !fp) return;
    fprintf(fp, "{ id=%llu, created_at=%llu, deleted=%d, fields=[",
            (unsigned long long)r->id,
            (unsigned long long)r->created_at,
            r->deleted);
    char vbuf[256];
    for (uint32_t i = 0; i < r->field_count; i++) {
        kdb_value_to_str(&r->fields[i].value, vbuf, sizeof(vbuf));
        fprintf(fp, "%s%s: %s",
                i > 0 ? ", " : "",
                r->fields[i].col_name,
                vbuf);
    }
    fprintf(fp, "] }\n");
}

int kdb_record_is_deleted(const KdbRecord *r) {
    return r && r->deleted;
}

void kdb_record_mark_deleted(KdbRecord *r) {
    if (!r) return;
    r->deleted    = 1;
    r->updated_at = (uint64_t)time(NULL);
}

int kdb_record_cmp_id(const void *a, const void *b) {
    const KdbRecord *ra = (const KdbRecord *)a;
    const KdbRecord *rb = (const KdbRecord *)b;
    if (ra->id < rb->id) return -1;
    if (ra->id > rb->id) return  1;
    return 0;
}