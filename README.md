# stoolap-mariadb

[![CI](https://github.com/stoolap/stoolap-mariadb/actions/workflows/ci.yml/badge.svg)](https://github.com/stoolap/stoolap-mariadb/actions/workflows/ci.yml)

MariaDB 11.4 storage engine plugin (`ha_stoolap.so`) backed by
[Stoolap](https://github.com/stoolap/stoolap), an embedded analytical
SQL engine written in Rust. The plugin is a C++ shim that translates
the MariaDB handler API into calls on Stoolap's C ABI
(`include/stoolap.h`, built with `cargo build --release --features ffi`).

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
against MariaDB 11.4.10 with 17 Python integration test files (517+
assertions, all green on `memory://` and `file://` DSNs).

Recent releases focus on:
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
- Read-only mode and DSN handling for production deployments.

## Architecture

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
                 +---------------+---------------+
                                 |  C ABI calls
                                 v
                 +-------------------------------+
                 |  libstoolap.{so,dylib}        |  Apache-2.0
                 |  (Rust crate, --features ffi) |
                 +-------------------------------+
```

### Source layout

```
CMakeLists.txt              Build glue. Finds MariaDB headers + libstoolap,
                            sets ABI flags (DBUG_OFF, NDEBUG), wires
                            third_party/wsrep-* include paths.
src/
  ha_stoolap.{h,cc}         handler subclass: row-pump, indexes, DDL,
                            cond_push, direct DML, status / system vars.
  ha_stoolap_select.{h,cc}  select_handler / derived_handler / unit
                            pushdown factories. ci-collation guard.
                            Adaptive projection. Identifier rewriting.
  stoolap_bridge.{h,cc}     C++ smart-pointer wrappers around the
                            Stoolap C ABI (DbPtr, TxPtr, RowsPtr,
                            StmtPtr, StoolapBuffer). Process-wide
                            engine handle and statistics counters.
  stoolap_thd_context.{h,cc} Per-connection state: per-THD DB clone,
                            active transaction, prepared bulk-INSERT
                            cache, AUTO_INCREMENT cache with engine
                            epoch invalidation.
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
|                    |                               | a Stoolap table and the WHERE/HAVING/   |
|                    |                               | ORDER/GROUP/projection pass the         |
|                    |                               | ci-collation guard.                     |
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

The ci-collation guard recursively walks WHERE/HAVING/ORDER/GROUP, the
SELECT list (skipping bare field refs that don't compare), every
derived TABLE_LIST entry, and every Item_subselect (with EXISTS_SUBS
treated specially: its SELECT-list is semantically ignored). When any
ci-collated column appears in a compare-sensitive surface, pushdown
bails to the row pump so MariaDB applies its own case-insensitive
compare. `SET stoolap_trust_binary_strings = 1` opts out and pushes
anyway.

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
| `CREATE TABLE` with DATE / TIME / DATETIME / TIMESTAMP | Yes (DATETIME ~1678-2262) |
| `CREATE TABLE` with DECIMAL                    | Yes (round-tripped via TEXT) |
| `CREATE TABLE` with ENUM / SET                 | Yes (label / member-joined) |
| `CREATE TABLE` with BLOB / VARBINARY           | Partial (UTF-8 only)        |
| `CREATE TABLE` with JSON                       | Yes (LONGTEXT route)        |
| `DROP TABLE` / `RENAME TABLE` / `TRUNCATE`     | Yes |
| `CREATE INDEX` (single-column)                 | Yes |
| `KEY (a, b)` composite (uses leading column)   | Partial |
| `PRIMARY KEY` (single-column inline)           | Yes |
| `PRIMARY KEY (a, b)` composite                 | Refused at CREATE (Stoolap only enforces single-column) |
| `UNIQUE` constraints (single-column)           | Yes |
| `UNIQUE (a, b)` composite                      | Refused at CREATE |
| `PRIMARY KEY` / `UNIQUE` on ci-collated VARCHAR | Refused at CREATE (use `_bin` collation or drop the constraint) |
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
| Whole SELECT pushdown                          | Yes (subject to ci-collation guard) |
| Derived-table pushdown                         | Yes |
| UNION / UNION ALL pushdown                     | Yes (single-engine units) |
| Cross-engine derived (Stoolap inside InnoDB)   | Yes |
| Aggregation: COUNT, SUM, MIN, MAX, AVG         | Yes (engine-side) |
| GROUP BY, HAVING                               | Yes |
| Window functions (ROW_NUMBER, RANK, LAG, ...)  | Yes |
| Frame clauses (`ROWS BETWEEN`)                 | Yes |
| CTE / recursive CTE                            | Yes |
| Subqueries: scalar / EXISTS / IN / NOT EXISTS  | Yes (ci guard recurses into derivations) |
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
| `SAVEPOINT name`                               | No (not in Stoolap C ABI yet) |
| 2PC / XA                                       | No |
| Read-only mode (`stoolap_dsn` with `?read_only=1`) | Yes |

### Error mapping

| Stoolap error                                  | MariaDB error |
| ---------------------------------------------- | ------------- |
| Primary-key / UNIQUE constraint violation      | `ER_DUP_ENTRY 1062` (with correct key in message) |
| FK violation                                   | `ER_NO_REFERENCED_ROW_2 1452` |
| `CREATE TABLE` of existing name                | `ER_TABLE_EXISTS_ERROR 1050` |
| Write-write conflict                           | `ER_LOCK_DEADLOCK 1213` (40001) |
| Conflict at COMMIT                             | `ER_ERROR_DURING_COMMIT 1180` |

## Known limitations

- **`DATETIME` is bounded to roughly 1678..2262.** Stoolap stores
  timestamps as i64 nanoseconds; out-of-range values (`9999-12-31`)
  return `HA_ERR_UNSUPPORTED` rather than corrupt silently.
- **`BLOB` / `VARBINARY` silently drop non-UTF-8 bytes.** Stoolap's
  `BLOB` type is reserved for vector columns, so we route the MariaDB
  BLOB family through Stoolap `TEXT`. Binary content with bytes that
  aren't valid UTF-8 (e.g. `UNHEX('FF00FF00')`) round-trips as `NULL`.
  ASCII / UTF-8-clean text in BLOB columns works.
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

1. MariaDB 11.4 server-internal headers. On macOS via Homebrew:

   ```sh
   brew install mariadb@11.4
   ```

   That ships everything under
   `/opt/homebrew/opt/mariadb@11.4/include/mysql/server`.

2. A release build of Stoolap with the FFI feature:

   ```sh
   cd ../stoolap
   cargo build --release --features ffi
   ```

   Produces `target/release/libstoolap.{dylib,so}`.

### Compile

```sh
cmake -S . -B build \
  -DMARIADB_PREFIX=/opt/homebrew/opt/mariadb@11.4 \
  -DSTOOLAP_DIR=../stoolap
cmake --build build -j
```

The output is `build/ha_stoolap.so` (.so on Linux, .so with
`SUFFIX ".so"` on macOS too. The MariaDB plugin loader looks for the
literal `.so` extension on Mach-O modules).

### Build-flag rationale

Already encoded in `CMakeLists.txt`, but worth knowing:

- `DBUG_OFF`, `NDEBUG` are defined. Brew's `mariadbd` is a release
  build and does not export `_db_keyword_` etc.; any `DBUG_*` macro in
  our headers would fail to dlopen.
- No `-fvisibility=hidden`. The `maria_declare_plugin` macro emits
  `_maria_plugin_interface_version_` and `_maria_plugin_declarations_`
  without `visibility("default")`, so they vanish under hidden
  visibility and the server reports "Can't find symbol".
- No `-fno-rtti`. MariaDB's `field.h` uses `dynamic_cast` inside
  inline `DBUG_ASSERT`s.
- `sql_class.h` is intentionally avoided in the row-pump translation
  unit; it transitively pulls `wsrep/*.hpp`, which the brewed headers
  reference via `WITH_WSREP=1` in `my_config.h` but do not ship.
  `sql_print_*` are declared in `log.h`, which is enough.
- macOS link: `-undefined dynamic_lookup`. Linux link:
  `-Wl,--unresolved-symbols=ignore-in-shared-libs`. Server symbols
  resolve at `dlopen` time inside `mariadbd`.
- macOS post-link: ad-hoc `codesign --sign -` on the `.so`. dyld on
  macOS 14+ refuses to load otherwise.

## Install

```sh
cp build/ha_stoolap.so /opt/homebrew/opt/mariadb@11.4/lib/plugin/
```

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
| `stoolap_dsn`                  | str  | `memory://` | Stoolap DSN. Use `file:///path` for persistence. Append `?read_only=1` for read-only mode. |
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
| `Stoolap_buffered_scans`          | `rnd_init` calls that used `fetch_all`.  |
| `Stoolap_buffered_rows`           | Rows delivered through the buffered path. |
| `Stoolap_unmapped_errors`         | Stoolap error strings the plugin couldn't classify (degraded to ER_GET_ERRMSG 1296). Non-zero means the error-mapping table in `map_stoolap_error` has fallen behind stoolap's wording — see `case_18_error_mapping`. |
| `Stoolap_perf_factory_setup_ns`   | Time spent inside `create_select` / `create_derived`. |
| `Stoolap_perf_eager_query_ns`     | Time spent inside `stoolap_query` / `fetch_all`. |
| `Stoolap_perf_init_scan_ns`       | Time spent in `init_scan` per pushed query. |
| `Stoolap_perf_next_row_ns`        | Total time across all `next_row` calls.  |
| `Stoolap_perf_end_scan_ns`        | Total time across all `end_scan` calls.  |
| `Stoolap_perf_query_count`        | Pushed-query factory calls.              |
| `Stoolap_perf_next_row_count`     | `next_row` invocations.                  |

`Stoolap_perf_*` are only updated when `stoolap_perf_trace = ON`.

### Smoke test (isolated datadir)

```sh
rm -rf /tmp/stoolap-test-data
/opt/homebrew/opt/mariadb@11.4/bin/mariadb-install-db --no-defaults \
  --datadir=/tmp/stoolap-test-data \
  --auth-root-authentication-method=normal

/opt/homebrew/opt/mariadb@11.4/bin/mariadbd --no-defaults \
  --datadir=/tmp/stoolap-test-data \
  --socket=/tmp/stoolap-test.sock --skip-networking \
  --plugin-dir=/opt/homebrew/opt/mariadb@11.4/lib/plugin \
  --plugin-maturity=alpha &

mariadb --no-defaults -S /tmp/stoolap-test.sock -uroot -e \
  "INSTALL PLUGIN stoolap SONAME 'ha_stoolap.so'; SHOW ENGINES;"
```

`--no-defaults` is essential: a `mysqlx-bind-address` from a different
MariaDB install in `/etc/my.cnf` or `~/.my.cnf` will crash 11.4 at
startup.

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
`READ COMMITTED` post-commit visibility), and unsupported-DDL
rejection (composite PK/UNIQUE/FK, direct UPDATE/DELETE error
mapping to ER_DUP_ENTRY 1062 / ER_NO_REFERENCED_ROW_2 1452).

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
- **SAVEPOINT.** Requires `stoolap_tx_savepoint_create / release / rollback`
  in the Stoolap C ABI; tracked upstream.
- **2PC / XA.** Needs a `prepare` callback and per-THD txn-id, neither of
  which Stoolap currently exposes.
- **Charset / collation propagation.** Today the plugin's response to
  ci collation is to bail to the row pump. Pushing collation context into
  Stoolap would unlock pushdown for the long tail of `_general_ci`
  workloads.
