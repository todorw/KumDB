#ifndef KUMDB_RECORD_H
#define KUMDB_RECORD_H

#include "internal.h"
#include "types.h"


KdbRecord *kdb_record_new(uint32_t field_count);


KdbRecord *kdb_record_copy(const KdbRecord *src);


void kdb_record_free(KdbRecord *r);


void kdb_record_free_array(KdbRecord *arr, size_t count);


KdbStatus kdb_record_set_field(KdbRecord  *r,
                               const char *col_name,
                               const KdbValue *value);


KdbStatus kdb_record_set_int   (KdbRecord *r, const char *col, int64_t  v);
KdbStatus kdb_record_set_float (KdbRecord *r, const char *col, double   v);
KdbStatus kdb_record_set_bool  (KdbRecord *r, const char *col, uint8_t  v);
KdbStatus kdb_record_set_string(KdbRecord *r, const char *col, const char *v);
KdbStatus kdb_record_set_null  (KdbRecord *r, const char *col);
KdbStatus kdb_record_set_blob  (KdbRecord *r, const char *col, const void *data, size_t len);


KdbStatus kdb_record_update_field(KdbRecord      *r,
                                  const char     *col_name,
                                  const KdbValue *new_value);


const KdbRecordField *kdb_record_get_field(const KdbRecord *r, const char *col_name);


KdbStatus kdb_record_get_int   (const KdbRecord *r, const char *col, int64_t  *out);
KdbStatus kdb_record_get_float (const KdbRecord *r, const char *col, double   *out);
KdbStatus kdb_record_get_bool  (const KdbRecord *r, const char *col, uint8_t  *out);
KdbStatus kdb_record_get_string(const KdbRecord *r, const char *col, const char **out);
KdbStatus kdb_record_get_blob  (const KdbRecord *r, const char *col, const uint8_t **data_out, size_t *len_out);


int kdb_record_is_null(const KdbRecord *r, const char *col_name);


int kdb_record_has_field(const KdbRecord *r, const char *col_name);


size_t kdb_record_serial_size(const KdbRecord *r);


size_t kdb_record_serialize(const KdbRecord *r, uint8_t *buf, size_t buf_size);


KdbRecord *kdb_record_deserialize(const uint8_t *buf,
                                  size_t         buf_size,
                                  size_t        *bytes_read);


KdbStatus kdb_record_write(const KdbRecord *r, FILE *fp);


KdbRecord *kdb_record_read(FILE *fp);


void kdb_record_print(const KdbRecord *r, FILE *fp);


int kdb_record_is_deleted(const KdbRecord *r);


void kdb_record_mark_deleted(KdbRecord *r);


int kdb_record_cmp_id(const void *a, const void *b);

#endif