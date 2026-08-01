#ifndef KUMDB_INTERNAL_H
#define KUMDB_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>


#define KDB_MAGIC              0x4B554D44  
#define KDB_VERSION_MAJOR      1
#define KDB_VERSION_MINOR      0
#define KDB_VERSION_PATCH      0


#define KDB_MAX_TABLES         256
#define KDB_MAX_COLUMNS        64
#define KDB_MAX_NAME_LEN       128
#define KDB_MAX_STRING_LEN     4096
#define KDB_MAX_RECORDS        (1 << 24)   
#define KDB_MAX_FILTER_KEYS    32
#define KDB_MAX_BATCH_SIZE     65536
#define KDB_PAGE_SIZE          4096
#define KDB_INDEX_BUCKETS      1024


typedef enum {
    KDB_TYPE_NULL    = 0,
    KDB_TYPE_INT     = 1,   
    KDB_TYPE_FLOAT   = 2,   
    KDB_TYPE_BOOL    = 3,   
    KDB_TYPE_STRING  = 4,   
    KDB_TYPE_BLOB    = 5,   
    KDB_TYPE_UNKNOWN = 255
} KdbType;


typedef struct {
    KdbType type;
    union {
        int64_t  as_int;
        double   as_float;
        uint8_t  as_bool;
        struct {
            char    *data;
            size_t   len;
        } as_string;
        struct {
            uint8_t *data;
            size_t   len;
        } as_blob;
    } v;
} KdbValue;


typedef struct {
    char     name[KDB_MAX_NAME_LEN];
    KdbType  type;
    uint8_t  nullable;       
    uint8_t  indexed;        
    uint8_t  _pad[6];
} KdbColumn;


typedef struct {
    uint32_t magic;                          
    uint8_t  version_major;
    uint8_t  version_minor;
    uint8_t  version_patch;
    uint8_t  _pad0;
    char     table_name[KDB_MAX_NAME_LEN];
    uint32_t column_count;
    uint64_t record_count;
    uint64_t next_id;                        
    uint64_t created_at;                     
    uint64_t updated_at;
    uint64_t data_offset;                    
    uint64_t index_offset;                   
    uint8_t  _pad1[64];                      
    KdbColumn columns[KDB_MAX_COLUMNS];
} KdbTableHeader;


typedef struct {
    char     col_name[KDB_MAX_NAME_LEN];
    KdbValue value;
} KdbRecordField;


typedef struct {
    uint64_t  id;            
    uint64_t  created_at;    
    uint64_t  updated_at;
    uint32_t  field_count;
    uint8_t   deleted;       
    uint8_t   _pad[3];
    KdbRecordField *fields;        
} KdbRecord;


typedef enum {
    KDB_OP_EQ          = 0,   
    KDB_OP_NEQ         = 1,   
    KDB_OP_GT          = 2,   
    KDB_OP_GTE         = 3,   
    KDB_OP_LT          = 4,   
    KDB_OP_LTE         = 5,   
    KDB_OP_CONTAINS    = 6,   
    KDB_OP_STARTSWITH  = 7,   
    KDB_OP_ENDSWITH    = 8,   
    KDB_OP_IN          = 9,   
    KDB_OP_BETWEEN     = 10,  
    KDB_OP_IS_NULL     = 11,  
    KDB_OP_IS_NOT_NULL = 12   
} KdbOperator;


typedef struct {
    char        col_name[KDB_MAX_NAME_LEN];
    KdbOperator op;
    KdbValue    value;
    KdbValue    value2;   
} KdbFilter;


typedef struct {
    KdbFilter filters[KDB_MAX_FILTER_KEYS];
    uint32_t  count;
} KdbQuery;


typedef struct {
    KdbRecord *rows;       
    size_t     count;
    size_t     capacity;
} KdbResult;


typedef struct KdbIndexNode {
    uint64_t            record_id;
    uint64_t            file_offset;    
    struct KdbIndexNode *next;           
} KdbIndexNode;

typedef struct {
    char          col_name[KDB_MAX_NAME_LEN];
    KdbIndexNode *buckets[KDB_INDEX_BUCKETS];
} KdbIndex;


typedef struct {
    char           name[KDB_MAX_NAME_LEN];
    char           path[4096];
    FILE          *fp;
    KdbTableHeader header;
    KdbIndex     **indices;
    uint32_t       index_count;
    uint8_t        dirty;           
    uint8_t        read_only;
    uint8_t        _pad[6];
    int            lock_fd;         
} KdbTable;


typedef struct {
    char      data_dir[4096];       
    KdbTable *tables[KDB_MAX_TABLES];
    uint32_t  table_count;
    uint8_t   read_only;
    uint8_t   _pad[7];
} KumDB;


typedef enum {
    KDB_OK               =  0,
    KDB_ERR_OOM          = -1,   
    KDB_ERR_IO           = -2,   
    KDB_ERR_NOT_FOUND    = -3,   
    KDB_ERR_EXISTS       = -4,   
    KDB_ERR_BAD_TYPE     = -5,   
    KDB_ERR_BAD_FILTER   = -6,   
    KDB_ERR_BAD_ARG      = -7,   
    KDB_ERR_LOCKED       = -8,   
    KDB_ERR_CORRUPT      = -9,   
    KDB_ERR_FULL         = -10,  
    KDB_ERR_VALIDATION   = -11,
    KDB_ERR_READ_ONLY    = -12,
    KDB_ERR_SQL_SYNTAX   = -13,
    KDB_ERR_UNKNOWN      = -99
} KdbStatus;


#define KDB_RECORD_FIXED_SIZE  (8 + 8 + 8 + 4 + 1 + 3)   
#define KDB_FIELD_HEADER_SIZE  (KDB_MAX_NAME_LEN + 1 + 7) 


#define KDB_UNUSED(x)       ((void)(x))
#define KDB_ARRAY_LEN(a)    (sizeof(a) / sizeof((a)[0]))
#define KDB_MIN(a, b)       ((a) < (b) ? (a) : (b))
#define KDB_MAX(a, b)       ((a) > (b) ? (a) : (b))
#define KDB_CLAMP(v, lo, hi) KDB_MIN(KDB_MAX((v), (lo)), (hi))


#define KDB_STRLCPY(dst, src, size) \
    do { \
        snprintf((dst), (size), "%s", (src)); \
    } while (0)


#define KDB_ALLOC(type)     ((type *)calloc(1, sizeof(type)))


#define KDB_FREE(ptr) \
    do { \
        free(ptr); \
        (ptr) = NULL; \
    } while (0)


#endif