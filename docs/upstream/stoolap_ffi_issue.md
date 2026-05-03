# Draft Upstream Stoolap FFI Issue

Title: FFI APIs to simplify MariaDB plugin correctness workarounds

The MariaDB plugin currently carries several compatibility workarounds that can
be deleted if Stoolap exposes a few small FFI surfaces. Proposed sequence:

**Landed (in stoolap `main`):** #1 typed errors, #2 table count, #5 read-only
DSN, #6 savepoints. Plugin-side consumption tracked in the
"Plugin-side consumption sequence" section at the bottom.

**Still pending:** #3 MVCC-safe bare COUNT, #4 stable rowid, #7 bytes type,
#8 wider DATETIME, #9 composite keys, #10 AUTO_INCREMENT seed.

## 1. Typed Error Codes And Details ✅ Landed

Shipped: 28 `STOOLAP_ERR_*` codes (see `include/stoolap.h:563-590`),
`StoolapErrorDetails` (code / message / table / column / constraint / detail),
and per-handle `stoolap_{db,tx,stmt,rows}_errcode + _errdetails` accessors.

Plugin work: replace `map_stoolap_error(const char*)` (substring-grep table)
with `map_stoolap_error_code(int32_t)` switch; replace `guess_errkey`'s
"on column NAME" prose parser with `details.constraint` (the actual stoolap
index name); collapse `report_stoolap_error` into a thin wrapper that calls
`stoolap_*_errdetails` and dispatches.

Original signatures proposed below, kept for the issue history:

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

## 2. MVCC-Safe Table Count ✅ Landed

Shipped exactly as proposed:

```c
int32_t stoolap_table_count   (StoolapDB* db, const char* table, uint64_t* out_count);
int32_t stoolap_tx_table_count(StoolapTx* tx, const char* table, uint64_t* out_count);
```

Bonus from stoolap: Database::table_count uses the SegmentedTable fast
path (O(1) atomic loads), so it's safe in the hot loop. Transaction::
table_count is snapshot-correct AND accounts for uncommitted local
INSERTs/DELETEs in the same tx -- exactly the MVCC contract the plugin's
three-layer records cache spent ~200 lines to approximate.

Plugin work: replace `cached_records()`'s live `SELECT COUNT(*) FROM t WHERE
1 = 1` with the matching count call. The `WHERE 1 = 1` workaround for the
metadata snapshot hazard is gone too. The records cache itself can stay (still
amortizes per-statement) but the cache-miss path becomes O(1) instead of a
full scan.

Payoff: collapses the plugin's records-cache workaround to one safe call.

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

## 5. Read-Only DSN ✅ Landed

Shipped: a separate `StoolapRoDB` type with no write entry points
(no `stoolap_ro_exec`, no `stoolap_ro_begin`), so write SQL routed
through it is a compile-time link error rather than a runtime one.

```c
typedef struct StoolapRoDB StoolapRoDB;
int32_t stoolap_open_read_only(const char* dsn, StoolapRoDB** out_db);
void    stoolap_ro_close       (StoolapRoDB* db);
int32_t stoolap_ro_query       (StoolapRoDB* db, const char* sql, StoolapRows** out_rows);
int32_t stoolap_ro_query_params(StoolapRoDB* db, const char* sql,
                                 const StoolapValue* params, int32_t n,
                                 StoolapRows** out_rows);
