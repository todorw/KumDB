#ifndef KUMDB_INTERNAL_H
#define KUMDB_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>


#define KDB_MAGIC              0x4B554D44
/* File format 3.0 -- same story as the 1.x -> 2.0 break (see below), one
 * more time: composite (multi-column) foreign keys need an array of
 * table-name-sized strings per definition (KdbCompositeFkDef), nowhere
 * close to fitting in 2.0's remaining ~63-byte padding budget the way
 * every earlier same-major-version extension (KdbColumn.on_delete/
 * on_update included) managed to. Every extension within one major
 * version has stayed padding-carved so far specifically so
 * kdb_storage_validate_header's version_major-only check stays honest
 * (same struct layout across a whole major version); breaking that
 * invariant without bumping the major version would make the version
 * check silently lie. Caught the same way 1.x->2.0 already was: a clean,
 * intentional "versions are incompatible" rejection, not a migration.
 *
 * 2.0 (previous): KdbColumn/KdbTableHeader both grew real fields (FK
 * definitions, CHECK constraints, then on_delete/on_update) instead of
 * carving them out of reserved padding the way every 1.x extension did;
 * a foreign key alone needs a whole table-name-sized string, far more
 * than 1.x's padding budget had left. Fine for this project (no deployed
 * 1.x/2.x data to migrate); a real migration tool would read each older
 * header layout explicitly and rewrite it forward. */
#define KDB_VERSION_MAJOR      3
#define KDB_VERSION_MINOR      0
#define KDB_VERSION_PATCH      0

#define KDB__STR(x) #x
#define KDB__XSTR(x) KDB__STR(x)
#define KDB_VERSION_MAJOR_STR KDB__XSTR(KDB_VERSION_MAJOR)
#define KDB_VERSION_MINOR_STR KDB__XSTR(KDB_VERSION_MINOR)
#define KDB_VERSION_PATCH_STR KDB__XSTR(KDB_VERSION_PATCH)


#define KDB_MAX_TABLES         256
#define KDB_MAX_COLUMNS        64
#define KDB_MAX_NAME_LEN       128
#define KDB_MAX_STRING_LEN     4096
#define KDB_MAX_RECORDS        (1 << 24)
#define KDB_MAX_FILTER_KEYS    32
#define KDB_MAX_FILTER_GROUPS  4
#define KDB_MAX_BATCH_SIZE     65536
#define KDB_PAGE_SIZE          4096
#define KDB_INDEX_BUCKETS      1024

/* A real composite (multi-column) index covers at most this many columns,
 * and a table can have at most this many composite indexes -- small fixed
 * bounds so the definitions fit in KdbTableHeader's existing reserved
 * padding (see KdbCompositeIndexDef below) rather than growing the file
 * format's on-disk layout. */
#define KDB_MAX_COMPOSITE_COLS    4
#define KDB_MAX_COMPOSITE_INDEXES 4

/* A table can have at most this many CHECK constraints (see KdbCheckDef) --
 * independent of column count, since several could target the same column. */
#define KDB_MAX_CHECK_CONSTRAINTS 8

/* ARRAY/OBJECT values: bounds so a corrupt/malicious file can't make
 * deserialization recurse or allocate without limit. */
#define KDB_MAX_NEST_DEPTH     16
#define KDB_MAX_NEST_ELEMS     KDB_MAX_COLUMNS


typedef enum {
    KDB_TYPE_NULL    = 0,
    KDB_TYPE_INT     = 1,
    KDB_TYPE_FLOAT   = 2,
    KDB_TYPE_BOOL    = 3,
    KDB_TYPE_STRING  = 4,
    KDB_TYPE_BLOB    = 5,
    KDB_TYPE_ARRAY   = 6,
    KDB_TYPE_OBJECT  = 7,
    KDB_TYPE_UNKNOWN = 255
} KdbType;


/* Added in file format 1.1 (KDB_VERSION_MINOR). Files written by 1.0 never
 * contain these type tags, so they're unaffected; a 1.0 engine opening a
 * 1.1 file will choke the moment it hits an ARRAY/OBJECT field, same as
 * any unknown-format-extension situation -- expected, not a corruption bug. */
typedef struct KdbValue {
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
        struct {
            struct KdbValue *items;
            size_t           count;
        } as_array;
        struct {
            struct KdbRecordField *fields;
            uint32_t                count;
        } as_object;
    } v;
} KdbValue;


