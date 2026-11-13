/*
 * Copyright 2025 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file stoolap_mariadb.h
 * @brief The MariaDB plugin's interface to the stoolap engine.
 *
 * Implemented by the Rust crate in rust/ (linked into ha_stoolap.so as a
 * static library), which links stoolap as a Rust crate. These functions
 * keep the names, types and packed-result format of stoolap's own C API
 * (include/stoolap.h, of which they are a subset) so the plugin's calls
 * are unchanged; the crate's lower-level functions are added beside them.
 */

#ifndef STOOLAP_MARIADB_H
#define STOOLAP_MARIADB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Opaque handles
 * ========================================================================= */

/** Database connection handle. */
typedef struct StoolapDB     StoolapDB;

/** Prepared statement handle. */

/** Transaction handle. */
typedef struct StoolapTx     StoolapTx;

/** Result set iterator handle. */
typedef struct StoolapRows   StoolapRows;

/* =========================================================================
 * Status codes
 * ========================================================================= */

/** Operation succeeded. */
#define STOOLAP_OK      0

/** Operation failed. Call the appropriate *_errmsg() for details. */
#define STOOLAP_ERROR   1

/** stoolap_rows_next(): a row is available. */
#define STOOLAP_ROW     100

/** stoolap_rows_next(): no more rows. */
#define STOOLAP_DONE    101

/* =========================================================================
 * Value type codes
 * ========================================================================= */

#define STOOLAP_TYPE_NULL       0
#define STOOLAP_TYPE_INTEGER    1
#define STOOLAP_TYPE_FLOAT      2
#define STOOLAP_TYPE_TEXT       3
#define STOOLAP_TYPE_BOOLEAN    4
#define STOOLAP_TYPE_TIMESTAMP  5
#define STOOLAP_TYPE_JSON       6
#define STOOLAP_TYPE_BLOB       7

/* =========================================================================
 * Isolation levels
 * ========================================================================= */

#define STOOLAP_ISOLATION_READ_COMMITTED    0
#define STOOLAP_ISOLATION_SNAPSHOT          1

/* =========================================================================
 * Parameter value struct
 * ========================================================================= */

/** Tagged union for passing parameter values to SQL statements. */
typedef struct StoolapValue {
    /** One of STOOLAP_TYPE_* constants. */
    int32_t value_type;
    int32_t _padding;
    union {
        int64_t  integer;
        double   float64;
        int32_t  boolean;
        struct { const char*    ptr; int64_t len; } text;   /**< STOOLAP_TYPE_TEXT / _JSON */
        struct { const uint8_t* ptr; int64_t len; } blob;  /**< STOOLAP_TYPE_BLOB: packed f32, len must be multiple of 4 */
        /** Nanoseconds since Unix epoch (UTC). */
        int64_t  timestamp_nanos;
    } v;
} StoolapValue;


/* =========================================================================
 * Library info
 * ========================================================================= */

/**
 * Returns the stoolap version string (e.g. "0.4.0").
 * The returned pointer is static and must NOT be freed.
 */
const char* stoolap_version(void);

/* =========================================================================
 * Thread safety
 * =========================================================================
 *
 * A single StoolapDB handle must NOT be used concurrently from multiple
 * threads. To use stoolap from multiple threads, clone the handle with
 * stoolap_clone(). Each clone shares the underlying engine (data, indexes,
 * transactions) but has its own executor and error state.
 *
 * StoolapRows and StoolapTx handles must remain on the thread that created
 * them.
 *
 * Recommended pattern:
 *   StoolapDB* db;
 *   stoolap_open("file:///path/to/db", &db);
 *
 *   // Per worker thread:
 *   StoolapDB* thread_db;
 *   stoolap_clone(db, &thread_db);
 *   // ... use thread_db exclusively in this thread ...
 *   stoolap_close(thread_db);
 *
 *   stoolap_close(db);
 */

/* =========================================================================
 * Database lifecycle
 * ========================================================================= */

