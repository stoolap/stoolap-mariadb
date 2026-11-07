# stoolap-mariadb

[![CI](https://github.com/stoolap/stoolap-mariadb/actions/workflows/ci.yml/badge.svg)](https://github.com/stoolap/stoolap-mariadb/actions/workflows/ci.yml)

A MariaDB 11.4 storage engine plugin (`ha_stoolap.so`) backed by
[Stoolap](https://github.com/stoolap/stoolap), an embedded SQL engine
written in Rust. `CREATE TABLE ... ENGINE=STOOLAP` stores a table in
Stoolap; MariaDB's parser, optimizer, protocol and tools stay in front
of it.

Two things make it fast:
- **Whole statements run in Stoolap.** SELECTs, derived tables, set
  operations, UPDATE and DELETE are written as Stoolap SQL from MariaDB's
  parsed Item tree and run by Stoolap's executor. A construct with no
  exact Stoolap equivalent is not guessed at: the statement stays with
  MariaDB.
- **Row access goes below SQL.** The handler's scans, key lookups and
  writes go through the plugin's Rust crate (`rust/`) straight to
  Stoolap's table handles, and rows are decoded into MariaDB's record
  format in Rust, with no SQL parse or plan per call.

The aim is InnoDB's behaviour, not just its syntax: results, errors,
affected-row counts and constraint checks should match. The remaining
differences are listed under [Known differences](#known-differences),
each with a scenario in `tests/diff/` that fails once it is fixed.

## Status

Alpha (`MariaDB_PLUGIN_MATURITY_ALPHA`). MariaDB 11.4 only; the build
warns on any other series.

CI builds and runs the suite on Ubuntu and macOS against the current
MariaDB 11.4 release, twice: on the in-memory DSN and on persistent
`file://` storage (21 case files, about 800 assertions each). The suite
includes a differential harness that runs every `tests/diff/*.sql`
scenario on STOOLAP and on InnoDB and compares each statement's rows,
errno and affected-row count, and a restart test that stops and kills
its own server on `file://` storage and checks what survives.

## Architecture

```
                 +----------------------------------+
                 |        mariadbd (GPL-2.0)        |
                 |  parser / optimizer / handler    |
                 +----------------+-----------------+
                                  | INSTALL PLUGIN (dlopen)
                                  v
                 +----------------------------------+
                 |   ha_stoolap.so (Apache-2.0)     |
                 |                                  |
                 |  src/  C++ shims over MariaDB    |
                 |    ha_stoolap.cc     handler     |
                 |    ha_stoolap_select pushdown,   |
                 |                      direct DML  |
                 |    stoolap_sqlgen    Item tree   |
                 |                      -> SQL      |
                 |                |  C ABI          |
                 |                v  (rust/include) |
                 |  rust/  the plugin's crate:      |
                 |    reads, writes, DDL, errors    |
                 |    stoolap crate, linked in      |
                 +----------------------------------+
```

The C++ side is kept to what needs MariaDB's types; logic that does not
lives in the Rust crate. Design notes live in block comments next to the
code they explain.

### Source layout

```
CMakeLists.txt              Build glue: finds MariaDB's headers, builds rust/
                            with cargo and links it in, sets ABI flags,
                            wires the vendored third_party/ headers.
rust/                       The plugin's Rust crate (static library), with
                            Stoolap as a dependency (git main; the commit
                            is pinned in rust/Cargo.lock). Its C ABI is
                            rust/include/stoolap_mariadb.h.
  src/session.rs            One plugin session: its executor and open
                            transaction, shared by SQL and direct access.
  src/read.rs, key.rs       Scans and key lookups on table handles; key
                            bounds into conditions.
  src/record.rs             Rows straight between Stoolap and a MariaDB
                            record buffer.
  src/write.rs              Inserts (batched, UNIQUE checked per batch),
                            updates and deletes by row id.
  src/sql_write.rs          Writes through SQL, for tables a FOREIGN KEY
                            names.
  src/ddl.rs                CREATE TABLE / indexes / FOREIGN KEYs from
                            MariaDB's definitions; DROP with its checks.
  src/ffi/                  The C functions, and error classes taken from
                            Stoolap's error values (ffi/error.rs).
src/
  ha_stoolap.{h,cc}         The handler: row access, indexes, DDL,
                            transactions, direct DML hooks, system and
                            status variables.
  ha_stoolap_select.{h,cc}  select_handler / derived_handler / unit handler
                            factories and the direct UPDATE / DELETE path.
  stoolap_sqlgen.{h,cc}     Stoolap SQL from MariaDB's Item tree: SELECT,
                            set operations, derived tables, UPDATE, DELETE.
  stoolap_types.h           How each MariaDB type is stored in Stoolap.
  stoolap_bridge.{h,cc}     RAII wrappers over the C ABI, the process-wide
                            engine handle, statistics counters, table names.
  stoolap_thd_context.{h,cc} Per-connection state: the session's engine
                            handle, its transaction, record-count and
                            AUTO_INCREMENT caches.
  stoolap_thd_inspect.{h,cc} The few readers of THD / LEX internals
                            (FOREIGN KEYs from the parser, ALTER renames).
  stoolap_packet.h          Parsers for Stoolap's packed result buffers.
third_party/                Vendored wsrep headers MariaDB's headers need
                            (see NOTICE).
tests/                      Python integration harness (see Testing).
benchmarks/bench.py         STOOLAP vs InnoDB dashboard (see Performance).
```

### Whole-statement pushdown

| MariaDB hook | Statement | What runs in Stoolap |
| --- | --- | --- |
| `select_handler` | SELECT | The whole query, when every table is a Stoolap table and every expression translates. |
| `derived_handler` | FROM subquery | The derived table, so an InnoDB query can join a Stoolap result. |
| unit handler | UNION / INTERSECT / EXCEPT [ALL] | The whole unit, when every leg is Stoolap-only. |
| `direct_update_rows` / `direct_delete_rows` (and `cond_push`) | UPDATE / DELETE | The statement, from its pushed condition and SET list (see below). |

`stoolap_sqlgen` writes the Stoolap SQL from MariaDB's Item tree, so
each construct is translated or the statement stays with MariaDB:
- NULL ordering is explicit (`ASC NULLS FIRST` / `DESC NULLS LAST`),
  `<=>` becomes `IS NOT DISTINCT FROM`, literals are single-quoted, and
  a prepared statement's parameters are written as their bound values.
- `CONCAT` becomes `||`, an integer branch of a DOUBLE `IF` / `CASE` /
  `COALESCE` is cast to FLOAT, a GROUP BY without ORDER BY is ordered by
  its keys as MariaDB does, and an EXISTS subquery selects `1`.
- UNION / INTERSECT / EXCEPT keep their written order (both engines
  apply INTERSECT first); a deduplicating operation pushes only when
  every result column compares the same way in both engines.
- Comparisons, GROUP BY, ORDER BY, DISTINCT and MIN / MAX push only for
  types that compare the same way in both engines: integers, doubles,
  DATE / DATETIME / TIME / TIMESTAMP, and utf8 strings under a binary
  NO PAD collation (or any collation with
  `stoolap_trust_binary_strings = 1`).
- A DATE / DATETIME or TIME column against a string constant pushes with
  the constant converted the way MariaDB's comparison would, unless it
  parses with a warning or has a zero date part.
- Integer `/`, DECIMAL arithmetic, `LENGTH`, `ROUND`, `SUBSTRING`, CAST,
  `TRIM`, `UPPER` / `LOWER`, `WITH TIES`, `SQL_CALC_FOUND_ROWS` and other
  constructs without an exact equivalent are not pushed.

With `SET stoolap_explain_pushdown = 1` the error log shows the pushed
SQL with Stoolap's plan, and why a query was not pushed.

### Direct UPDATE and DELETE

A single-table UPDATE or DELETE runs as one Stoolap statement, written
from the condition MariaDB pushes and the SET list, inside the session's
transaction. It keeps what MariaDB does around each row:
- **Rows changed and rows found.** Only the rows a SET changes are
  written and counted as affected; "Rows matched" counts the rows the
  condition found (a client with `CLIENT_FOUND_ROWS` gets those as its
  affected rows).
- **Strict mode.** A value too large or too long for its column fails
  with 1264 / 1406, as MariaDB's would, and NULL into NOT NULL with 1048.

What it cannot do the same way stays on MariaDB's row-by-row path (the
row pump), which applies MariaDB's own rules: a table with CHECK
constraints or `ON UPDATE CURRENT_TIMESTAMP` columns, an UPDATE of a
PRIMARY KEY or UNIQUE column (MariaDB checks each row's keys as it
writes it, and fails the statement, not the COMMIT), a value that needs
conversion to its column's type, a target MariaDB rounds or checks
specially (FLOAT, `DOUBLE(M,D)`, YEAR, CHAR, dates and times), any value
that may not fit outside strict mode, a SET value reading a column the
same SET assigned before it (MariaDB reads the new value, Stoolap the old
row), `DELETE IGNORE`, ORDER BY / LIMIT, multi-table statements, triggers
and row-based binary logging.

DELETE without WHERE is such a statement too: inside the transaction,
with the foreign keys' ON DELETE actions. `TRUNCATE TABLE` runs Stoolap's
TRUNCATE, resets AUTO_INCREMENT and refuses a table a foreign key
references.

### Value storage

Each column is stored so that Stoolap's comparisons and order match
MariaDB's (`src/stoolap_types.h`):

| MariaDB type | Stoolap column | Value |
| --- | --- | --- |
| DATE, DATETIME, TIME | INTEGER | MariaDB's `pack_time()` value: years 0000-9999, zero and partial dates, negative and >99 h times; a DATE is the DATETIME at midnight |
| TIMESTAMP | INTEGER | UTC microseconds since the epoch, independent of `time_zone` |
| ENUM / SET | INTEGER | the index / bit mask MariaDB sorts by |
| BINARY / VARBINARY / BLOB, and text in latin1 etc. | TEXT | uppercase hex of the bytes, which keeps their order |
| DECIMAL | TEXT | the decimal string |
| integers, doubles, utf8 text | INTEGER / FLOAT / TEXT | as is |

A pushed query or a direct UPDATE that would compare or compute with
the stored form of a DECIMAL or hex-encoded column stays with MariaDB.
Tables created by an older build of the plugin must be re-created.

## Feature matrix

### DDL

| Feature | Status |
| --- | --- |
| Numeric, string, BLOB, DATE / TIME / DATETIME / TIMESTAMP, DECIMAL, ENUM / SET, JSON columns | Yes (stored as in [Value storage](#value-storage)) |
| Character sets other than utf8 | Yes (stored as hex; not pushed) |
| `PRIMARY KEY`, `UNIQUE`, single-column or composite | Yes |
| `KEY` / `CREATE INDEX`, single-column or composite | Yes (one Stoolap index over all parts) |
| `PRIMARY KEY` / `UNIQUE` on a case-insensitive collation | Refused (Stoolap compares bytes: use a `_bin` collation) |
| `PRIMARY KEY` / `UNIQUE` over a column prefix (`a(3)`) | Refused (Stoolap enforces whole values) |
| `FOREIGN KEY` | Yes: single-column, to a PRIMARY KEY or UNIQUE column; RESTRICT, NO ACTION, CASCADE, SET NULL |
| Composite or self-referencing `FOREIGN KEY` | Refused |
| FOREIGN KEYs through a copying `ALTER TABLE` | Kept, following renamed columns; dropping a key column is refused |
| `DROP TABLE` of a referenced parent | Refused with 1451, as InnoDB does (not with `foreign_key_checks = 0`); `DROP DATABASE` drops its own children first |
| `RENAME TABLE` | Yes (children's FOREIGN KEYs follow the parent) |
| `ALTER TABLE` ADD / DROP / CHANGE COLUMN, ADD / DROP INDEX | Yes (copying ALTER) |
| `TRUNCATE TABLE` | Yes |
| `AUTO_INCREMENT` | Yes (see [Known differences](#known-differences)) |
| Generated columns (VIRTUAL / STORED) | Refused (1910) |
| `COMPRESSED` BLOB / TEXT | Refused (`COMPRESSED` VARCHAR works) |
| Table names past Stoolap's 255-byte limit | Yes (shortened with a hash) |

### DML

| Feature | Status |
| --- | --- |
| `INSERT`, multi-row `INSERT`, `INSERT ... SELECT` | Yes (multi-row inserts batched, UNIQUE keys checked once per batch) |
| `INSERT ... ON DUPLICATE KEY UPDATE`, `REPLACE`, `INSERT IGNORE` | Yes (row by row, so MariaDB drives each duplicate) |
| `UPDATE` / `DELETE` | Yes (direct when possible, see [Direct UPDATE and DELETE](#direct-update-and-delete)) |
| `UPDATE` / `DELETE` with `ORDER BY` / `LIMIT`, multi-table | Yes (row pump) |
| `LAST_INSERT_ID()` | Yes |

### Queries

| Feature | Status |
| --- | --- |
| Whole SELECT, derived table and set-operation pushdown | Yes (constructs with an exact translation) |
| Joins (INNER, LEFT, RIGHT, CROSS, self), subqueries (scalar, IN, EXISTS, NOT IN / NOT EXISTS), CTEs | Yes |
| Aggregates, GROUP BY, HAVING, window functions and frames | Yes |
| Index range scans, MIN / MAX through `index_first` / `index_last` | Yes |
| Prepared statements (text and binary protocol) | Yes |
| `EXPLAIN` | Shows `PUSHED SELECT` / `PUSHED DERIVED` / `PUSHED UNION` |

### Transactions

| Feature | Status |
| --- | --- |
| `BEGIN` / `COMMIT` / `ROLLBACK`, autocommit | Yes |
| A failed statement inside a transaction | Undone as a whole; the transaction goes on |
| `READ COMMITTED` | Yes (`READ UNCOMMITTED` runs as READ COMMITTED; Stoolap is MVCC) |
| `REPEATABLE READ`, `SERIALIZABLE` | Yes (a Stoolap snapshot transaction) |
| `START TRANSACTION WITH CONSISTENT SNAPSHOT` | Yes |
| `SAVEPOINT` / `RELEASE SAVEPOINT` / `ROLLBACK TO SAVEPOINT` | Yes |
| Write-write conflicts | `ER_LOCK_DEADLOCK` (1213) |
| Crash recovery (`file://` DSN) | Yes (WAL replay at open) |
| XA / two-phase commit | No |

### Errors

Each Stoolap error is classified from its error value (`rust/src/ffi/error.rs`,
by its wording only where no value is kept) and raised as MariaDB's own:

| Stoolap error | MariaDB error |
| --- | --- |
| PRIMARY KEY / UNIQUE violation | `ER_DUP_ENTRY` 1062, naming the key |
| Missing parent row | `ER_NO_REFERENCED_ROW_2` 1452 |
| Parent row or table still referenced | `ER_ROW_IS_REFERENCED_2` 1451 |
| Write conflict, lock, schema change mid-statement | `ER_LOCK_DEADLOCK` 1213 |
| Table exists / table not found | 1050 / 1146 |
| Anything else | 1296 with Stoolap's message |

An error found at COMMIT is raised with the same class. A message no
class covers increments `Stoolap_unmapped_errors`.

## Known differences

Differences from InnoDB that remain. Each has a scenario in `tests/diff/`
marked `-- known-diff-file:`, which fails once the scenario matches, so
the list stays current.

- **AUTO_INCREMENT** is recomputed from `MAX(col) + 1` after DDL;
  `ALTER TABLE ... AUTO_INCREMENT = N` is ignored; deleted top ids are
  reused and an explicit value moves the counter.
- **`utf8mb4_bin` UNIQUE** ignores PAD SPACE (`'a'` and `'a '` are both
  accepted).
- **A FOREIGN KEY to an indexed non-unique column** is refused.
- **`HA_READ_PREFIX_LAST`** (MAX with a prefix bound) is not supported.
- **`LOCK TABLES` with `autocommit = 0`** runs the statements after
  COMMIT in autocommit.

Also:
- **Case-insensitive collations** compare in MariaDB, not in Stoolap: a
  query or UPDATE that compares such strings stays with MariaDB, unless
  `stoolap_trust_binary_strings = 1`.
- **"Rows matched"** of a direct UPDATE comes from a count before the
  UPDATE; in autocommit, a commit between the two can make it off
  ([stoolap#936](https://github.com/stoolap/stoolap/issues/936)).
  Affected rows are exact.
- **A strict-mode range or length error** from a direct UPDATE names row
  1, since the failing row is not known.
- **Tables a FOREIGN KEY names** are written through SQL, so Stoolap's
  executor checks the keys: slower than the direct write path.
- **`ALTER TABLE` on `file://` storage** loses the table's rows at the
  next restart when the table was already on disk (after a restart or a
  checkpoint): Stoolap replays the ALTER's renames wrongly
  ([stoolap#941](https://github.com/stoolap/stoolap/issues/941)).
  `case_22_restart` reports it as known until the fix lands.

## Build

### Prerequisites

1. **MariaDB 11.4 server headers.** `cmake/FindMariaDB.cmake` finds the
   common layouts:

   | Platform | Install | Found at |
   | --- | --- | --- |
   | macOS (Homebrew) | `brew install mariadb@11.4` | `/opt/homebrew/opt/mariadb@11.4` (Apple Silicon) or `/usr/local/opt/mariadb@11.4` (Intel) |
   | Debian / Ubuntu | `sudo apt-get install mariadb-server libmariadbd-dev`, from [MariaDB's repository](https://r.mariadb.com/downloads/mariadb_repo_setup) | `/usr/include/mysql/server/private/` |
   | Red Hat / Fedora | `sudo dnf install mariadb-server mariadb-server-devel` | `/usr/include/mariadb/server/private/` |
   | Source build | `cmake --install build --prefix "$PREFIX"`, then `-DMariaDB_ROOT="$PREFIX"` | `$PREFIX/include/mysql/server/private/` |

   It also tries `mariadb_config --prefix`. Pass `-DMariaDB_ROOT=<path>`
   to override (`MARIADB_PREFIX` is accepted too).

2. **A stable Rust toolchain** (`cargo`). The build fetches Stoolap from
   git at the commit pinned in `rust/Cargo.lock`;
   `cargo update --manifest-path rust/Cargo.toml -p stoolap` moves it to
   main's head. CI does that nightly, so upstream changes that break the
   crate show up there first.

### Compile

```sh
cmake -S . -B build
cmake --build build -j
```

The output is `build/ha_stoolap.so` (`.so` on macOS too: MariaDB's
plugin loader looks for that suffix). Pass `-DSTOOLAP_DIR=../stoolap`
to build against a local Stoolap checkout.

### Build flags

Set in `CMakeLists.txt`; worth knowing when something fails to load:
- `DBUG_OFF` and `NDEBUG`: release `mariadbd` binaries do not export
  the `DBUG_*` symbols.
- No `-fvisibility=hidden`: `maria_declare_plugin` emits its symbols
  without default visibility, and the server would not find them.
- No `-fno-rtti`: MariaDB's `field.h` uses `dynamic_cast`.
- `ha_stoolap.cc` does not include `sql_class.h`; the files that read
  THD / LEX internals do, with the wsrep headers it needs vendored under
  `third_party/` (Homebrew's MariaDB does not ship them).
- Server symbols resolve when `mariadbd` loads the plugin
  (`-undefined dynamic_lookup` on macOS,
  `--unresolved-symbols=ignore-in-shared-libs` on Linux); on macOS the
  `.so` is ad-hoc signed, which dyld requires.

## Install

Stoolap is linked into `ha_stoolap.so`, so it is the only file to copy:

```sh
sudo install -m 0755 build/ha_stoolap.so "$(mariadb_config --plugindir)/"
```

The plugin directory is `/opt/homebrew/opt/mariadb@11.4/lib/plugin` on
Homebrew, `/usr/lib/mysql/plugin` on Debian / Ubuntu and
`/usr/lib64/mysql/plugin` on Red Hat / Fedora (CMake reports it as
`MariaDB_PLUGIN_DIR`).

The plugin is alpha and the server loads GAMMA and above by default, so
start `mariadbd` with `--plugin-maturity=alpha`, then:

```sql
INSTALL PLUGIN stoolap SONAME 'ha_stoolap.so';
SHOW ENGINES;                      -- STOOLAP is listed
CREATE TABLE t (id INT PRIMARY KEY, v INT) ENGINE=STOOLAP;
```

### Configuration

| Variable | Scope | Default | Effect |
| --- | --- | --- | --- |
| `stoolap_dsn` | startup only (`--stoolap-dsn=...`) | `memory://` | Stoolap's DSN; `file:///path` for persistent storage. |
| `stoolap_perf_trace` | global | `OFF` | Fill the `Stoolap_perf_*` timing counters (costs ~50 ns per pushed row). |
| `stoolap_trust_binary_strings` | session | `OFF` | Push string comparisons, sorts and groups on any collation as byte comparisons. Only for data where that is the same (ASCII, one case). |
| `stoolap_explain_pushdown` | session | `OFF` | Log each pushed SQL with Stoolap's plan, and why a query was not pushed. |

### Status counters (`SHOW STATUS LIKE 'Stoolap_%'`)

| Name | Meaning |
| --- | --- |
| `Stoolap_pushdown_hits` / `Stoolap_pushdown_misses` | Queries the pushdown handlers took / left to MariaDB. |
| `Stoolap_direct_modify_hits` | UPDATE / DELETE statements that ran directly. |
| `Stoolap_records_live_counts` | Row counts the record-count caches could not answer. |
| `Stoolap_buffered_scans` / `Stoolap_buffered_rows` | Direct table reads and the rows they delivered. |
| `Stoolap_unmapped_errors` | Stoolap errors no class covers (see `case_18_error_mapping`). |
| `Stoolap_perf_*` | Pushdown timings, filled with `stoolap_perf_trace = ON`. |

## Testing

```sh
pip install mysql-connector-python      # Python 3.13 (CI's; 3.15 breaks the connector)

python3 tests/runner.py                 # every tests/cases/case_*.py
python3 tests/runner.py 06_pushdown     # cases whose name contains this
KEEP_RUNNING=1 python3 tests/runner.py  # leave the server up afterwards
REUSE_RUNNING=1 python3 tests/runner.py # reuse a server left up
STOOLAP_DSN=file:///tmp/stoolap-test-stoolap-py python3 tests/runner.py
                                        # against persistent storage
STOOLAP_PLUGIN=/path/ha_stoolap.so python3 tests/runner.py
                                        # test another build
```

The runner starts a private `mariadbd` (`--no-defaults`, datadir
`/tmp/stoolap-test-data`, socket `/tmp/stoolap-test.sock`), loads the
plugin and runs each case file in its own database. `--no-defaults`
matters: a system `my.cnf` can carry options this server rejects.

`case_22_restart` starts a second server of its own on `file://`
storage, stops it cleanly and kills it, and checks that committed rows,
indexes, FOREIGN KEYs and AUTO_INCREMENT survive and uncommitted ones do
not.

`case_21_differential` runs every `tests/diff/*.sql` scenario statement
by statement on STOOLAP and on InnoDB and requires the same rows, errno
and affected-row count, then the same table contents. A `-- known-diff`
note marks an accepted difference and fails once it no longer differs.
To add a regression for a difference from InnoDB, add a scenario there.

## Performance

`benchmarks/bench.py` runs the same workload on STOOLAP and InnoDB under
one server (10K rows; the shapes of Stoolap's own
`examples/benchmark.rs`). One run on an Apple M5, MariaDB 11.4.13,
in-memory DSN, Stoolap won 41 operations and InnoDB 1. Some of them:

| Operation | STOOLAP | InnoDB | Ratio |
| --- | ---: | ---: | ---: |
| Nested subquery (3 levels) | 148 us | 8907 us | 60x |
| Compare with subquery | 128 us | 3818 us | 30x |
| Window ROW_NUMBER (PK) | 64 us | 1589 us | 25x |
| NOT IN subquery | 141 us | 3122 us | 22x |
| UNION ALL | 68 us | 1185 us | 17x |
| INNER JOIN | 105 us | 1006 us | 9.6x |
| Aggregation (GROUP BY) | 154 us | 1030 us | 6.7x |
| Batch INSERT (100 rows) | 288 us | 513 us | 1.8x |
| INSERT single | 16 us | 23 us | 1.4x |
| UPDATE by ID | 20 us | 21 us | 1.1x |
| SELECT by ID | 27 us | 25 us | 0.95x |

Analytical shapes (joins, subqueries, window functions, aggregates) are
where Stoolap's executor pulls ahead. Point lookups and writes are close
to InnoDB: there MariaDB's own per-statement work dominates. InnoDB's one
win is "UPDATE complex" (a range UPDATE of a random float balance,
458 us against 308 us). See
`benchmarks/README.md` for the full dashboard and its baseline check.

## Licensing

- The plugin (`src/`, `rust/`, the build files) is
  [Apache License 2.0](LICENSE).
- The vendored headers under `third_party/` are GPL-2.0 (see their
  `COPYING` files); they are needed only at compile time and ship
  unmodified.
- MariaDB Server is GPL-2.0 with a
  [FOSS License Exception](https://mariadb.com/foss-license-exception/)
  that permits Apache-2.0 plugins to link against it.
- Stoolap is Apache-2.0.

See [NOTICE](NOTICE) for the full attribution.
