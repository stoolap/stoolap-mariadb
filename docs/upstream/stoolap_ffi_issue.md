# Draft Upstream Stoolap FFI Issue

Title: FFI APIs to simplify MariaDB plugin correctness workarounds

The MariaDB plugin currently carries several compatibility workarounds that can
be deleted if Stoolap exposes a few small FFI surfaces. Proposed sequence:

**Landed upstream:** #1 typed errors, #2 table count, #6 savepoints (merged
to stoolap `main`); #5 read-only DSN (currently on stoolap's
`read-only-trait-split` branch -- the prior `main` PR was reverted).

**Consumed in plugin:** #1 (commit `9a11b66`), #2 (`5256cc5`), #6 (`26cf1db`).
#5 is intentionally NOT consumed -- see "Plugin-side consumption sequence".

**Still pending upstream:** #3 MVCC-safe bare COUNT, #4 stable rowid, #7 bytes
type, #8 wider DATETIME, #9 composite keys, #10 AUTO_INCREMENT seed.

**Open follow-up ask:** stoolap currently returns `STOOLAP_ERR_INTERNAL` (21)
for write-conflict messages of the shape `row N has uncommitted changes from
transaction M`. Should classify as `STOOLAP_ERR_TX_ABORTED` (15) or
`STOOLAP_ERR_DB_LOCKED` (18) so the plugin's prose-grep fallback can retire.
The plugin observes this gap via `Stoolap_typed_fallback_hits` (3 hits per
suite run) and logs each occurrence as `[Note] stoolap typed-error gap:
code=21 msg='...'`.

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

PR-A through PR-C consumed the corresponding upstream API as a separate
commit per the hardening roadmap's "one workaround removed per PR" plan,
ordered risk-ascending so the early PRs built confidence in the cross-
repo cadence. PR-D was reviewed and intentionally dropped after the
write-up below.

**PR-A: Typed errors (#1)** — landed `9a11b66`. ~500 lines.
- Wires `stoolap_*_errcode` / `stoolap_*_errdetails` via per-handle
  `fetch_*_error` helpers returning a `StoolapErrorView` (typed code +
  populated details).
- `map_stoolap_errcode(int32_t)` returns `{ha_err, known}` so the prose
  fallback only runs for codes stoolap explicitly flagged generic
  (`STOOLAP_ERR_GENERIC` / `STOOLAP_ERR_INTERNAL`) or unknown future
  codes (`!known`). Codes deliberately mapped to HA_ERR_GENERIC because
  no specific MariaDB code exists (NOT_NULL, CHECK, ...) skip prose.
- `errkey_from_view` prefers `details.constraint` (the index name
  stoolap hands us directly) over message-grep for UNIQUE collisions.
- `finish_view` promotes `STOOLAP_ERR_OK` -> `STOOLAP_ERR_GENERIC` so an
  empty fetch (NULL handle, FFI returned no live error after a known
  failure rc) cannot silently turn into a HA_ERR_OK return.
- `Stoolap_typed_fallback_hits` counter + `[Note] stoolap typed-error
  gap: code=N msg='...'` log surface the upstream typed-error gap.

**PR-B: Table count (#2)** — landed `5256cc5`. ~70 lines.
- `cached_records()`'s live `SELECT COUNT(*) FROM t WHERE 1 = 1` is
  now `stoolap_tx_table_count` (in-tx) or `stoolap_table_count`
  (autocommit) via a `count_via` helper. Cost dropped from full scan
  to O(1) atomic load.
- `info(HA_STATUS_VARIABLE)` deliberately still does NOT call
  `cached_records()` even at O(1) cost: it fires multiple times per
  statement during planning + records_in_range loops + post-statement,
  and the cache-only lookup avoids that multiplier on every connection.

**PR-C: Savepoints (#6)** — landed `26cf1db`. ~310 lines + test case.
- Three handlerton callbacks (`savepoint_set` / `savepoint_release` /
  `savepoint_rollback`) wired through `stoolap_tx_savepoint` /
  `stoolap_tx_release_savepoint` / `stoolap_tx_rollback_to_savepoint`.
- MariaDB does not pass the user's SAVEPOINT name to the engine, so
  the plugin synthesises `sp<id>` from a per-connection monotonic
  counter (`ThdContext::next_savepoint_id`) and stashes id+name in
  the engine-private chunk MariaDB allocates per SAVEPOINT (sized via
  `hton->savepoint_offset`). All three callbacks share the chunk.
- `ROLLBACK TO` invalidates the tx-local record-count cache (we track
  count deltas at tx granularity, not savepoint granularity).
- `case_19_savepoints.py` covers ROLLBACK TO undo, RELEASE persistence,
  nested savepoints, ROLLBACK TO unknown -> errno 1305, and reused
  savepoint names overriding the prior.

**PR-D: Read-only DSN (#5)** — intentionally dropped.
- Use case for the MariaDB plugin is narrow: MariaDB already exposes
  `read_only=1` / `super_read_only=1` server variables that block
  writes at the SQL layer; replication scales reads across separate
  MariaDB instances. The single remaining case (MariaDB as a read-only
  query layer over a stoolap file written by another process) is
  niche and not actively requested.
- Cost is high: ~350 lines of restructuring (`Engine::db_` tagged
  union, every read path forks, every write path needs a RO rejection,
  `info()` / `cached_records()` / error helpers all dual-pathed) plus
  ongoing maintenance (every future read-path change has to be
  considered for both modes) plus a parallel RO test surface.
- Stoolap's `Database::open` already rejects `?read_only=true` flags
  with `STOOLAP_ERR_INVALID_ARGUMENT` and a message pointing at
  `stoolap_open_read_only`, so accidental misconfiguration produces a
  clean error today.
- Status: documented as out-of-scope for the plugin; revisit only if
  a concrete user surfaces.

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