int32_t stoolap_ro_table_count (StoolapRoDB* db, const char* table, uint64_t* out_count);
int32_t stoolap_ro_table_exists(StoolapRoDB* db, const char* name);
int32_t stoolap_ro_refresh     (StoolapRoDB* db);   /* manual snapshot advance */
void    stoolap_ro_set_auto_refresh(StoolapRoDB* db, int32_t enabled);
/* Plus stoolap_ro_errmsg / _errcode / _errdetails / _dsn. */
```

Bonus: `Database::open` now REJECTS `?read_only=true` / `?readonly=true` /
`?mode=ro` with `STOOLAP_ERR_INVALID_ARGUMENT` and a message pointing the
caller to `stoolap_open_read_only`. `stoolap_open_read_only` accepts those
flags as redundant no-ops, so existing driver DSN strings continue working
unchanged.

Plugin work: parse `?read_only=...` (and `?readonly=...`, `?mode=ro`) in
`Engine::open`, route to `stoolap_open_read_only` when set. The plugin's
internal Engine type becomes a tagged union of (rw `StoolapDB*`, ro
`StoolapRoDB*`); every FFI call site dispatches on the tag. Write SQL
(INSERT/UPDATE/DELETE/DDL) returns `HA_ERR_TABLE_READONLY` up front when
the engine is RO. Auto-refresh stays on (the safe default); we can add a
session var later for the snapshot-stable case.

This is the largest of the four landed-API consumption PRs because the
type split touches every call site. Worth landing last in the sequence.

Original signature options proposed below, kept for the issue history:

```c
/* Option A: query-param parsing inside stoolap_open. The plugin
 * already accepts the suffix from users via stoolap_dsn, so this
 * needs zero plugin-side change. */
int32_t stoolap_open(const char* dsn, StoolapDB** out_db);
/* dsn = "file:///var/lib/stoolap?read_only=1" */

/* Option B: explicit function. Cleaner if stoolap doesn't want a
 * DSN parser surface. The plugin would parse the query string
 * itself and route to the right call. */
int32_t stoolap_open_read_only(const char* dsn, StoolapDB** out_db);

/* Option C: open-with-flags. Future-proof for other modifiers
 * (sync_mode, compression, etc) without touching the DSN parser. */
int32_t stoolap_open_with_flags(const char* dsn, uint32_t flags,
                                StoolapDB** out_db);
#define STOOLAP_OPEN_READ_ONLY 0x1
```

Plugin-side cost: ~5 lines in `Engine::open` to parse the `read_only`
query param and route to whichever API stoolap exposes. The
README's read-only row gets to drop its "(pending FFI)" caveat.

## 6. Savepoints ✅ Landed

Shipped with name-based addressing (matches MariaDB's per-savepoint name
chunk directly):

```c
int32_t stoolap_tx_savepoint            (StoolapTx* tx, const char* name, int32_t name_len);
int32_t stoolap_tx_release_savepoint    (StoolapTx* tx, const char* name, int32_t name_len);
int32_t stoolap_tx_rollback_to_savepoint(StoolapTx* tx, const char* name, int32_t name_len);
```

Bonus: `name_len = -1` treats `name` as a NUL-terminated C string. Use the
explicit length when interoperating with MariaDB's handlerton savepoint
chunk (which stores `char* name` + `uint length` and may not be
NUL-terminated).

Plugin work: register three handlerton callbacks (`savepoint_set`,
`savepoint_release`, `savepoint_rollback`); each receives a `void* sv`
chunk that's the engine's per-savepoint scratch. We use it to stash the
savepoint name pulled from MariaDB's `st_savepoint::{name, length}` and
pass through to stoolap. `savepoint_offset` becomes the size of our
chunk (just enough for the name pointer + length, or copy the bytes
inline). Total ~40 lines.

Original signatures proposed below, kept for the issue history:


`savepoint_set` / `savepoint_release` / `savepoint_rollback`. Each
gets a per-savepoint scratch buffer (sized via `savepoint_offset`)
that the engine fills with whatever it needs to identify the
savepoint later. The plugin can wire all three the moment stoolap
exposes the underlying ops; today `SAVEPOINT name` returns
`ERROR 1305: SAVEPOINT does not exist` because we don't register
the callbacks at all.

```c
/* Opaque savepoint identifier. uint64_t is enough -- the plugin
 * only ever stashes it in MariaDB's per-savepoint chunk and hands
 * it back on release/rollback. If stoolap prefers a pointer
 * (StoolapSavepoint*), the plugin can store that just as easily. */