/**
 * Open a database connection.
 *
 * @param dsn     DSN string. Supported schemes:
 *                  "memory://"           - in-memory database
 *                  "file:///path/to/db"  - persistent database
 *                File DSN supports query parameters:
 *                  sync_mode=none|normal|full
 *                  compression=on|off
 *                  wal_flush_trigger=<bytes>
 *                  checkpoint_interval=<seconds>
 *                Example: "file:///tmp/mydb?sync_mode=full&compression=on"
 * @param out_db  On success, receives the database handle.
 * @return STOOLAP_OK on success. On failure, *out_db is NULL.
 *         Use stoolap_errmsg(NULL) to get the error message.
 */
int32_t stoolap_open(const char* dsn, StoolapDB** out_db);

/**
 * Clone a database handle for multi-threaded use.
 *
 * The new handle shares the same underlying engine but has its own
 * executor and error state. Each clone must be closed independently.
 *
 * @param db      Source database handle.
 * @param out_db  On success, receives the cloned handle.
 */
int32_t stoolap_clone(const StoolapDB* db, StoolapDB** out_db);

/**
 * Close a database connection and free all associated resources.
 * Safe to call with NULL (no-op).
 * After this call, the db pointer is invalid.
 */
int32_t stoolap_close(StoolapDB* db);

/**
 * Get the last error message for a database handle.
 *
 * @param db  Database handle, or NULL for the last global error
 *            (e.g. from stoolap_open failures).
 * @return Pointer valid until the next API call on this handle.
 *         Returns "" if no error. Must NOT be freed.
 */
const char* stoolap_errmsg(const StoolapDB* db);

/* =========================================================================
 * Execute (DDL/DML)
 * ========================================================================= */

/**
 * Execute a SQL statement without parameters.
 *
 * @param rows_affected  If non-NULL, receives the number of affected rows.
 */
int32_t stoolap_exec(StoolapDB* db, const char* sql, int64_t* rows_affected);

/**
 * Execute a SQL statement with positional parameters ($1, $2, ...).
 *
 * @param params      Array of parameter values. May be NULL if params_len is 0.
 * @param params_len  Number of parameters.
 * @param rows_affected  If non-NULL, receives the number of affected rows.
 */
int32_t stoolap_exec_params(
    StoolapDB* db,
    const char* sql,
    const StoolapValue* params,
    int32_t params_len,
    int64_t* rows_affected
);

/* =========================================================================
 * Query (returns result rows)
 * ========================================================================= */

/**
 * Execute a query without parameters.
 * On success, *out_rows must be closed with stoolap_rows_close().
 */
int32_t stoolap_query(StoolapDB* db, const char* sql, StoolapRows** out_rows);

/* =========================================================================
 * Transactions
 * ========================================================================= */

/**
 * Begin a transaction with a specific isolation level.
 * @param isolation  STOOLAP_ISOLATION_READ_COMMITTED or STOOLAP_ISOLATION_SNAPSHOT.
 */
int32_t stoolap_begin_with_isolation(
    StoolapDB* db,
    int32_t isolation,
    StoolapTx** out_tx
);

/** Execute within a transaction (no parameters). */
int32_t stoolap_tx_exec(StoolapTx* tx, const char* sql, int64_t* rows_affected);

/** Execute within a transaction (with parameters). */
int32_t stoolap_tx_exec_params(
    StoolapTx* tx,
    const char* sql,
    const StoolapValue* params,
    int32_t params_len,
    int64_t* rows_affected
);

/** Query within a transaction (no parameters). */
int32_t stoolap_tx_query(StoolapTx* tx, const char* sql, StoolapRows** out_rows);

/**
 * Commit a transaction.
 * The tx handle is consumed (freed) regardless of success or failure.
 */
int32_t stoolap_tx_commit(StoolapTx* tx);

/**
 * Rollback a transaction.
 * The tx handle is consumed (freed) regardless of success or failure.
 */
int32_t stoolap_tx_rollback(StoolapTx* tx);

/** Get the last error message for a transaction handle. */
const char* stoolap_tx_errmsg(const StoolapTx* tx);

