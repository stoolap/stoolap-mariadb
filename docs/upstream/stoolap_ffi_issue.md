# Draft Upstream Stoolap FFI Issue

Title: FFI APIs to simplify MariaDB plugin correctness workarounds

The MariaDB plugin carries several compatibility workarounds that can be
deleted if Stoolap exposes a few small surfaces. The plugin targets stoolap
`main`'s C ABI (`include/stoolap.h`); errors are classified from the
`*_errmsg` text, savepoints run as SQL on the tx handle, and table counts
come from a one-call `SELECT COUNT(*)`.

## 1. Stable Row Identifier

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

## 2. Arbitrary-Bytes Type (BLOB / VARBINARY round-trip)

The plugin currently routes MariaDB's `BLOB` / `TINYBLOB` /
`MEDIUMBLOB` / `LONGBLOB` / `VARBINARY` family through Stoolap
`TEXT`, because Stoolap's existing `BLOB` type is reserved for
vector columns. `TEXT` validates UTF-8 on insert, so any binary
content with non-UTF-8 bytes (a JPEG header, `UNHEX('FF00FF00')`,
a hashed password) silently round-trips as `NULL`. ASCII-clean
text in BLOB columns happens to work; arbitrary bytes do not.

```c
/* New value type: arbitrary bytes, no validation. */
#define STOOLAP_TYPE_BYTES   <next-free-enum-value>

typedef struct StoolapBytes {
    const uint8_t* ptr;
    int64_t        len;
} StoolapBytes;

/* StoolapValue.v gains a `.bytes` member alongside .text/.integer/...
 *   union {
 *       int64_t        integer;
 *       double         floating;
 *       StoolapText    text;
 *       StoolapBytes   bytes;   // <-- new
 *       ...
 *   } v;
 */

/* CREATE TABLE accepts the type spelled `BYTES` (or `BINARY`):
 *   CREATE TABLE t (data BYTES);
 *   CREATE TABLE t (sig  BINARY(32));
 */
```

Plugin-side cost: ~30 lines in `extract_field` / `pkt_store_value`
to map the MariaDB BLOB family to `STOOLAP_TYPE_BYTES` instead of
TEXT, plus the handlerton CREATE TABLE emitter switching the type
string. The "BLOB / VARBINARY silently drop non-UTF-8 bytes" known
limitation goes away.

## 3. Wider DATETIME Range (or a separate type)

Stoolap stores `TIMESTAMP` columns as i64 nanoseconds since the
Unix epoch, capping the representable range at roughly 1678..2262.
The plugin returns `HA_ERR_UNSUPPORTED` when MariaDB hands us a
DATETIME outside that window (e.g. `'9999-12-31 23:59:59'`, which
is well within MariaDB's documented `1000..9999` range). Apps that
use sentinel "far-future" timestamps for soft-delete or
"never-expire" rows just can't run on Stoolap today.

Two shapes either fix it:

```c
/* Option A: keep i64 backing but switch unit to microseconds.
 *   i64 micros covers ~+/- 292,000 years from epoch -- comfortably
 *   past MariaDB's 1000..9999 limit. The FFI surface stays
 *   identical (still a single int64), only the semantic unit
 *   shifts; document the change in the release notes. */

/* Option B: add a separate DATETIME type whose backing storage is
 *   broad enough for MariaDB's range (e.g. struct of {year,
 *   month, day, hour, minute, second, microsecond}). Plugin would
 *   pick this for DATETIME columns and keep TIMESTAMP on the
 *   existing nanosecond backing for high-resolution use cases. */
```

Plugin-side cost: a few lines in `extract_field` to stop
returning `HA_ERR_UNSUPPORTED` on out-of-range DATETIMEs, plus
the CREATE TABLE emitter picking the right type. The "DATETIME
is bounded to roughly 1678..2262" known limitation goes away.

## 4. Composite PRIMARY KEY / UNIQUE / FOREIGN KEY

The plugin refuses composite constraints at CREATE TABLE because
Stoolap's parser+enforcer is single-column only. Multi-column
PRIMARY KEY / UNIQUE / FOREIGN KEY are common in legacy schemas
and ORM-generated tables; users hit this immediately on real
workloads.

What's needed on the Stoolap side (no new FFI surface, just
grammar+enforcer support):

```sql
CREATE TABLE t (
    a INT NOT NULL,
    b INT NOT NULL,
    c INT,
    PRIMARY KEY (a, b),
    UNIQUE KEY uq_bc (b, c),
    FOREIGN KEY (a, b) REFERENCES parent (a, b)
);
```

The MariaDB plugin already emits canonical CREATE TABLE text with
multi-column constraint clauses; today `build_create_sql` rejects
them up front to avoid the silent-drop trap of the previous
"accepted but not enforced" behaviour (P1, fixed in this repo).
Once Stoolap accepts and enforces them, the plugin's rejection
gate flips to a passthrough.

Plugin-side cost: ~20 lines deleted from `build_create_sql` /
`copy_one_fk_clause`. The "Composite PRIMARY KEY / UNIQUE / FK
refused at CREATE TABLE" known limitation goes away.

## 5. AUTO_INCREMENT Seed

`ALTER TABLE t AUTO_INCREMENT = N` is silently ignored today; the
plugin recomputes the next value from `MAX(col) + 1` per
connection because Stoolap has no way to seed the counter. Real
deployments use this to reserve id ranges across migrations or to
recover after a partial restore.

```c
/* Set the next value the auto-increment counter will hand out.
 * If the current MAX(col) > next_value, stoolap should still
 * never issue ids <= existing rows (clamp to MAX(col) + 1).
 *
 * Returns OK even when next_value <= MAX(col); the contract is
 * "no rows will be issued an id less than the larger of (existing
 * max + 1, next_value)". That matches MySQL/MariaDB semantics and
 * lets the plugin pass the user-requested seed through verbatim. */
int32_t stoolap_table_set_auto_increment(StoolapDB* db,
                                          const char* table,
                                          uint64_t next_value);
```

Plugin-side cost: ~10 lines wiring `ha_stoolap::extra(HA_EXTRA_*)`
or the `ALTER TABLE` reconcile path to forward the user-supplied
seed through. The "ALTER TABLE AUTO_INCREMENT = N is not
propagated" known limitation goes away.
