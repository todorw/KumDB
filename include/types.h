#ifndef KUMDB_TYPES_H
#define KUMDB_TYPES_H

#include "internal.h"

KdbType kdb_type_infer(const char *raw);

KdbStatus kdb_value_from_string(const char *raw, KdbType hint, KdbValue *out);
KdbStatus kdb_value_from_int   (int64_t v,   KdbValue *out);
KdbStatus kdb_value_from_float (double v,    KdbValue *out);
KdbStatus kdb_value_from_bool  (uint8_t v,   KdbValue *out);
KdbStatus kdb_value_from_null  (KdbValue *out);
KdbStatus kdb_value_from_blob  (const void *data, size_t len, KdbValue *out);

KdbStatus kdb_value_copy(const KdbValue *src, KdbValue *dst);
void      kdb_value_free(KdbValue *v);

int kdb_value_compare(const KdbValue *a, const KdbValue *b);

int kdb_value_matches(const KdbValue *field,
                      KdbOperator     op,
                      const KdbValue *filter_value,
                      const KdbValue *filter_value2);

const char *kdb_type_name(KdbType type);
const char *kdb_op_name  (KdbOperator op);

KdbStatus kdb_parse_filter_key(const char  *key,
                               char         col_name_out[KDB_MAX_NAME_LEN],
                               KdbOperator *op_out);

int kdb_value_to_str(const KdbValue *v, char *buf, size_t buf_size);

KdbStatus kdb_value_cast(const KdbValue *src, KdbType target_type, KdbValue *dst);

int kdb_str_is_int  (const char *s);
int kdb_str_is_float(const char *s);
int kdb_str_is_bool (const char *s);
int kdb_str_is_null (const char *s);

#endif 