/* =========================================================================
 * Direct reads
 *
 * Reads and writes go straight to a table: no SQL text, parse or plan.
 * They run in the handle's open transaction, or else in a transaction of
 * the statement's own that stoolap_end_statement() ends. Rows are named
 * by their stoolap row id, which every read returns. Columns are named by position, which is MariaDB's field
 * order (the plugin creates every table that way).
 * ========================================================================= */

/** A key bound's comparison on its last part (earlier parts are =). */
#define STOOLAP_OP_EQ           0
#define STOOLAP_OP_GT           1
#define STOOLAP_OP_GE           2
#define STOOLAP_OP_LT           3
#define STOOLAP_OP_LE           4

/** A key lookup: = on every bound part but the last, `op` there. */
#define STOOLAP_BOUND_KEY       0
/**
 * Only the first part, inclusive whatever `op` says (GE lower, LE upper;
 * an upper bound keeps the NULLs, which sort first).
 */
#define STOOLAP_BOUND_LEADING   1
/**
 * A range bound: a composite one compares in MariaDB's lexicographic key
 * order, (a, b) >= (1, 5) being a > 1 OR (a = 1 AND b >= 5).
 */
#define STOOLAP_BOUND_RANGE     2

/**
 * One bound of a read on a key: the key restored into a record
 * (key_restore), the parts it binds from the first, and how. Values
 * compare in MariaDB's index order: NULL before every value (so an upper
 * bound keeps the NULLs), a BIGINT UNSIGNED past INT64_MAX after every
 * stored one.
 */
typedef struct StoolapKeyBound {
    const uint8_t* record;
    int32_t nparts;
    int32_t op;
    /** Parts left out for the caller's own comparison (bit i) */
    uint32_t skip;
    /**
     * Skipped equality parts in a PAD SPACE binary collation (bit i):
     * looked up as rtrim(v) followed by each count of spaces the column
     * fits, which the caller's comparison still filters
     */
    uint32_t pad;
    int32_t mode;
} StoolapKeyBound;

/**
 * Where a read's rows come out. Keep one and read into it again and
 * again. Errors of its reads go to stoolap_cursor_errmsg().
 */
typedef struct StoolapCursor StoolapCursor;

StoolapCursor* stoolap_cursor_new(void);

/** StoolapField kinds: handed to the store callback */
#define STOOLAP_FIELD_STORE     0
/** A little-endian integer of `length` bytes (1, 2, 3, 4 or 8) */
#define STOOLAP_FIELD_INT       1
/** An 8-byte DOUBLE with no fixed decimals */
#define STOOLAP_FIELD_DOUBLE    2
/** A 4-byte FLOAT with no fixed decimals */
#define STOOLAP_FIELD_FLOAT     3
/**
 * A utf8mb4 VARCHAR, not COMPRESSED: a `length`-byte (1 or 2) length
 * prefix, then the bytes; a value of at most `max_chars` bytes always fits
 */
#define STOOLAP_FIELD_VARCHAR   4
/**
 * A CHAR, handed to the store callback as STORE is; its padding is no
 * part of its value (a write's check compares it without it). `length`
 * is 0 for text stored as it is, else the bytes of the character set's
 * space, given big-endian in `max_chars`, for text stored as hex.
 */
#define STOOLAP_FIELD_CHAR      5

/**
 * One field a read fills: the stoolap column it reads and where it sits
 * in the record. A value its kind does not take as it is (and NULL in a
 * NOT NULL field) goes to the store callback instead.
 */
typedef struct StoolapField {
    int32_t column;
    int32_t kind;
    uint32_t offset;
    /** The NULL bit's byte and mask; mask 0 when NOT NULL */
    uint32_t null_offset;
    uint32_t null_mask;
    uint32_t length;
    uint32_t max_chars;
    uint32_t is_unsigned;
    /** The field's index, passed to the store and encode callbacks */
    int32_t field;
    /** The bytes it takes in the record */
    uint32_t pack_length;
} StoolapField;