typedef struct {
    char     name[KDB_MAX_NAME_LEN];
    KdbType  type;
    uint8_t  nullable;
    uint8_t  indexed;
    /* Added in file format 1.2 (KDB_VERSION_MINOR) -- takes one byte out of
     * what used to be _pad[6], so a 1.1-or-earlier file (whose column
     * entries were always memset to 0 before being written -- see
     * kdb_table_add_column/kdb_storage_create) reads back unique=0 for
     * every column, exactly matching that file's actual pre-1.2 behavior
     * (no real uniqueness enforcement existed yet). Forward-compatible
     * without a migration step. UNIQUE/PRIMARY KEY set this; a plain
     * INDEX/INDEXED/KEY (lookup-only, no enforcement) does not -- see
     * sql__parse_column_modifiers in sql.c. */
    uint8_t  unique;
    /* Added in file format 2.0 -- a real struct growth, not a padding
     * carve-out (a foreign key needs a whole table-name-sized string; see
     * the KDB_VERSION_MAJOR comment above). has_fk==0 means this column
     * has no foreign key; when it's 1, every INSERT/UPDATE giving this
     * column a non-NULL value requires a matching row in
     * fk_ref_table.fk_ref_col to already exist (kdb__check_fk_insert in
     * kumdb.c -- needs KumDB* to open the referenced table, so it can't
     * live in table.c like NOT NULL/UNIQUE do). One FK per column, no
     * composite (multi-column) foreign keys. */
    uint8_t  has_fk;
    char     fk_ref_table[KDB_MAX_NAME_LEN];
    char     fk_ref_col[KDB_MAX_NAME_LEN];
    /* Added in file format 2.0, alongside has_fk/fk_ref_table/fk_ref_col
     * (part of the same growth, this pad slot was already reserved --
     * within-2.0 budget, not a new minor version). What happens to a
     * referencing row when the row it points to is deleted/updated --
     * KdbFkAction, KDB_FK_RESTRICT (0) is the default a pre-#40 FK
     * (task #35) always meant, so an existing column reading these two
     * bytes as 0 behaves exactly as it always did. */
    uint8_t  on_delete;
    uint8_t  on_update;
    uint8_t  _pad[2]; /* reserved for a future 2.x minor extension, same discipline as _pad above used to follow */
} KdbColumn;

/* has_fk's on_delete/on_update action. RESTRICT (0, the original-and-
 * only behavior before #40) rejects the delete/update if any row still
 * references the value; CASCADE deletes/updates the referencing rows
 * too (deleting them, or propagating the new value, respectively);
 * SET_NULL sets the referencing rows' FK column to NULL instead (the
 * column must be nullable -- checked at FK-creation time, not deferred
 * to whenever a delete/update actually needs it). CASCADE/SET_NULL can
 * chain across multiple tables (A -> B -> C); capped at
 * KDB_FK_MAX_CASCADE_DEPTH to catch a cycle instead of recursing
 * forever. */
typedef enum { KDB_FK_RESTRICT = 0, KDB_FK_CASCADE = 1, KDB_FK_SET_NULL = 2 } KdbFkAction;
#define KDB_FK_MAX_CASCADE_DEPTH 8


/* A real composite (multi-column) index's definition: which columns (by
 * position in KdbTableHeader.columns[], in the order the index covers
 * them), stored compactly enough to fit in KdbTableHeader's existing
 * reserved padding -- see below. n_cols==0 means an unused slot. */
typedef struct {
    uint8_t col_positions[KDB_MAX_COMPOSITE_COLS];
    uint8_t n_cols;
} KdbCompositeIndexDef;

/* A CHECK (col_name OP literal) table constraint -- added in file format
 * 2.0, alongside KdbColumn's FK fields (see KDB_VERSION_MAJOR above).
 * op is restricted to the six plain comparisons (EQ/NEQ/GT/GTE/LT/LTE,
 * see kdb_table_add_check in table.c) -- no BETWEEN/IN/LIKE/etc, which
 * would need more than one literal or a list. The literal is a fixed-size
 * value (val_type says which union/string member is meaningful), never a
 * heap pointer, so this stays plain, fwrite-safe data like the rest of
 * KdbTableHeader -- a real KdbValue's STRING/BLOB/ARRAY/OBJECT variants
 * hold heap pointers that can't be persisted this way. */
typedef struct {
    char     col_name[KDB_MAX_NAME_LEN];
    uint8_t  op;         /* KdbOperator, one of EQ/NEQ/GT/GTE/LT/LTE */
    uint8_t  val_type;   /* KdbType: INT/FLOAT/BOOL/STRING */
    uint8_t  _pad[2];
    int64_t  as_int;
    double   as_float;
    uint8_t  as_bool;
    char     as_string[128];
} KdbCheckDef;

