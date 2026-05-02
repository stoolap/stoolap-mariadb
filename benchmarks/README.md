# benchmarks/

Performance dashboard for the plugin. Mirrors the workload of the
in-tree Stoolap reference benchmark
(`stoolap/examples/benchmark.rs`) so we can see what overhead the
MariaDB handler layer imposes versus the embedded engine.

## What it measures

Same dataset shape on both engines (10K users, 30K orders by default),
same iteration tiers (500 / 250 / 50, mirroring Rust's
`ITERATIONS / ITERATIONS_MEDIUM / ITERATIONS_HEAVY`). Per-iteration
timings let us report:

- **min**: best-case latency (warm cache, no contention)
- **median**: the typical case
- **p95**: tail latency
- **ops/s**: thoughput derived from the median

Each operation also has its EXPLAIN captured once. Cases where STOOLAP
is slower than InnoDB get their EXPLAIN dumped at the end so we can
see *why* (whole-SELECT pushdown vs handler row pump, filesort, etc.).

## Running

You need a running test mariadbd with the plugin loaded. The repo's
test runner does this:

```sh
# Brings up an isolated mariadbd on /tmp/stoolap-test.sock and leaves
# it running for repeated runs.
KEEP_RUNNING=1 tests/run_all.sh 14_scale
```

Then:

```sh
python3 benchmarks/bench.py
python3 benchmarks/bench.py --rows 50000 --iterations 1000
python3 benchmarks/bench.py --csv /tmp/raw.csv          # write samples
```

Tear down the test server when done (or just `kill` the PID it
printed):

```sh
rm -f /tmp/stoolap-test.sock /tmp/stoolap-test.pid
```

## Loss-budget CI check

The bench can compare the current run against a committed baseline
(`benchmarks/baseline.json`). It exits non-zero if any operation
breaks one of two budgets:

- **Win regression**: an operation where STOOLAP was faster than InnoDB
  in the baseline now has a STOOLAP median that grew by more than
  `--win-regress-pct` percent (default: 15%).
- **Loss worsened**: an operation where STOOLAP was slower than InnoDB
  in the baseline now has a worse `InnoDB / STOOLAP` ratio by more
  than `--loss-worsen-pct` percent (default: 10%). A loss recovering
  toward parity is *not* an alert.

```sh
# Regenerate the baseline (commit the result):
python3 benchmarks/bench.py --write-baseline benchmarks/baseline.json

# CI gate:
python3 benchmarks/bench.py --check benchmarks/baseline.json
# exits 0 on OK, 1 on alert, 2 on missing baseline
```

### Reducing CI flakiness

Sub-50µs operations are noise-sensitive on shared hosts. Two knobs to
tune the gate to your runner:

- **`--runs N`**: repeat the whole bench `N` times and keep the best
  median per (engine, op). One scheduler hiccup in run 2 doesn't tank
  the budget if runs 1 and 3 came in clean. Recommended for CI on
  shared / cloud runners (e.g. GitHub Actions). Cost: linear in `N`.
- **`--mode warn`**: prints the alerts but exits 0. The same diagnostic
  output as `fail` mode, just non-blocking. Recommended on noisy hosts
  where you want visibility without blocking merges.

Pick by host:

| Host                        | Recommended gate                                         |
| --------------------------- | -------------------------------------------------------- |
| Dedicated perf runner       | `--runs 1 --mode fail` (the strict default)              |
| Self-hosted, light load     | `--runs 3 --mode fail`                                   |
| Shared CI (GH Actions, ...) | `--runs 3 --mode warn` plus a separate scheduled run     |
|                             | on a dedicated host that gates merges                    |

Example CI invocation for a moderately noisy host:

```sh
python3 benchmarks/bench.py \
  --runs 3 \
  --check benchmarks/baseline.json \
  --mode warn
```

## Diagnosing a slow query (stoolap-side EXPLAIN)

MariaDB's `EXPLAIN` reports `PUSHED SELECT` with NULL columns when a
query is whole-pushed: it's *handed* to the engine, but the plan is
opaque to the server. The plugin exposes a session var that surfaces
stoolap's own plan:

```sql
SET stoolap_explain_pushdown = 1;
-- run any SELECT that pushes; e.g.
WITH a AS (SELECT * FROM t WHERE x < 5),
     b AS (SELECT * FROM t WHERE y > 50)
SELECT a.k, b.k FROM a INNER JOIN b ON a.k = b.k;
```

Each pushed query writes its plan to the server error log
(`/tmp/stoolap-test.err` for the test runner, or the standard
`mariadbd` log path otherwise). Sample output:

```
[Note] stoolap[explain]: WITH a AS (SELECT * FROM t WHERE x < 5)...
[Note] stoolap[explain]:   SELECT
[Note] stoolap[explain]:     -> Hash Join (build: left) (INNER Join)
[Note] stoolap[explain]:        Join Cond: (a.k = b.k)
[Note] stoolap[explain]:       -> Subquery Scan AS a
[Note] stoolap[explain]:           -> Index Scan using t_idx_x   Cond: x < 5
[Note] stoolap[explain]:       -> Subquery Scan AS b
[Note] stoolap[explain]:           -> Seq Scan on t   Filter: y > 50
```

The flag is OFF by default; enabling it adds one extra `stoolap_query`
call per pushed SELECT, so leave it off for benchmarks.

## Dependencies

- Python 3.9+
- `mysql-connector-python` (`pip install mysql-connector-python`)

## Output

Three blocks:

1. **Side-by-side table.** One row per operation. STOOLAP and InnoDB
   columns each show `min/med/p95`. The `med ratio` column shows
   `InnoDB-median / STOOLAP-median`; `>1.00x` means STOOLAP is faster,
   `<1.00x` means InnoDB is faster.
2. **Loss-case detail.** For every row where STOOLAP lost, the median
   gap and EXPLAIN output for both engines. This is the fastest way
   to see whether pushdown engaged, what access type each engine
   chose, and whether MariaDB is doing per-row Item evaluation in
   user space.
3. **Optional CSV.** With `--csv path.csv`, every individual sample
   is written for offline analysis.

## CSV schema

```
operation, engine, iter, us
"SELECT by ID", "STOOLAP", 0, 31.123
...
```

`us` is microseconds from `time.perf_counter()` around `cursor.execute`
(plus `fetchall` for read queries).

## Reproducibility

The data is generated from a fixed RNG seed (`--seed`, default `0xC0FFEE`),
so STOOLAP and InnoDB see byte-identical rows. Plugin behaviour is
mostly deterministic; macOS scheduler noise dominates the `min/med`
spread for sub-50µs queries. Re-running 3-5 times and looking at the
median across runs is more reliable than any single run.
