# stoolap-mariadb

[![CI](https://github.com/stoolap/stoolap-mariadb/actions/workflows/ci.yml/badge.svg)](https://github.com/stoolap/stoolap-mariadb/actions/workflows/ci.yml)

MariaDB 11.4 storage engine plugin (`ha_stoolap.so`) backed by
[Stoolap](https://github.com/stoolap/stoolap), an embedded analytical
SQL engine written in Rust. The plugin is C++ over a Rust crate
(`rust/`) that links Stoolap as a Rust crate; the crate is linked into
`ha_stoolap.so`, so the plugin is one self-contained file.

The plugin pushes whole SELECT queries, derived tables, UPDATE/DELETE
statements, and unit operations down to Stoolap's executor when it can,
and falls back to MariaDB's row-pump only for the cases Stoolap does
not yet reproduce semantically (case-insensitive collation, stored
procedures, user variables, ...). Stoolap's columnar storage and
vectorised aggregation give large speedups on analytical workloads
versus InnoDB while staying pluggable into the MariaDB ecosystem
(replication, drivers, BI tools).

## Status

Beta. Plugin maturity is declared `MariaDB_PLUGIN_MATURITY_ALPHA`
because the persistent storage path is still relatively young; in-memory
mode and the read pipeline are heavily tested. Verified end-to-end
against MariaDB 11.4.10 with 18 Python integration test files (531
assertions, all green on `memory://` and `file://` DSNs).

Subsystems that have settled recently:
- Whole-SELECT and derived-table pushdown (`select_handler` /
  `derived_handler` / unit pushdown), including derived projections
  whose outer reference is an `Item_ref`.
- Direct UPDATE/DELETE through the `pre_direct_*` hooks, with
  constraint errors mapped to the right MariaDB error number
  (`ER_DUP_ENTRY`, `ER_NO_REFERENCED_ROW_2`, etc).
- Per-session isolation: `register_trx` reads `@@tx_isolation` and
  opens a stoolap snapshot tx for `REPEATABLE READ`/`SERIALIZABLE`,
  read-committed otherwise. `START TRANSACTION WITH CONSISTENT
  SNAPSHOT` is hooked through the `start_consistent_snapshot`
  handlerton callback.
- `SAVEPOINT` / `RELEASE SAVEPOINT` / `ROLLBACK TO SAVEPOINT` run
  as SQL on the open stoolap transaction (`stoolap_tx_exec`). Names
  are synthesised per-connection because MariaDB does not pass the
  user's SAVEPOINT name to the engine.
- Error mapping: stoolap's error text (`*_errmsg`) is matched against
  a pattern table (`map_stoolap_error`) that picks the HA_ERR_*;
  wordings it doesn't cover bump `Stoolap_unmapped_errors`.
- MVCC-safe `cached_records()` via a one-call `SELECT COUNT(*)`
  (`stoolap_query_params_all`) on the session's tx or autocommit view.
- Composite `PRIMARY KEY` / `UNIQUE` / multi-column or self-FK
  refused at `CREATE TABLE` (Stoolap enforces single-column only;
  silently accepting them would let duplicates and FK-violating
  rows in).
- Multi-row `INSERT IGNORE` / `REPLACE` / `INSERT ... ON DUPLICATE
  KEY UPDATE` route per-row so MariaDB drives the recovery; the
  bulk batch path is opt-out for these shapes (it would otherwise
  abort the batch on the first dup).
- `info(HA_STATUS_VARIABLE)` no longer triggers a fresh `SELECT
  COUNT(*)` per call; the optimizer-estimate path uses the cached
  value while the user's `COUNT(*)` shortcut still refreshes on
  demand.
- A correctness guard for case-insensitive collations vs. Stoolap's
  byte-wise compare semantics.

## Architecture

Subsystem design rationale lives in block comments next to the code it
explains; the source-layout table below names the entry-point files. The
records-cache invariants live around `cached_records()` /
`ThdContext::record_counts_`; the direct-DML error contract lives in
`try_direct_modify`'s error path; the per-THD clone rule lives on
`ThdContext::db()`. Upstream-coordination drafts live under
[`docs/upstream/`](docs/upstream/).

```
                 +-------------------------------+
                 |     mariadbd (GPL-2.0)        |
                 |   handler / select_handler /  |
                 |   derived_handler / cond_push |
                 +---------------+---------------+
                                 |  dlopen INSTALL PLUGIN
                                 v
                 +-------------------------------+
                 |  ha_stoolap.so (Apache-2.0)   |
                 |   src/ha_stoolap.cc           |  per-row + DDL ops
                 |   src/ha_stoolap_select.cc    |  pushdown handlers
                 |   src/stoolap_bridge.cc       |  RAII over C ABI
                 |   src/stoolap_thd_context.cc  |  per-THD txn / cache
                 |               |  C ABI (rust/include/stoolap_mariadb.h)
                 |   rust/ (static library)      |  Apache-2.0
                 |   stoolap crate, linked in    |
                 +-------------------------------+
```

### Source layout

```
CMakeLists.txt              Build glue. Finds MariaDB headers, builds
                            rust/ with cargo and links it in, sets ABI
                            flags (DBUG_OFF, NDEBUG), wires
                            third_party/wsrep-* include paths.
rust/                       The plugin's Rust crate (static library):
                            stoolap as a dependency (git main; the
                            commit is in rust/Cargo.lock) and the C ABI
                            in rust/include/stoolap_mariadb.h.
src/
  ha_stoolap.{h,cc}         handler subclass: row-pump, indexes, DDL,
                            cond_push, direct DML, status / system vars.
  ha_stoolap_select.{h,cc}  select_handler / derived_handler / unit
                            pushdown factories; direct DML guards and
                            identifier rewriting.
  stoolap_sqlgen.{h,cc}     Pushdown SQL (SELECT, UNION / INTERSECT /
                            EXCEPT, derived tables) generated from
                            MariaDB's prepared Item tree.
  stoolap_bridge.{h,cc}     C++ smart-pointer wrappers around the
                            Stoolap C ABI (DbPtr, TxPtr, RowsPtr,
                            StmtPtr, StoolapBuffer). Process-wide
                            engine handle and statistics counters.
  stoolap_thd_context.{h,cc} Per-connection state: per-THD DB clone,
                            active transaction, prepared bulk-INSERT
                            cache, AUTO_INCREMENT cache with engine
                            epoch invalidation.
  stoolap_thd_inspect.{h,cc} Small sql_class.h-dependent THD inspectors
                            kept out of the hot handler translation unit.
  stoolap_packet.h          Inline parsers for Stoolap's packed
                            fetch_all binary buffer.
third_party/                Vendored MariaDB-required headers
                            (wsrep-lib, wsrep-API). See NOTICE.
tests/                      Python integration harness.
  runner.py                 Spawns a private mariadbd, loads the
                            freshly built plugin, runs every
                            cases/case_*.py file in its own database.
  lib/harness.py            Harness API (assert_ok / assert_scalar /
                            assert_err / assert_pushed / open_session
                            / run_async / run_client).
  cases/case_*.py           One file per topic; ~30-160 assertions each.
benchmarks/bench.py         Side-by-side STOOLAP vs InnoDB dashboard.
```

### Pushdown architecture

The plugin implements three of MariaDB's pushdown contracts:

| Contract           | Surface                       | What we push                            |
| ------------------ | ----------------------------- | --------------------------------------- |
| `select_handler`   | Whole-query SELECT pushdown   | Single-SELECT plans where every leaf is |
|                    |                               | a Stoolap table and every expression    |
|                    |                               | translates (see below).                 |
| `derived_handler`  | Derived-table pushdown        | A FROM-clause subquery materialised on  |
|                    |                               | Stoolap so MariaDB sees the result as a |
|                    |                               | regular table (hybrid stoolap+InnoDB    |
|                    |                               | joins benefit).                         |
| `unit_handler`     | UNION/EXCEPT/INTERSECT        | Whole-unit pushdown when every leg is   |
|                    |                               | Stoolap-only.                           |
| `cond_push`        | Predicate pushdown for        | Returns NULL when Stoolap will          |
|                    | row-pump UPDATE/DELETE        | re-evaluate the WHERE; signals to       |
|                    |                               | MariaDB it can route to the direct DML  |
|                    |                               | hooks instead of the per-row callback.  |
| `pre_direct_*`     | Direct UPDATE / DELETE        | Forwards `thd->query()` verbatim to     |
|                    |                               | Stoolap with leaf-name rewriting.       |

Pushed queries are not forwarded as text. `stoolap_sqlgen` writes the
stoolap SQL from MariaDB's prepared Item tree (the select, unit and
derived handlers are created after prepare, before any optimizer
rewrite), so each construct is translated to its stoolap equivalent or
the statement is left to MariaDB:
- NULL ordering is explicit (`ASC NULLS FIRST` / `DESC NULLS LAST`),
  `<=>` becomes `IS NOT DISTINCT FROM`, literals are always single-quoted,
  and an EXECUTE's placeholders are written as their bound values.
- `CONCAT` becomes `||` (NULL if any argument is NULL), an integer
  branch of a DOUBLE `IF` / `CASE` / `COALESCE` is cast to FLOAT, a
  GROUP BY without ORDER BY is ordered by its keys as MariaDB does, and
  an EXISTS subquery without aggregates selects `1`.
- UNION / INTERSECT / EXCEPT [ALL] keep their written order (both
  engines apply INTERSECT first); an integer leg of a DOUBLE result
  column is cast to FLOAT, and a deduplicating operation pushes only
  when every result column compares the same way in both engines.
  A leg's own ORDER BY / LIMIT is not pushed.
- Comparisons, GROUP BY, ORDER BY, DISTINCT and MIN / MAX push only for
  types that compare the same way in both engines: integers, doubles,
  DATE / DATETIME / TIME / TIMESTAMP (each against its own kind; DATE and
  DATETIME together), and utf8 strings under a binary NO PAD collation
  (or any collation with `stoolap_trust_binary_strings = 1`).
- A DATE / DATETIME or TIME column compared with a string constant
  (`d >= '2024-01-01'`, `dt BETWEEN '2024-01-01' AND '2024-02-01'`)
  pushes with the constant converted the way MariaDB's comparison would,
  unless it parses with a warning or has a zero date part.
- Integer `/`, DECIMAL arithmetic, `LENGTH`, `ROUND`, `SUBSTRING`, CAST,
  `TRIM`, `UPPER` / `LOWER`, `WITH TIES`, `SQL_CALC_FOUND_ROWS`,
  non-boolean WHERE operands and other constructs without an exact
  stoolap equivalent are not pushed.
With `SET stoolap_explain_pushdown = 1` the error log names why a query
was not pushed (`stoolap[explain]: not pushed: <reason>`).

When pushdown succeeds, Stoolap returns rows via `stoolap_rows_fetch_all`
into a packed binary buffer that the plugin parses in-process; no FFI
call per row, no FFI call per cell. For the row-pump path,
`rnd_init` uses the same packed buffer when there is no explicit
LIMIT, falling back to the streaming cursor for early-exit plans.

## Feature matrix

### DDL / schema

| Feature                                        | Status |
| ---------------------------------------------- | ------ |
| `CREATE TABLE` with all numeric types          | Yes    |
| `CREATE TABLE` with VARCHAR/CHAR/TEXT          | Yes    |
| `CREATE TABLE` with DATE / TIME / DATETIME / TIMESTAMP | Yes (full ranges, zero dates; see below) |
| `CREATE TABLE` with DECIMAL                    | Yes (round-tripped via TEXT) |
| `CREATE TABLE` with ENUM / SET                 | Yes (index / bit mask; UNIQUE allowed) |
| `CREATE TABLE` with BINARY / VARBINARY / BLOB  | Yes (any bytes) |
| Character sets other than utf8 (latin1, ...)   | Yes (stored as hex; not pushed) |
| `CREATE TABLE` with JSON                       | Yes (LONGTEXT route)        |
| `DROP TABLE` / `RENAME TABLE` / `TRUNCATE`     | Yes |
| `CREATE INDEX` (single or multi-column)        | Yes |
| `KEY (a, b)` composite                         | Yes (one stoolap index over all parts) |
| `PRIMARY KEY` (single-column inline)           | Yes |
| `PRIMARY KEY (a, b)` composite                 | Yes (enforced as a NOT NULL composite unique index) |
| `UNIQUE` constraints (single-column)           | Yes |
| `UNIQUE (a, b)` composite                      | Yes |
| `PRIMARY KEY` / `UNIQUE` on ci-collated VARCHAR | Refused at CREATE (use `_bin` collation or drop the constraint) |
| `PRIMARY KEY` / `UNIQUE` over a column prefix (`a(3)`) | Refused at CREATE (stoolap enforces whole-column uniqueness) |
| `FOREIGN KEY` (single-column inline)           | Yes (RESTRICT, CASCADE) |
| Multi-column FK / self-referencing FK          | Refused at CREATE with a client-visible error |
| `AUTO_INCREMENT`                               | Yes (cross-connection cache invalidates on TRUNCATE/DROP/RENAME) |
| `ALTER TABLE ADD/DROP COLUMN`                  | Yes |
| `ALTER TABLE AUTO_INCREMENT = N`               | Ignored (recomputes from MAX) |
| `SHOW CREATE TABLE`                            | Yes |
| `SHOW INDEXES` (incl. HNSW parameters)         | Yes |

### DML

| Feature                                        | Status |
| ---------------------------------------------- | ------ |
| `INSERT` single-row, multi-row VALUES          | Yes |
| `INSERT ... SELECT`                            | Yes |
| `INSERT ... ON DUPLICATE KEY UPDATE`           | Yes (PK + UNIQUE; multi-row routes per-row) |
| `REPLACE INTO`                                 | Yes (multi-row routes per-row) |
| `INSERT IGNORE` (single + multi-row)           | Yes (per-row callbacks; non-conflicting rows kept) |
| `UPDATE` / `DELETE` with `WHERE`               | Yes (direct DML when shape allows; constraint errors map to ER_DUP_ENTRY / ER_NO_REFERENCED_ROW_2) |
| `UPDATE`/`DELETE` with `ORDER BY` + `LIMIT`    | Row-pump (filesort-based plan; requires single-column PK) |
| Bulk `INSERT` (start_bulk_insert + write_row)  | Yes (single FFI batch per chunk; opted out for IGNORE/REPLACE/ODKU shapes that need per-row dup callbacks) |
| `LAST_INSERT_ID()` after AUTO_INCREMENT INSERT | Yes |
| Multi-table UPDATE / DELETE                    | Row-pump |

### Query

| Feature                                        | Status |
| ---------------------------------------------- | ------ |
| Whole SELECT pushdown                          | Yes (constructs with an exact stoolap translation) |
| Derived-table pushdown                         | Yes |
| UNION / INTERSECT / EXCEPT [ALL] pushdown      | Yes (single-engine units) |
| Cross-engine derived (Stoolap inside InnoDB)   | Yes |
| Aggregation: COUNT, SUM, MIN, MAX, AVG         | Yes (engine-side) |
| GROUP BY, HAVING                               | Yes |
| Window functions (ROW_NUMBER, RANK, LAG, ...)  | Yes |
| Frame clauses (`ROWS BETWEEN`)                 | Yes |
| CTE / recursive CTE                            | Yes |
| Subqueries: scalar / EXISTS / IN / NOT EXISTS  | Yes |
| Joins: INNER, LEFT, RIGHT, FULL OUTER          | Yes |
| CROSS JOIN                                     | Yes |
| Self-join                                      | Yes |
| Predicate pushdown for direct DML              | Yes (`cond_push` agrees with `can_direct_modify`) |
| Adaptive projection (read_set ∪ write_set)     | Yes (buffered scan path) |
| Range scans via index (`>=`, `>`, `BETWEEN`)   | Yes (engine emits the bound op) |
| `index_first` / `index_last` (MIN/MAX shortcut)| Yes |
| Prepared-statement pushdown (binary protocol)  | Yes |
| `EXPLAIN`                                      | Yes (`PUSHED SELECT` / `PUSHED DERIVED`) |

### Transactions / isolation

| Feature                                        | Status |
| ---------------------------------------------- | ------ |
| `BEGIN` / `COMMIT` / `ROLLBACK`                | Yes (per-THD `StoolapTx`) |
| Autocommit                                     | Yes (skips Tx, runs `exec_params`) |
| `READ COMMITTED` isolation                     | Yes |
| `READ UNCOMMITTED`                             | Mapped to `READ COMMITTED` (Stoolap is MVCC) |
| `REPEATABLE READ` isolation                    | Yes (opens a Stoolap snapshot tx) |
| `SERIALIZABLE` isolation                       | Yes (opens a Stoolap snapshot tx) |
| `START TRANSACTION WITH CONSISTENT SNAPSHOT`   | Yes (snapshot anchored at BEGIN time via the handlerton callback) |
| Write-write conflicts → `ER_LOCK_DEADLOCK`     | Yes (SQLSTATE 40001) |
| Crash recovery via WAL replay on `open()`      | Yes |
| `SAVEPOINT name` / `RELEASE` / `ROLLBACK TO`   | Yes (per-connection synthesised name; MariaDB does not pass the user's name to engines) |
| 2PC / XA                                       | No |

### Error mapping

| Stoolap error                                  | MariaDB error |
| ---------------------------------------------- | ------------- |
| Primary-key / UNIQUE constraint violation      | `ER_DUP_ENTRY 1062` (with correct key in message) |
| FK violation                                   | `ER_NO_REFERENCED_ROW_2 1452` |
| `CREATE TABLE` of existing name                | `ER_TABLE_EXISTS_ERROR 1050` |
| Write-write conflict                           | `ER_LOCK_DEADLOCK 1213` (40001) |
| Conflict at COMMIT                             | `ER_ERROR_DURING_COMMIT 1180` |

### Value storage

Each column is stored so that stoolap's comparisons and order match
MariaDB's (`src/stoolap_types.h`):

| MariaDB type | Stoolap column | Value |
| --- | --- | --- |
| DATE, DATETIME, TIME | INTEGER | MariaDB's `pack_time()` value: years 0000-9999, zero and partial dates, negative and >99 h times; a DATE is the DATETIME at midnight |
| TIMESTAMP | INTEGER | UTC microseconds since the epoch, independent of `time_zone` |
| ENUM / SET | INTEGER | the index / bit mask MariaDB sorts by |
| BINARY / VARBINARY / BLOB, and text in latin1 etc. | TEXT | uppercase hex of the bytes, which keeps their order (stoolap TEXT is UTF-8; its BLOB type holds vectors) |
| DECIMAL | TEXT | the decimal string |
| integers, doubles, utf8 text | INTEGER / FLOAT / TEXT | as is |

Direct UPDATE / DELETE forward the statement text, so they decline any
statement that touches a DECIMAL or encoded column; the row pump runs
it. Tables created by an older build must be re-created.

## Known limitations

- **`rnd_pos` over BLOB/TEXT tables**: any plan that takes the
  filesort + rowid-sort path (e.g. `UPDATE ... ORDER BY n LIMIT 2`)
  fails loudly with a clear error rather than corrupting the row,
  because the BLOB Field's in-record bytes are a (length, ptr) trio
  and restoring them blindly would resurrect a freed pointer. Plain
  WHERE-driven UPDATE/DELETE on BLOB tables works.
- **Composite `PRIMARY KEY` / `UNIQUE` / `FOREIGN KEY` are refused at
  `CREATE TABLE`.** Stoolap enforces single-column constraints only, so
  silently accepting a multi-column constraint would let duplicates
  (or FK-violating rows) in. The error message names the offending
  constraint and points at the workaround (drop it, or use a
  single-column form). Self-referencing FKs are refused for the same
  class of reason: Stoolap resolves the parent table at CREATE-time,
  before our just-being-created table is registered.
- **Charset/collation**. Stoolap compares strings byte-wise. The plugin
  detects ci-collated columns and bails pushdown to the row pump so
  MariaDB applies its own collation. `SET stoolap_trust_binary_strings = 1`
  opts in to byte-wise pushdown for ASCII-clean data.
- **`PRIMARY KEY` / `UNIQUE` on case-insensitive string columns is
  refused at `CREATE TABLE` time.** Stoolap enforces UNIQUE byte-wise,
  so a `_general_ci` VARCHAR UNIQUE would silently accept `'a'` and
  `'A'` as distinct rows. Either declare the column with `_bin`
  collation (e.g. `email VARCHAR(80) COLLATE utf8mb4_bin UNIQUE`) or
  drop the constraint. Non-unique secondary keys on ci-string columns
  are allowed; the byte-wise stoolap index is just a best-effort access
  hint, not a uniqueness contract.
- **Row-pump `UPDATE`/`DELETE` on no-PK tables is refused.** Stoolap
  has no stable row-id and its `UPDATE/DELETE` grammar has no `LIMIT`,
  so a fingerprint-by-all-columns WHERE could over-mutate byte-identical
  duplicates when MariaDB's filesort+LIMIT path calls `update_row` per
  logical row. Add a single-column `PRIMARY KEY`, or write the
  statement so the WHERE alone selects the rows -- the direct DML hook
  (`cond_push` -> stoolap WHERE) handles that case as one engine-side
  query.
- **`ALTER TABLE AUTO_INCREMENT = N` is not propagated.** The plugin
  recomputes the next value from `MAX(col)+1` per connection, with a
  cross-connection epoch invalidating on TRUNCATE/DROP/RENAME.

## Build

### Prerequisites

1. **MariaDB 11.4 server-internal headers.** `cmake/FindMariaDB.cmake`
   auto-discovers the install across the common layouts:

   | Platform | Install command | Auto-detected at |
   | --- | --- | --- |
   | macOS (Homebrew) | `brew install mariadb@11.4` | `/opt/homebrew/opt/mariadb@11.4` (Apple Silicon) or `/usr/local/opt/mariadb@11.4` (Intel) |
   | Debian / Ubuntu | `sudo apt-get install mariadb-server libmariadbd-dev` (after enabling MariaDB's official repo at https://r.mariadb.com/downloads/mariadb_repo_setup) | `/usr/include/mysql/server/private/` |
   | Red Hat / Fedora | `sudo dnf install mariadb-server mariadb-server-devel` | `/usr/include/mariadb/server/private/` |
   | Source build | `cmake --install build --prefix "$PREFIX"` from a MariaDB checkout, then `-DMariaDB_ROOT="$PREFIX"` | wherever `$PREFIX/include/mysql/server/private/handler.h` lives |

   The find module also tries `mariadb_config --prefix` as a last resort.
   Pass `-DMariaDB_ROOT=/explicit/path` to override discovery.

2. **A Rust toolchain** (`cargo`, stable). The build fetches Stoolap
   from git (`rust/Cargo.toml`; the commit is pinned in
   `rust/Cargo.lock`, and `cargo update --manifest-path rust/Cargo.toml
   -p stoolap` moves it to main's head).

### Compile

```sh
cmake -S . -B build
cmake --build build -j
```

To build against a local Stoolap checkout instead, pass
`-DSTOOLAP_DIR=../stoolap`.

`MariaDB_ROOT` only needs to be passed if auto-discovery missed your install.
The legacy `MARIADB_PREFIX` is accepted as a backwards-compatible alias.

The output is `build/ha_stoolap.so` (`.so` on Linux; on macOS the
target also uses the `.so` suffix because the MariaDB plugin loader
looks for a literal `.so` extension on Mach-O modules).

### Build-flag rationale

Already encoded in `CMakeLists.txt`, but worth knowing:

- `DBUG_OFF`, `NDEBUG` are defined. Distribution `mariadbd` binaries are
  release builds and do not export `_db_keyword_` etc.; any `DBUG_*` macro
  in our headers would fail to `dlopen`.
- No `-fvisibility=hidden`. The `maria_declare_plugin` macro emits
  `_maria_plugin_interface_version_` and `_maria_plugin_declarations_`
  without `visibility("default")`, so they vanish under hidden
  visibility and the server reports "Can't find symbol".
- No `-fno-rtti`. MariaDB's `field.h` uses `dynamic_cast` inside
  inline `DBUG_ASSERT`s.
- `sql_class.h` is intentionally avoided in the hot translation units;
  it transitively pulls `wsrep/*.hpp`, which Homebrew's MariaDB
  references via `WITH_WSREP=1` in `my_config.h` but does not ship.
  We vendor those headers under `third_party/` so the build works on
  any layout (apt, brew, source). `sql_print_*` is declared in `log.h`,
  which is enough for the row-pump TU.
- macOS link: `-undefined dynamic_lookup`. Linux link:
  `-Wl,--unresolved-symbols=ignore-in-shared-libs`. Server symbols
  resolve at `dlopen` time inside `mariadbd`.
- macOS post-link: ad-hoc `codesign --sign -` on the `.so`. dyld on
  macOS 14+ refuses to load otherwise.

## Install

Stoolap is linked into `ha_stoolap.so`; copy that one file:

```sh
sudo install -m 0755 build/ha_stoolap.so "$(mariadb_config --plugindir)/"
```

### Plugin directory paths

`mariadb_config --plugindir` resolves to `/opt/homebrew/opt/mariadb@11.4/lib/plugin`
on macOS (Brew), `/usr/lib/mysql/plugin` on Debian/Ubuntu, `/usr/lib64/mysql/plugin`
on Red Hat/Fedora. CMake also exposes the discovered path as `MariaDB_PLUGIN_DIR`.

Plugin maturity is `MariaDB_PLUGIN_MATURITY_ALPHA`. The server default
is GAMMA, so start `mariadbd` with `--plugin-maturity=alpha`:

```sh
mariadbd --plugin-maturity=alpha   # plus your other flags
```

Then in a SQL client:

```sql
INSTALL PLUGIN stoolap SONAME 'ha_stoolap.so';
SHOW ENGINES;                          -- STOOLAP appears
SHOW VARIABLES LIKE 'stoolap_%';       -- stoolap_dsn, stoolap_perf_trace
```

### Configuration

Plugin-level (`SET GLOBAL ...`):

| Variable                       | Type | Default     | Effect                                           |
| ------------------------------ | ---- | ----------- | ------------------------------------------------ |
| `stoolap_dsn`                  | str  | `memory://` | Stoolap DSN. Use `file:///path` for persistence. |
| `stoolap_perf_trace`           | bool | `OFF`       | Populate `Stoolap_perf_*` status counters.       |

Session-level (`SET SESSION ...`):

| Variable                          | Type | Default | Effect                                       |
| --------------------------------- | ---- | ------- | -------------------------------------------- |
| `stoolap_trust_binary_strings`    | bool | `OFF`   | Push string predicates as byte-wise compares. Use only when data is ASCII / already-cased. |

### Status counters (`SHOW STATUS LIKE 'Stoolap_%'`)

| Name                              | Meaning                                  |
| --------------------------------- | ---------------------------------------- |
| `Stoolap_pushdown_hits`           | Whole-query plans accepted by the factory. |
| `Stoolap_pushdown_misses`         | Plans that fell back to the row pump.    |
| `Stoolap_direct_modify_hits`      | UPDATE / DELETE that took the direct path. |
| `Stoolap_records_live_counts`     | `cached_records()` cache miss that ran a live `SELECT COUNT(*)`. High values mean the handler / tx / global record-count caches are not being reused. |
| `Stoolap_buffered_scans`          | `rnd_init` calls that used `fetch_all`.  |
| `Stoolap_buffered_rows`           | Rows delivered through the buffered path. |
| `Stoolap_unmapped_errors`         | A non-empty stoolap error message matched no `map_stoolap_error` pattern. Non-zero means stoolap reworded or added an error class -- see `case_18_error_mapping`. |
| `Stoolap_perf_factory_setup_ns`   | Time spent inside `create_select` / `create_derived`. |
| `Stoolap_perf_eager_query_ns`     | Time spent inside `stoolap_query` / `fetch_all`. |
| `Stoolap_perf_init_scan_ns`       | Time spent in `init_scan` per pushed query. |
| `Stoolap_perf_next_row_ns`        | Total time across all `next_row` calls.  |
| `Stoolap_perf_end_scan_ns`        | Total time across all `end_scan` calls.  |
| `Stoolap_perf_query_count`        | Pushed-query factory calls.              |
| `Stoolap_perf_next_row_count`     | `next_row` invocations.                  |

`Stoolap_perf_*` are only updated when `stoolap_perf_trace = ON`.

### Smoke test (isolated datadir)

Assumes `mariadbd` / `mariadb` / `mariadb-install-db` / `mariadb_config`
are on `PATH`. On Homebrew that means
`brew --prefix mariadb@11.4`/bin is on `PATH`; on apt/dnf installs the
binaries land in `/usr/sbin` and `/usr/bin` directly.

```sh
PLUGIN_DIR="$(mariadb_config --plugindir)"
rm -rf /tmp/stoolap-test-data

mariadb-install-db --no-defaults \
  --datadir=/tmp/stoolap-test-data \
  --auth-root-authentication-method=normal

mariadbd --no-defaults \
  --datadir=/tmp/stoolap-test-data \
  --socket=/tmp/stoolap-test.sock --skip-networking \
  --plugin-dir="$PLUGIN_DIR" \
  --plugin-maturity=alpha &

mariadb --no-defaults -S /tmp/stoolap-test.sock -uroot -e \
  "INSTALL PLUGIN stoolap SONAME 'ha_stoolap.so'; SHOW ENGINES;"
```

`--no-defaults` is essential: a `mysqlx-bind-address` from a different
MariaDB install in `/etc/my.cnf` or `~/.my.cnf` will crash 11.4 at
startup.

The Python test runner (`tests/runner.py`) automates this dance and
spawns its own private `mariadbd`; you only need the manual smoke test
when validating an install layout.

## Testing

```sh
pip install mysql-connector-python      # one-time

python3 tests/runner.py                 # run every cases/case_*.py
python3 tests/runner.py 06_pushdown     # one case by basename substring
KEEP_RUNNING=1 python3 tests/runner.py  # leave mariadbd up for repl
REUSE_RUNNING=1 python3 tests/runner.py # reuse the running server
STOOLAP_DSN=file:///tmp/stoolap-test-stoolap-py python3 tests/runner.py
                                        # run against persistent stoolap
```

The runner spins up a private `mariadbd` against `/tmp/stoolap-test-data`
on `/tmp/stoolap-test.sock`, loads the freshly built plugin, and runs
each Python case in its own database. Suites cover CRUD, indexes,
transactions, ODKU/REPLACE/AUTO_INCREMENT (incl. multi-row dup
recovery), foreign keys, pushdown (with EXPLAIN-based assertions),
aggregations + joins, edge cases (NULLs, BLOB/TEXT, boundary
numerics), strings, numerics, concurrency, DDL surface, scale
(50K rows vs InnoDB), error message parity, transactional
visibility / isolation (`REPEATABLE READ` snapshot stability,
`READ COMMITTED` post-commit visibility), unsupported-DDL
rejection (composite / self-referencing FK, direct UPDATE/DELETE error
mapping to ER_DUP_ENTRY 1062 / ER_NO_REFERENCED_ROW_2 1452),
error-class round-trip (every known stoolap error class reaches the
client with the right MariaDB errno; drift counters stay at
baseline), and savepoint plumbing (SAVEPOINT / RELEASE / ROLLBACK
TO including nested and reused names).

## Performance

Side-by-side comparison against InnoDB on the same server, mirroring
`stoolap/examples/benchmark.rs` (10K rows, 500/250/50 iteration tiers,
WARMUP=10). Highlights from a recent run:

| Workload                       | STOOLAP    | InnoDB     | Ratio     |
| ------------------------------ | ---------- | ---------- | --------- |
| LEFT JOIN + GROUP BY           | 6311 ops/s | 54 ops/s   | 116x      |
| Compare with subquery          | 7261 ops/s | 257 ops/s  | 28x       |
| Window ROW_NUMBER (PK)         | 14765 ops/s | 609 ops/s | 24x       |
| NOT IN / NOT EXISTS subquery   | 6069 ops/s | 327 ops/s  | 18-20x    |
| Aggregation (GROUP BY)         | 8645 ops/s | 954 ops/s  | 9x        |
| INNER JOIN                     | 8142 ops/s | 994 ops/s  | 8x        |
| INSERT single                  | 50K ops/s  | 35K ops/s  | 1.4x      |
| SELECT by ID                   | 32K ops/s  | 38K ops/s  | 0.84x     |
| CROSS JOIN (limited)           | 5959 ops/s | 14653 ops/s | 0.41x    |

Stoolap dominates analytical workloads (joins with grouping, window
functions, subqueries, aggregations). InnoDB stays competitive on
simple OLTP point lookups and on a handful of cases where MariaDB's
per-row Item evaluation in user space dominates the engine cost
(`SELECT by ID`, `Math expressions`, `CROSS JOIN`).

## Licensing

This project is dual-aware:

- The plugin source code under `src/` and the build glue at the
  repository root are licensed under the
  [Apache License, Version 2.0](LICENSE).
- The vendored upstream headers under `third_party/` are GPL-2.0
  (see `third_party/wsrep-lib/COPYING` and
  `third_party/wsrep-API/COPYING`). They are required only at compile
  time and ship in their original form.
- MariaDB Server is GPL-2.0 with a
  [FOSS License Exception](https://mariadb.com/foss-license-exception/)
  that explicitly permits Apache-2.0 plugins to link against it. The
  compiled `ha_stoolap.so` falls under that exception when distributed
  alongside MariaDB.
- Stoolap itself is Apache-2.0.

See [NOTICE](NOTICE) for the full attribution and compatibility
breakdown. There is no copyleft contamination of the plugin source: an
Apache-2.0 contributor sees only Apache-2.0 code in `src/`. The GPL-2.0
materials are kept in `third_party/` with their own license files.

## Open questions

- **Allocation pressure in MariaDB.** Several "MariaDB-evaluates-Items-per-row"
  shapes (math expressions over 100 rows, scalar subqueries in projection,
  cross joins) keep STOOLAP within 0.4-0.95x of InnoDB despite an order-of-
  magnitude faster engine. Adaptive projection on the pushdown path and
  per-template prepared-statement caching for hot point lookups should
  recover most of that gap.
- **2PC / XA.** Needs a `prepare` callback and per-THD txn-id, neither of
  which Stoolap currently exposes.
- **Charset / collation propagation.** Today the plugin's response to
  ci collation is to bail to the row pump. Pushing collation context into
  Stoolap would unlock pushdown for the long tail of `_general_ci`
  workloads.