/**
 * Encodes field `field` of `record` into `out`, for the kinds StoolapField
 * does not read itself; returns 0, else the write fails. Text `out` points
 * to must stay valid until the next call.
 */
typedef int32_t (*StoolapEncodeFn)(void* ctx, int32_t field,
                                   const uint8_t* record, StoolapValue* out);

/**
 * Stores one value, `cell` (one cell of the packed format below), in
 * field `field`; returns 0, else the read fails.
 */
typedef int32_t (*StoolapStoreFn)(void* ctx, int32_t field,
                                  const uint8_t* cell, size_t len);

/** stoolap_read() flags: order descending */
#define STOOLAP_READ_DESC       1
/**
 * stoolap_read() flags: the statement may move rows to new row ids (it
 * sets the primary key), so the read takes its rows when it opens instead
 * of a page at a time, where a moved row would come back
 */
#define STOOLAP_READ_FIXED      2

/**
 * Open a read of `table` into `cursor`, ending the one it had: the rows
 * within every bound on the key `keys` describes (no bound: every row),
 * each filling `fields`, ordered by `order` (NULL first, all descending
 * with STOOLAP_READ_DESC in `flags`), or in no set order when `norder` is
 * 0. `encode(ctx, ...)` gives the bound values the key's fields do not
 * read themselves. STOOLAP_DONE when no row can be within the bounds (no
 * read opens). `table_columns` is the column count the plugin knows (the
 * table's own and its collation key columns); a table that differs is
 * refused.
 */
int32_t stoolap_read(
    StoolapDB* db,
    const char* table,
    int32_t table_columns,
    const StoolapField* fields, int32_t nfields,
    const StoolapField* keys, int32_t nkeys,
    const StoolapKeyBound* bounds, int32_t nbounds,
    const int32_t* order, int32_t norder,
    int32_t flags,
    StoolapEncodeFn encode, void* ctx,
    StoolapCursor* cursor
);

/** As stoolap_read(), for the one row `row_id` (no row if not visible). */
int32_t stoolap_read_row(
    StoolapDB* db,
    const char* table,
    int32_t table_columns,
    const StoolapField* fields, int32_t nfields,
    int64_t row_id,
    StoolapCursor* cursor
);

/**
 * Write the read's next row into `record`, its id into `row_id`, and 1
 * into `last` when no row follows it (the read has ended then). Returns
 * STOOLAP_ROW, STOOLAP_DONE at the end, or STOOLAP_ERROR; both end the
 * read. A value goes to `store(ctx, ...)` as StoolapField says.
 */
int32_t stoolap_cursor_next(
    StoolapCursor* cursor,
    uint8_t* record,
    StoolapStoreFn store,
    void* ctx,
    int64_t* row_id,
    int32_t* last
);

/**
 * Take every row of `rows` into `cursor`, ending the read it had, so that
 * a query's runtime error shows here rather than halfway through its rows.
 * stoolap_cursor_set_fields() then says what the rows fill, and
 * stoolap_cursor_next() gives them (row id 0). `rows` is left empty.
 */
int32_t stoolap_rows_into_cursor(StoolapRows* rows, StoolapCursor* cursor);

/**
 * Set the fields the taken rows fill, each naming a column of the result;
 * refused when one is past its columns.
 */
int32_t stoolap_cursor_set_fields(StoolapCursor* cursor,
                                  const StoolapField* fields, int32_t nfields);

const char* stoolap_cursor_errmsg(const StoolapCursor* cursor);

/** End the cursor's read, if one is open; the cursor stays. */
void stoolap_cursor_end(StoolapCursor* cursor);

/**
 * End the statement's direct access, and the transaction it opened
 * outside an open one: committed when `commit`, else rolled back. Call at
 * the end of every statement, with its cursors' reads ended. A failed commit
 * rolls back and reports to stoolap_errmsg(db).
 */
int32_t stoolap_end_statement(StoolapDB* db, int32_t commit);

