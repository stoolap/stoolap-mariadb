# Draft Upstream Stoolap FFI Issue

Title: FFI APIs to simplify MariaDB plugin correctness workarounds

The MariaDB plugin currently carries several compatibility workarounds that can
be deleted if Stoolap exposes a few small FFI surfaces. Proposed sequence:

## 1. Typed Error Codes And Details

```c
typedef enum StoolapErrorCode {
    STOOLAP_ERR_OK = 0,
    STOOLAP_ERR_GENERIC = 1,
    STOOLAP_ERR_DUPLICATE_KEY = 2,
    STOOLAP_ERR_FOREIGN_KEY_NO_PARENT = 3,
    STOOLAP_ERR_FOREIGN_KEY_CHILD_EXISTS = 4,
    STOOLAP_ERR_LOCK_DEADLOCK = 5,
    STOOLAP_ERR_TABLE_EXISTS = 6,
    STOOLAP_ERR_TABLE_NOT_FOUND = 7,
    STOOLAP_ERR_UNSUPPORTED = 8
} StoolapErrorCode;

typedef struct StoolapErrorDetails {
    int32_t code;
    const char* message;
    const char* table;
    const char* column;
    const char* constraint;
} StoolapErrorDetails;

int32_t stoolap_errcode(StoolapDB* db);
int32_t stoolap_tx_errcode(StoolapTx* tx);
int32_t stoolap_stmt_errcode(StoolapStmt* stmt);
int32_t stoolap_errdetails(StoolapDB* db, StoolapErrorDetails* out);
int32_t stoolap_tx_errdetails(StoolapTx* tx, StoolapErrorDetails* out);
int32_t stoolap_stmt_errdetails(StoolapStmt* stmt, StoolapErrorDetails* out);
```

Payoff: replace prose-grep error mapping with a switch.

## 2. MVCC-Safe Table Count

```c
int32_t stoolap_table_count(StoolapDB* db, const char* table, uint64_t* out);
int32_t stoolap_tx_table_count(StoolapTx* tx, const char* table, uint64_t* out);
```

Payoff: collapse the plugin's records-cache workaround to one safe call.

## 3. MVCC-Safe Bare COUNT

```c
int32_t stoolap_set_safe_count_only(StoolapDB* db, int32_t enabled);
```

Alternative: make bare `COUNT(*)` use the same MVCC-visible path as ordinary
scans. Payoff: remove the plugin's `WHERE 1 = 1` count workaround.

## 4. Stable Row Identifier

```c
typedef struct StoolapRowId {
    uint64_t hi;
    uint64_t lo;
} StoolapRowId;

int32_t stoolap_rows_column_rowid(StoolapRows* rows, int32_t col,
                                  StoolapRowId* out);
int32_t stoolap_delete_rowid(StoolapDB* db, const char* table,
                             StoolapRowId rowid, int64_t* affected);
int32_t stoolap_tx_delete_rowid(StoolapTx* tx, const char* table,
                                StoolapRowId rowid, int64_t* affected);
int32_t stoolap_update_rowid(StoolapDB* db, const char* table,
                             StoolapRowId rowid,
                             const StoolapValue* values, int32_t value_count,
                             int64_t* affected);
int32_t stoolap_tx_update_rowid(StoolapTx* tx, const char* table,
                                StoolapRowId rowid,
                                const StoolapValue* values,
                                int32_t value_count, int64_t* affected);
```

Payoff: lift the no-PK row-pump UPDATE/DELETE refusal.