/* A composite (multi-column) foreign key -- added in file format 3.0 (see
 * KDB_VERSION_MAJOR above). col_positions/n_cols name this table's side
 * (by column position, same convention as KdbCompositeIndexDef); ref_table/
 * ref_cols name the referenced table and its columns, in matching order
 * (ref_cols[i] corresponds to the column at col_positions[i]). Unlike
 * single-column FK (KdbColumn.has_fk), a composite FK can never reference
 * the pseudo-columns id/created_at/updated_at -- it inherently deals with
 * multiple real, user-defined stored columns on both sides, and "id" isn't
 * meaningful as one piece of a multi-column key. Enforcement uses MATCH
 * SIMPLE semantics: if any referenced-side column is NULL (or the row is
 * missing that field), the whole check is skipped for that row. n_cols==0
 * means an unused slot. on_delete/on_update reuse KdbFkAction, same
 * RESTRICT/CASCADE/SET_NULL semantics and cascade depth cap as single-
 * column FK. */
typedef struct {
    uint8_t col_positions[KDB_MAX_COMPOSITE_COLS];
    uint8_t n_cols;
    char    ref_table[KDB_MAX_NAME_LEN];
    char    ref_cols[KDB_MAX_COMPOSITE_COLS][KDB_MAX_NAME_LEN];
    uint8_t on_delete;
    uint8_t on_update;
} KdbCompositeFkDef;
#define KDB_MAX_COMPOSITE_FKS 4

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
    /* Added in file format 1.3 (KDB_VERSION_MINOR) -- takes 21 of the 64
     * bytes that used to be _pad1[64] in full, so a pre-1.3 file (whose
     * padding was always zeroed before being written -- see
     * kdb_storage_create) reads back n_composite_indexes=0, exactly its
     * real prior behavior (no composite indexes existed yet). Forward-
     * compatible without a migration step, same reasoning as KdbColumn's
     * "unique" byte in file format 1.2. */
    KdbCompositeIndexDef composite_indexes[KDB_MAX_COMPOSITE_INDEXES];
    uint8_t  n_composite_indexes;
    uint8_t  _pad1[43];
    /* Added in file format 2.0, a real struct growth like KdbColumn's FK
     * fields above (see KDB_VERSION_MAJOR) -- CHECK constraints. */
    KdbCheckDef checks[KDB_MAX_CHECK_CONSTRAINTS];
    uint8_t     n_checks;
    uint8_t     _pad2[63]; /* reserved for a future 2.x minor extension */
    /* Added in file format 3.0, a real struct growth (see KDB_VERSION_MAJOR
     * above) -- composite foreign keys, alongside 2.0's single-column FK
     * fields and CHECK constraints. */
    KdbCompositeFkDef composite_fks[KDB_MAX_COMPOSITE_FKS];
    uint8_t           n_composite_fks;
    uint8_t           _pad3[63]; /* reserved for a future 3.x minor extension, same discipline as _pad2 */
    KdbColumn columns[KDB_MAX_COLUMNS];
} KdbTableHeader;


typedef struct KdbRecordField {
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
    KDB_OP_IS_NOT_NULL = 12,
    KDB_OP_LIKE        = 13,
    KDB_OP_ILIKE       = 14,
    KDB_OP_REGEXP      = 15
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
} KdbFilterGroup;

/* Groups are OR'd together; filters within a group are AND'd (same
 * precedence as SQL: AND binds tighter than OR, no parens/nesting). A
 * fresh query starts with one empty group, same as always. */
typedef struct {
    KdbFilterGroup groups[KDB_MAX_FILTER_GROUPS];
    uint32_t       group_count;
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
    char          col_name[KDB_MAX_NAME_LEN]; /* first/primary column -- always set, even for a composite index */
    /* Additional columns for a real composite (multi-column) index --
     * n_extra_cols==0 means this is an ordinary single-column index
     * (existing behavior, completely unchanged: every lookup/insert path
     * that only ever dealt with col_name still works exactly as before).
     * When n_extra_cols>0, every hash/insert/lookup combines col_name's
     * value with all of extra_cols's values together (kdb_index_hash_multi)
     * instead of hashing col_name alone. */
    char          extra_cols[KDB_MAX_COMPOSITE_COLS - 1][KDB_MAX_NAME_LEN];
    uint32_t      n_extra_cols;
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
    /* SQL BEGIN/COMMIT/ROLLBACK's open transaction, or NULL -- really a
     * KdbTx*, but KdbTx is defined in kumdb.h, which includes this header
     * (not the other way around), so it can't be named here without a
     * circular include. Left void* and cast at the handful of sql.c call
     * sites that touch it; nothing in this file dereferences it. */
    void     *sql_tx;
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
#define KDB_VALUE_HEADER_SIZE  (1 + 7)  /* type tag + padding, no name -- unnamed array elements */

/* Sanity ceiling on a single serialized record, checked before trusting a
 * size prefix read from disk enough to malloc it. Generous enough for any
 * realistic record (including nested ARRAY/OBJECT values) while still
 * rejecting an obviously-corrupt size prefix without a huge allocation. */
#define KDB_MAX_RECORD_SERIAL_SIZE (16u * 1024 * 1024)


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