/**
 * 1 when a foreign key names `table` (as child or parent), so its writes
 * go through SQL, where stoolap checks the keys; 0 when they go direct;
 * STOOLAP_ERROR.
 */
int32_t stoolap_writes_through_sql(StoolapDB* db, const char* table);

/**
 * 1 when a foreign key (the table's own included) names `table` as parent,
 * 0 when none does; STOOLAP_ERROR.
 */
int32_t stoolap_is_referenced(StoolapDB* db, const char* table);

/**
 * Check every row of `table` against its foreign keys (the rows a copying
 * ALTER wrote `unchecked`): STOOLAP_OK, or the first violation.
 */
int32_t stoolap_check_foreign_keys(StoolapDB* db, const char* table);

/**
 * Insert the row in `record`: `fields` describe every column, in order;
 * `encode(ctx, ...)` gives the values they do not read themselves.
 * `unchecked` writes it direct whatever foreign keys name the table.
 */
int32_t stoolap_insert_record(
    StoolapDB* db,
    const char* table,
    int32_t table_columns,
    const StoolapField* fields, int32_t nfields,
    const uint8_t* record,
    StoolapEncodeFn encode, void* ctx,
    int32_t unchecked,
    int64_t* rows_affected
);

/** Rows a bulk insert holds until stoolap_insert_buffer() writes them. */
typedef struct StoolapRowBuffer StoolapRowBuffer;

StoolapRowBuffer* stoolap_row_buffer_new(void);
void stoolap_row_buffer_free(StoolapRowBuffer* buffer);

/**
 * Add the row in `record`, read as stoolap_insert_record() reads it.
 * Returns the buffer's row count, or -1 (stoolap_row_buffer_errmsg()).
 */
int64_t stoolap_row_buffer_add(
    StoolapRowBuffer* buffer,
    const StoolapField* fields, int32_t nfields,
    const uint8_t* record,
    StoolapEncodeFn encode, void* ctx
);

/** Drop the rows the buffer holds, and their memory. */
void stoolap_row_buffer_clear(StoolapRowBuffer* buffer);

const char* stoolap_row_buffer_errmsg(const StoolapRowBuffer* buffer);

/**
 * Insert the rows `buffer` holds, which it gives up: it is empty after,
 * whatever the outcome. Through SQL, rows outside an open transaction go
 * in one of their own. `unchecked` as stoolap_insert_record().
 */
int32_t stoolap_insert_buffer(
    StoolapDB* db,
    const char* table,
    int32_t table_columns,
    StoolapRowBuffer* buffer,
    int32_t unchecked,
    int64_t* rows_affected
);

/**
 * Set the row from `new_record` where `fields` (the columns the statement
 * writes) differ from `old_record`: row `row_id` when `has_row_id`, else
 * the row the primary key `keys` holds in `old_record` names. 1 affected
 * when nothing differs, 0 when the row is gone.
 */
int32_t stoolap_update_record(
    StoolapDB* db,
    const char* table,
    int32_t table_columns,
    int64_t row_id, int32_t has_row_id,
    const StoolapField* fields, int32_t nfields,
    const StoolapField* keys, int32_t nkeys,
    const StoolapField* read, int32_t nread,
    const uint8_t* old_record,
    const uint8_t* new_record,
    StoolapEncodeFn encode, void* ctx,
    int64_t* rows_affected
);

/**
 * Delete row `row_id` when `has_row_id`, else the row the primary key
 * `keys` holds in `record` names; 0 affected when it is gone.
 *
 * For both writes, `read` names the columns the statement read: the row
 * must still hold the values `old_record` / `record` has for them, or the
 * write fails as a write conflict (another transaction changed the row
 * since MariaDB read it, and its new values would be written over).
 */
int32_t stoolap_delete_record(
    StoolapDB* db,
    const char* table,
    int64_t row_id, int32_t has_row_id,
    const StoolapField* keys, int32_t nkeys,
    const StoolapField* read, int32_t nread,
    const uint8_t* record,
    StoolapEncodeFn encode, void* ctx,
    int64_t* rows_affected
);

