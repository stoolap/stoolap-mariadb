# stoolap-mariadb tests

Python integration tests that run against a freshly built `ha_stoolap.so`.
The runner spawns a private `mariadbd` (datadir under `/tmp/stoolap-test-data`,
socket at `/tmp/stoolap-test.sock`) so test runs don't disturb any existing
MariaDB instance. Each case file owns its own database and connects via
`mysql.connector`.

## Layout

```
tests/
  runner.py            # entry point. starts mariadbd, runs every cases/case_*.py, stops mariadbd
  lib/harness.py       # Harness class + assertion helpers
  cases/
    case_01_crud.py                  INSERT/SELECT/UPDATE/DELETE/TRUNCATE/ALTER
    case_02_indexes.py               PRIMARY/UNIQUE/secondary/composite, range scans, ORDER BY
    case_03_transactions.py          BEGIN/COMMIT/ROLLBACK, autocommit, write conflict
    case_04_odku_replace.py          ON DUPLICATE KEY UPDATE / REPLACE / AUTO_INCREMENT
    case_05_foreign_keys.py          CREATE-time FK, ALTER ADD CONSTRAINT, INSERT/UPDATE checks
    case_06_pushdown.py              select_handler/derived_handler/unit_handler pushdown
    case_07_aggregations_joins.py    aggregates / GROUP BY / subqueries / CTE / JOIN / UNION
    case_08_edge_cases.py            NULLs / boundaries / dates / TEXT / DEFAULT
    case_09_strings.py               LIKE / IN / BETWEEN / CONCAT / SUBSTRING / TRIM
    case_10_numerics.py              ABS / ROUND / MOD / unsigned round-trip
    case_11_concurrent.py            multi-session visibility, isolation, parallel DDL
    case_13_ddl_surface.py           column types / DEFAULT / RENAME / SHOW CREATE
    case_14_scale.py                 50K/120K row loads, parity vs InnoDB
    case_15_error_messages.py        error-message plumbing
    case_16_tx_isolation.py          REPEATABLE READ snapshot, READ COMMITTED post-commit visibility, CONSISTENT SNAPSHOT
    case_17_unsupported_ddl.py       composite PK/UNIQUE/FK refused at CREATE; direct UPDATE 1062/1452 strict errno
    case_18_error_mapping.py         every STOOLAP_ERR_* round-trips to the right MariaDB errno; drift counters stay at baseline
    case_19_savepoints.py            SAVEPOINT / RELEASE / ROLLBACK TO, nested + reused names, unknown-name error
```

## Requirements

```sh
pip install mysql-connector-python
```

## Running

Build the plugin first, then run the full suite:

```sh
cmake --build build
python3 tests/runner.py
```

Run a single case file by name (substring match against the basename):

```sh
python3 tests/runner.py pushdown
python3 tests/runner.py 02
```

## Useful environment overrides

| Variable          | Purpose                                                                  |
|-------------------|--------------------------------------------------------------------------|
| `MARIADB_PREFIX`  | Path to the MariaDB install (defaults to `/opt/homebrew/opt/mariadb@11.4`) |
| `MARIADBD_BIN` / `MARIADB_BIN` / `MARIADB_INSTALL_DB_BIN` | Override MariaDB binary paths for distro layouts |
| `MARIADB_PLUGIN_DIR` | Override plugin directory when it is not `$MARIADB_PREFIX/lib/plugin` |
| `STOOLAP_PLUGIN`  | Plugin path to install (defaults to `build/ha_stoolap.so`)                |
| `STOOLAP_DSN`     | Stoolap-side DSN. Default `memory://`. Set `file:///tmp/stoolap-test-stoolap` for an apples-to-apples InnoDB comparison |
| `KEEP_RUNNING=1`  | Leave `mariadbd` up after tests so you can poke at the data manually     |
| `REUSE_RUNNING=1` | Reuse an already-running server at `/tmp/stoolap-test.sock`              |

For fast iteration: leave a server running once with `KEEP_RUNNING=1`, then
re-run with `REUSE_RUNNING=1` until you need to swap in a fresh build of
the plugin.

## Adding a case

Drop a `case_NN_name.py` into `cases/` that defines a top-level `run(harness)`:

```python
def run(h):
    h.exec_script(f"""
CREATE TABLE t (id INT PRIMARY KEY) ENGINE={h.engine};
INSERT INTO t VALUES (1);
""")
    h.section("My checks")
    h.assert_ok("insert ok",      "INSERT INTO t VALUES (2)")
    h.assert_err("dup rejected",  "INSERT INTO t VALUES (1)", r"Duplicate")
    h.assert_scalar("row count",  "SELECT COUNT(*) FROM t", "2")
```

The runner gives each case a fresh database (default name derived from the
file stem, e.g. `case_06_pushdown` -> `stoolap_test_06_pushdown`). The
`Harness` exposes:

- `assert_ok / assert_err / assert_scalar / assert_eq`
- `assert_pushed / assert_not_pushed / assert_pushed_union / assert_pushed_derived`
- `sql(stmt)` - tab/newline-joined result text (`mariadb -ss` shape)
- `exec_script(sql)` - multi-statement bootstrap via the `mariadb` client
- `run_client(script)` - synchronous mariadb subprocess (returns rc, output)
- `run_async(script)` - background mariadb subprocess for concurrent cases
- `sql_with_session([SET ...], stmt)` - run on a fresh connection with session vars
- `section(name)` - print a section banner

The runner snapshots `SHOW STATUS LIKE 'Stoolap_%'` before and after each case.
Normal activity counters are allowlisted centrally. `Stoolap_unmapped_errors`
is a strict drift counter -- non-zero means the typed-error map and the
prose-grep fallback both gave up on a real error wording, which is a
plugin-side regression. `Stoolap_typed_fallback_hits` is allowlisted because
it signals a stoolap-side typed-error gap, not a plugin regression: the
plugin still produces the right SQLSTATE via the prose fallback, and each
occurrence emits a `[Note] stoolap typed-error gap: code=N msg='...'` log
line so the wording can be filed upstream. If a case intentionally moves
some other non-activity counter, declare
`STOOLAP_COUNTERS_ALLOW_DELTA = {"CounterName"}` next to its `run(harness)`
function.