typedef uint64_t StoolapSavepointId;

/* Take a savepoint inside an open tx. Returns an id the caller
 * stashes for later release/rollback. */
int32_t stoolap_tx_savepoint_create(StoolapTx* tx,
                                     StoolapSavepointId* out_id);

/* Release a savepoint: forget it; data unchanged. SQL standard
 * says subsequent ROLLBACK TO this savepoint is an error. */
int32_t stoolap_tx_savepoint_release(StoolapTx* tx,
                                      StoolapSavepointId id);

/* Roll back everything done in this tx since the savepoint was
 * taken. The savepoint itself remains valid (per SQL standard);
 * subsequent writes can re-target the same savepoint. */
int32_t stoolap_tx_savepoint_rollback(StoolapTx* tx,
                                       StoolapSavepointId id);
```

If named savepoints with replace-by-name semantics are easier to
expose stoolap-side, that also works -- MariaDB hands the engine
the savepoint name as part of the chunk, so the plugin can pass
either an id or a name through:

```c
int32_t stoolap_tx_savepoint_create_named(StoolapTx* tx,
                                           const char* name);
int32_t stoolap_tx_savepoint_release_named(StoolapTx* tx,
                                            const char* name);
int32_t stoolap_tx_savepoint_rollback_named(StoolapTx* tx,
                                             const char* name);