void stoolap_cursor_close(StoolapCursor* cursor);

/* =========================================================================
 * Result set iteration
 * ========================================================================= */

/**
 * Advance to the next row.
 * @return STOOLAP_ROW if a row is available.
 *         STOOLAP_DONE when exhausted.
 *         STOOLAP_ERROR on error.
 */
int32_t stoolap_rows_next(StoolapRows* rows);

/** Get an integer value. Returns 0 if NULL or not convertible. */
int64_t stoolap_rows_column_int64(const StoolapRows* rows, int32_t index);

/**
 * Get a text value from the current row.
 * @param out_len  If non-NULL, receives the full byte length (excluding the
 *                 trailing NUL terminator). This may exceed strlen() if the
 *                 value contains embedded NUL bytes. Use out_len to read the
 *                 complete data in that case.
 * @return Pointer valid until the next stoolap_rows_next() call.
 *         Returns NULL if the column is NULL. Must NOT be freed.
 */
const char* stoolap_rows_column_text(StoolapRows* rows, int32_t index, int64_t* out_len);

/** Check if the current row's column is NULL. Returns 1 if NULL, 0 otherwise. */
int32_t stoolap_rows_column_is_null(const StoolapRows* rows, int32_t index);

/**
 * Close the result set and free resources.
 * Safe to call with NULL (no-op).
 * Must be called even after STOOLAP_DONE.
 */
void stoolap_rows_close(StoolapRows* rows);

/* =========================================================================
 * Bulk fetch
 * ========================================================================= */

/* =========================================================================
 * Tables
 * ========================================================================= */

/** The stoolap type a column is stored in. */
#define STOOLAP_COL_INTEGER     1
#define STOOLAP_COL_FLOAT       2
#define STOOLAP_COL_TEXT        3

typedef struct StoolapColumnDef {
    const char* name;
    int32_t kind;           /**< STOOLAP_COL_* */
    int32_t nullable;
    int32_t auto_increment;
} StoolapColumnDef;

#define STOOLAP_KEYDEF_PRIMARY  0
#define STOOLAP_KEYDEF_UNIQUE   1
/** A non-unique key: a stoolap index over its parts. */
#define STOOLAP_KEYDEF_INDEX    2

typedef struct StoolapKeyDef {
    const char* name;
    const int32_t* columns; /**< the key's columns, by position */
    int32_t ncolumns;
    int32_t kind;           /**< STOOLAP_KEYDEF_* */
} StoolapKeyDef;

/** The table was created, but some indexes were not (stoolap_errmsg). */
#define STOOLAP_INDEX_FAILED 3

/** A FOREIGN KEY's ON DELETE / ON UPDATE action: MariaDB's enum_fk_option. */
#define STOOLAP_FK_UNDEF        0
#define STOOLAP_FK_RESTRICT     1
#define STOOLAP_FK_NO_ACTION    2
#define STOOLAP_FK_CASCADE      3
#define STOOLAP_FK_SET_NULL     4
#define STOOLAP_FK_SET_DEFAULT  5

/** A single-column FOREIGN KEY, as MariaDB's parser read it. */
typedef struct StoolapForeignKeyDef {
    const char* column;
    /** The parent table, by stoolap's name */
    const char* parent;
    /** NULL: the parent's primary key */
    const char* parent_column;
    int32_t on_delete;
    int32_t on_update;
} StoolapForeignKeyDef;

/** A column an ALTER renames. */
typedef struct StoolapColumnRename {
    const char* from;
    const char* to;
} StoolapColumnRename;

/**
 * Create `table` (stoolap's name) from `columns` and `keys` (in MariaDB's
 * key order), with its `foreign_keys` and secondary indexes, and, when
 * `keep_from` is not NULL, the foreign keys of that table, which a copying
 * ALTER rebuilds into this one (`renames`: the columns it renames;
 * `dropped`: the columns it drops, refused when a kept key uses one). A
 * single-column PRIMARY KEY is inline; a composite one and each UNIQUE
 * key are table-level constraints, numbered in that order (the ordinals
 * stoolap_error_class gives). A self-referencing foreign key is refused.
 */