```

Plugin-side cost: ~40 lines for three handlerton callbacks plus
the `savepoint_offset` constant in the handlerton init. The
README's "SAVEPOINT name | No (not in Stoolap C ABI yet)" row
flips to Yes; the open-question entry goes away.

Payoff: matches InnoDB's transactional surface so apps using
SAVEPOINT for partial-rollback / nested-tx patterns work
unchanged. Frequently asked for by ORMs.

## 7. Arbitrary-Bytes Type (BLOB / VARBINARY round-trip)

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

## 8. Wider DATETIME Range (or a separate type)

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

## 9. Composite PRIMARY KEY / UNIQUE / FOREIGN KEY

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

## 10. AUTO_INCREMENT Seed

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

## Plugin-side consumption sequence (for the four landed asks)

Each consumes one upstream API as a separate plugin PR per the
hardening roadmap's PR 6 plan ("file four plugin deletion PRs in
dependency order, one workaround removed per PR"). Order picked by
risk-ascending so the early PRs build confidence in the cross-repo
cadence:

**PR-A: Typed errors (#1)** — smallest plugin diff, biggest visibility.
- Wire `stoolap_*_errcode` / `stoolap_*_errdetails` calls.
- Replace `map_stoolap_error(const char*)` substring table with a
  switch on the int32_t code.
- Replace `guess_errkey`'s `"on column ..."` regex parser with
  `details.constraint` (the actual stoolap index name).
- Keep the prose-grep path as a fallback for any unmapped code so
  the PR can land before every site is audited.
- Drop the `Stoolap_unmapped_errors` drift counter once we're sure
  every code routes through the typed path; until then it still
  earns its keep.
- Rough size: ~120 lines changed in `ha_stoolap.cc::map_stoolap_error`
  + report_stoolap_error + guess_errkey, ~30 lines deleted from the
  prose-grep table.

**PR-B: Table count (#2)** — collapses the records-cache subsystem.
- In `cached_records()`, replace the live `SELECT COUNT(*) FROM t
  WHERE 1 = 1` with `stoolap_tx_table_count` (in-tx) or
  `stoolap_table_count` (autocommit). Both are O(1) atomic loads.
- Drop the `WHERE 1 = 1` MVCC-fast-path workaround entirely.
- The three-layer cache (handler-local / tx-local / global) can stay
  -- it amortizes per-statement -- but the cache-miss path becomes
  cheap, so `Stoolap_records_live_counts` stops being a "cold
  caches" warning and becomes pure telemetry.
- Updates `docs/architecture/records_cache_architecture.md`
  ... but wait, that doc was deleted in the docs cleanup. Update
  the in-code comment block in `ha_stoolap.cc::cached_records()`
  instead.
- Rough size: ~30 lines in `cached_records()`, ~15 lines deleted
  from the live-count fallback.

**PR-C: Savepoints (#6)** — additive, doesn't disturb existing paths.
- Add `savepoint_set` / `savepoint_release` / `savepoint_rollback`
  handlerton callbacks.
- Set `handlerton::savepoint_offset` to whatever chunk size we need
  (probably 0 -- we can read the name from MariaDB's surrounding
  `st_savepoint` struct).
- Each callback pulls the savepoint name from the chunk and forwards
  to `stoolap_tx_{savepoint, release_savepoint, rollback_to_savepoint}`
  with `name_len` from MariaDB's `st_savepoint::length`.
- Add `case_19_savepoints.py` covering: SAVEPOINT inside BEGIN,
  ROLLBACK TO SAVEPOINT inside same tx, RELEASE SAVEPOINT, error on
  reference to undeclared name.
- Removes the "SAVEPOINT name | No (not in Stoolap C ABI yet)" row
  from the README transactions matrix and the matching open-question
  bullet.
- Rough size: ~50 lines added to `ha_stoolap.cc` (three callbacks +
  hton init), ~70 lines for the new test case.

**PR-D: Read-only DSN (#5)** — biggest refactor, land last.
- Parse `?read_only=...` in `Engine::open`.
- `Engine::db_` becomes a tagged union: `(rw StoolapDB*, ro StoolapRoDB*)`.
- Every FFI call site dispatches on the tag. The bridge layer
  (`exec_via`, `query_via`, `query_params_via`) gains an RO branch
  that calls `stoolap_ro_query` / `_query_params` and refuses
  exec/begin entirely.
- `register_trx` returns early when RO (no tx state to track).
- `cached_records()` uses `stoolap_ro_table_count` on RO handles.
- Write paths (`write_row`, `update_row`, `delete_row`,
  `direct_*_rows`) check the tag up front and return
  `HA_ERR_TABLE_READONLY` (errno 1036).
- DDL paths (`create`, `delete_table`, `rename_table`, etc.) refuse
  too -- with a matching ER_OPEN_AS_READONLY (errno 1036) or
  ER_READ_ONLY_MODE.
- Add `case_20_read_only.py` covering: read query works, write
  rejected with the right errno, DDL rejected, COUNT(*) routes
  through ro_table_count.
- Removes the "read-only mode (DSN with ?read_only=1) | Yes" row's
  caveat and updates the README install instructions.
- Rough size: ~250 lines (touches every FFI call site), ~100 lines
  for the test case.

## Sequencing recap

By plugin-side deletion payoff (largest first):

  1. Typed error codes -- collapses prose-grep error mapping.
  2. Stable rowid -- lifts no-PK UPDATE/DELETE refusal AND the
     BLOB rnd_pos hazard.
  3. MVCC-safe table count -- collapses the records-cache
     subsystem.
  4. Composite keys -- removes a whole gate in build_create_sql
     and unlocks legacy schemas.
  5. Bytes type -- removes the BLOB UTF-8 truncation footgun.
  6. Savepoints -- adds a frequently-requested transactional
     surface.
  7. MVCC-safe bare COUNT -- removes the WHERE 1=1 workaround.
  8. Wider DATETIME range -- removes a class of "won't insert"
     errors.
  9. Read-only DSN -- five-line plugin fix.
 10. AUTO_INCREMENT seed -- ten-line plugin fix.

The smaller asks (read-only, AI seed) are good warm-ups for
establishing the cross-repo cadence; bigger ones (rowid, typed
errors) deliver the most plugin-side simplification.