int32_t stoolap_create_table(
    StoolapDB* db,
    const char* table,
    const StoolapColumnDef* columns, int32_t ncolumns,
    const StoolapKeyDef* keys, int32_t nkeys,
    const StoolapForeignKeyDef* foreign_keys, int32_t nforeign_keys,
    const char* keep_from,
    const StoolapColumnRename* renames, int32_t nrenames,
    const char* const* dropped, int32_t ndropped,
    int32_t if_not_exists
);

/** The most bytes a stoolap table name takes (it names a file). */
#define STOOLAP_MAX_TABLE_NAME 255

/**
 * Write `name` (`len` bytes), the plugin's escaped (db, table) name, as
 * stoolap can hold it to `out` (STOOLAP_MAX_TABLE_NAME bytes): past the
 * limit, its first characters, `_2` and a hash of the whole. Returns the
 * length written, 0 when `name` fits as it is.
 */
size_t stoolap_fit_table_name(const char* name, size_t len, char* out);

/**
 * Drop `table` if it is there. A parent other tables' foreign keys name
 * is refused (STOOLAP_ERR_ROW_IS_REFERENCED) when `check_fks`, except for
 * children among `dropping` (DROP DATABASE: the database's tables, by
 * stoolap's names), which are dropped first.
 */
int32_t stoolap_drop_table(StoolapDB* db, const char* table, int32_t check_fks,
                           const char* const* dropping, int32_t ndropping);

/**
 * Rename table `from` to `to`. `swap` when a copying ALTER renames its table
 * away to swap a copy in: refused when another table's FOREIGN KEY names it,
 * since the rename would take the key to the old table (stoolap#987).
 */
int32_t stoolap_rename_table(StoolapDB* db, const char* from, const char* to,
                             int32_t swap);

/** stoolap_alter_column() changes */
#define STOOLAP_ALTER_ADD_COLUMN    1
#define STOOLAP_ALTER_DROP_COLUMN   2
#define STOOLAP_ALTER_RENAME_COLUMN 3

/**
 * Change a column of `table` in place, as `op` says: add `name` at the end
 * (`kind` STOOLAP_COL_*, `nullable`), the rows there holding `default`;
 * drop it (refused when a FOREIGN KEY of the table uses it); or rename it
 * to `new_name` (a name in other letter case alone changes nothing).
 */
int32_t stoolap_alter_column(StoolapDB* db, const char* table, int32_t op,
                             const char* name, const char* new_name,
                             int32_t kind, int32_t nullable,
                             const StoolapValue* default_value);

/**
 * The version of `table`'s definition in stoolap (its columns and
 * indexes, hashed), which an in-place ALTER changes; 0 when it cannot be
 * read.
 */
uint64_t stoolap_table_version(StoolapDB* db, const char* table);

/**
 * AUTO_INCREMENT of a table whose INTEGER PRIMARY KEY it is, by stoolap's
 * id counter (the last id handed out: generated, explicit or rolled back;
 * a delete does not lower it, a restart keeps it).
 *
 * stoolap_auto_increment_set raises the counter so the next id is `next`
 * (CREATE TABLE ... AUTO_INCREMENT = n, a copying ALTER's new table).
 * stoolap_auto_increment_next gives that next id.
 * stoolap_auto_increment_reserve takes `count` ids, `step` apart and each
 * `offset` modulo `step`, past every id handed out; the first in *first.
 * stoolap_auto_increment_note raises the counter to `id`, the explicit id
 * of a row the plugin buffers, which stoolap takes only at the flush.
 */
int32_t stoolap_auto_increment_set(StoolapDB* db, const char* table,
                                   uint64_t next);
int32_t stoolap_auto_increment_next(StoolapDB* db, const char* table,
                                    uint64_t* next);
int32_t stoolap_auto_increment_reserve(StoolapDB* db, const char* table,
                                       uint64_t count, uint64_t step,
                                       uint64_t offset, uint64_t* first);
int32_t stoolap_auto_increment_note(StoolapDB* db, const char* table,
                                    uint64_t id);

/**
 * Create the index of `table`'s non-unique key `key` over `columns`
 * (`create`), unless an index over them is there, which serves the key
 * too; or drop the non-unique one over them, but a FOREIGN KEY's (`key`
 * unused). Stoolap keeps one index per column list, shared by the keys
 * over it: drop it for the last of them.
 */
int32_t stoolap_alter_index(StoolapDB* db, const char* table, const char* key,
                            int32_t create, const char* const* columns,
                            int32_t ncolumns);

/* =========================================================================
 * Error classes
 * ========================================================================= */

/** A wording no class covers (stoolap drift). */
#define STOOLAP_ERR_UNKNOWN             0
/** Known, with no MariaDB counterpart: report the text. */
#define STOOLAP_ERR_GENERIC             1
#define STOOLAP_ERR_DUP_KEY             2
#define STOOLAP_ERR_NO_REFERENCED_ROW   3
#define STOOLAP_ERR_ROW_IS_REFERENCED   4
/** Retry the statement: a write conflict, a lock, a schema change. */
#define STOOLAP_ERR_DEADLOCK            5
#define STOOLAP_ERR_TABLE_EXISTS        6
#define STOOLAP_ERR_NO_SUCH_TABLE       7
#define STOOLAP_ERR_UNSUPPORTED         8
/** An integer expression past BIGINT's range. */
#define STOOLAP_ERR_OUT_OF_RANGE        9

/** The key a duplicate names: none, or the PRIMARY KEY. */
#define STOOLAP_KEY_NONE               -1
#define STOOLAP_KEY_PRIMARY            -2

/**
 * The STOOLAP_ERR_* class of the error message `msg`, as any handle's
 * errmsg gives it; for a duplicate, `key` gets the key it names: a
 * STOOLAP_KEY_* or the ordinal of the table-level unique constraint in
 * CREATE TABLE order. An error recorded with its stoolap value is
 * classed by the value, anything else by its wording. `key` may be NULL.
 */
int32_t stoolap_error_class(const char* msg, int32_t* key);

/**
 * The packed format of stoolap_query_params_all() and of the cells a
 * StoolapStoreFn takes (one tag and payload).
 *
 * Format:
 *   [column_count: u32 LE]
 *   [for each column: name_len:u16 LE, name_bytes:u8[name_len]]
 *   [row_count: u32 LE]
 *   [for each row, for each column:
 *     type_tag: u8
 *     payload (varies):
 *       NULL(0):      (empty)
 *       INTEGER(1):   i64 LE (8 bytes)
 *       FLOAT(2):     f64 LE (8 bytes)
 *       TEXT(3):      len:u32 LE + bytes
 *       BOOLEAN(4):   u8 (0 or 1)
 *       TIMESTAMP(5): i64 LE (8 bytes, nanos since epoch)
 *       JSON(6):      len:u32 LE + bytes
 *       BLOB(7):      len:u32 LE + bytes (packed f32 for vectors)
 *   ]
 */

/**
 * Run a query with positional parameters and pack every row, in the
 * packed format above, in one call and without a rows handle.
 *
 * Runs inside tx when tx is not NULL; a tx begun on another database than
 * db (a stoolap_clone() of it is the same one) is an error. The buffer
 * belongs to db: it stays valid until the next call on db or
 * stoolap_close(), and must NOT be freed. On error, read
 * stoolap_errmsg(db).
 */
int32_t stoolap_query_params_all(
    StoolapDB* db,
    StoolapTx* tx,
    const char* sql,
    const StoolapValue* params,
    int32_t params_len,
    const uint8_t** out_buf,
    int64_t* out_len
);

/* =========================================================================
 * Memory management
 * ========================================================================= */

#ifdef __cplusplus
}
#endif

#endif /* STOOLAP_MARIADB_H */
