#!/usr/bin/env python3
# Copyright 2026 Stoolap Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
"""Side-by-side performance dashboard: STOOLAP vs InnoDB.

Mirrors the workload of `stoolap/examples/benchmark.rs` so that the
plugin's overhead vs. the embedded engine is measurable. Same schema,
same row count, same iteration tiers.

Each operation is timed per-iteration (after a warmup) so we can report
min / median / mean / p95 instead of just average. EXPLAIN output is
captured for every operation; cases where STOOLAP loses to InnoDB get
their EXPLAIN dumped at the end so we can see *why*.

Connects to the running test mariadbd at /tmp/stoolap-test.sock by
default. Easiest way to bring one up:

    KEEP_RUNNING=1 tests/run_all.sh 14_scale   # starts on the socket

Then:

    python3 benchmarks/bench.py
    python3 benchmarks/bench.py --csv out.csv
    python3 benchmarks/bench.py --rows 50000 --iterations 200
"""

import argparse
import csv
import json
import os
import random
import statistics
import sys
import time
from typing import Callable, List, Tuple

import mysql.connector


# --------------------------------------------------------------------------
# Defaults match stoolap/examples/benchmark.rs.
# --------------------------------------------------------------------------
ROW_COUNT_DEFAULT     = 10_000
ITERATIONS_DEFAULT    = 500    # point queries
ITERATIONS_MEDIUM     = 250    # index scans, aggregations
ITERATIONS_HEAVY      = 50     # full scans, joins
WARMUP                = 10
DB                    = "bench_cmp"
SOCKET_DEFAULT        = "/tmp/stoolap-test.sock"

# Each entry: (display label, MariaDB engine name, table-suffix tag).
ENGINES = [
    ("STOOLAP", "STOOLAP", "stoolap"),
    ("InnoDB",  "InnoDB",  "innodb"),
]

# Section grouping mirrors stoolap-go/example/benchmark/main.go so the
# rendered output is directly comparable. Each section is a list of
# (operation_label,) tuples; the operations are populated by run_engine
# and the section bucket determines their position in the report.
SECTIONS = [
    ("CORE OPERATIONS", [
        "SELECT by ID",
        "SELECT by index (exact)",
        "SELECT by index (range)",
        "SELECT complex",
        "SELECT * (full scan)",
        "UPDATE by ID",
        "UPDATE complex",
        "INSERT single",
        "DELETE by ID",
        "DELETE complex",
        "Aggregation (GROUP BY)",
    ]),
    ("ADVANCED OPERATIONS", [
        "INNER JOIN",
        "LEFT JOIN + GROUP BY",
        "Scalar subquery",
        "IN subquery",
        "EXISTS subquery",
        "CTE + JOIN",
        "Window ROW_NUMBER",
        "Window ROW_NUMBER (PK)",
        "Window PARTITION BY",
        "UNION ALL",
        "CASE expression",
        "Complex JOIN+GROUP+HAVING",
        "Batch INSERT (100 rows)",
    ]),
    ("BOTTLENECK HUNTERS", [
        "DISTINCT (no ORDER)",
        "DISTINCT + ORDER BY",
        "COUNT DISTINCT",
        "LIKE prefix (User_1%)",
        "LIKE contains (%50%)",
        "OR conditions (3 vals)",
        "IN list (7 values)",
        "NOT IN subquery",
        "NOT EXISTS subquery",
        "OFFSET pagination (5000)",
        "Multi-col ORDER BY (3)",
        "Self JOIN (same age)",
        "Multi window funcs (3)",
        "Nested subquery (3 lvl)",
        "Multi aggregates (6)",
        "COALESCE + IS NOT NULL",
        "Expr in WHERE (funcs)",
        "Math expressions",
        "String concat (||)",
        "Large result (no LIMIT)",
        "Multiple CTEs (2)",
        "Correlated in SELECT",
        "BETWEEN (non-indexed)",
        "GROUP BY (2 columns)",
        "CROSS JOIN (limited)",
        "Derived table (FROM sub)",
        "Window ROWS frame",
        "HAVING complex",
        "Compare with subquery",
    ]),
]


# --------------------------------------------------------------------------
# Result aggregation
# --------------------------------------------------------------------------
class Result:
    """Per-(engine, op) stats. Times stored as microseconds."""
    __slots__ = ("op", "engine", "samples", "explain")

    def __init__(self, op: str, engine: str):
        self.op      = op
        self.engine  = engine
        self.samples: List[float] = []
        self.explain: str = ""

    def add(self, us: float):
        self.samples.append(us)

    def stat(self, fn) -> float:
        return fn(self.samples) if self.samples else 0.0

    @property
    def min_us(self):    return min(self.samples) if self.samples else 0.0
    @property
    def median_us(self): return statistics.median(self.samples) if self.samples else 0.0
    @property
    def mean_us(self):   return statistics.fmean(self.samples) if self.samples else 0.0
    @property
    def p95_us(self):
        if not self.samples:
            return 0.0
        # Statistics.quantiles needs at least two points.
        if len(self.samples) < 2:
            return self.samples[0]
        # n=20 then take the 19th decile -> the 95th percentile.
        return statistics.quantiles(self.samples, n=20)[18]


# --------------------------------------------------------------------------
# Schema setup
# --------------------------------------------------------------------------
def setup_db(socket: str) -> mysql.connector.connection.MySQLConnection:
    boot = mysql.connector.connect(
        user="root", unix_socket=socket, autocommit=True,
    )
    cur = boot.cursor()
    cur.execute(f"DROP DATABASE IF EXISTS {DB}")
    cur.execute(f"CREATE DATABASE {DB}")
    cur.close()
    boot.close()


def make_conn(socket: str):
    return mysql.connector.connect(
        user="root", unix_socket=socket, autocommit=True, database=DB,
    )


def populate_users(cur, table: str, engine: str,
                    rng: random.Random, row_count: int):
    cur.execute(f"""
        CREATE TABLE {table} (
            id         INT NOT NULL PRIMARY KEY,
            name       VARCHAR(64) NOT NULL,
            email      VARCHAR(64) NOT NULL,
            age        INT NOT NULL,
            balance    DOUBLE NOT NULL,
            active     TINYINT(1) NOT NULL,
            created_at VARCHAR(32) NOT NULL,
            KEY idx_age (age),
            KEY idx_active (active)
        ) ENGINE={engine}
    """)
    rows = []
    for i in range(1, row_count + 1):
        rows.append((
            i,
            f"User_{i}",
            f"user{i}@example.com",
            rng.randint(18, 79),
            rng.uniform(0.0, 100_000.0),
            1 if rng.random() < 0.7 else 0,
            "2024-01-01 00:00:00",
        ))
    CHUNK = 1000
    for i in range(0, len(rows), CHUNK):
        cur.executemany(
            f"INSERT INTO {table} (id, name, email, age, balance, active, "
            "created_at) VALUES (%s,%s,%s,%s,%s,%s,%s)",
            rows[i : i + CHUNK],
        )


def populate_orders(cur, table: str, engine: str,
                    rng: random.Random, user_count: int):
    cur.execute(f"""
        CREATE TABLE {table} (
            id         INT NOT NULL PRIMARY KEY,
            user_id    INT NOT NULL,
            amount     DOUBLE NOT NULL,
            status     VARCHAR(32) NOT NULL,
            order_date VARCHAR(32) NOT NULL,
            KEY idx_user_id (user_id),
            KEY idx_status (status)
        ) ENGINE={engine}
    """)
    statuses = ["pending", "completed", "shipped", "cancelled"]
    rows = []
    for i in range(1, user_count * 3 + 1):
        rows.append((
            i,
            rng.randint(1, user_count),
            rng.uniform(10.0, 1000.0),
            statuses[rng.randint(0, 3)],
            "2024-01-15",
        ))
    CHUNK = 2000
    for i in range(0, len(rows), CHUNK):
        cur.executemany(
            f"INSERT INTO {table} (id, user_id, amount, status, order_date) "
            "VALUES (%s,%s,%s,%s,%s)",
            rows[i : i + CHUNK],
        )


# --------------------------------------------------------------------------
# Timed loop with per-iteration samples
# --------------------------------------------------------------------------
def time_each(cur, sql: str, *,
              iters: int, warmup: int = WARMUP,
              params_seq=None, fetch: bool = True) -> List[float]:
    """Return per-iteration microsecond samples after a warmup pass."""
    for k in range(warmup):
        if params_seq is None:
            cur.execute(sql)
        else:
            cur.execute(sql, params_seq[k % len(params_seq)])
        if fetch:
            cur.fetchall()

    samples = [0.0] * iters
    for k in range(iters):
        if params_seq is None:
            t0 = time.perf_counter()
            cur.execute(sql)
            if fetch:
                cur.fetchall()
        else:
            p = params_seq[k % len(params_seq)]
            t0 = time.perf_counter()
            cur.execute(sql, p)
            if fetch:
                cur.fetchall()
        samples[k] = (time.perf_counter() - t0) * 1_000_000.0
    return samples


def time_callable(fn: Callable[[], None], *,
                   iters: int, warmup: int = 0) -> List[float]:
    for _ in range(warmup):
        fn()
    samples = [0.0] * iters
    for k in range(iters):
        t0 = time.perf_counter()
        fn()
        samples[k] = (time.perf_counter() - t0) * 1_000_000.0
    return samples


def capture_explain(cur, sql: str, params=None) -> str:
    """Run EXPLAIN once and return a one-line summary plus the type/Extra
    column, which is what we usually care about for plan diagnosis."""
    try:
        if params is None:
            cur.execute("EXPLAIN " + sql)
        else:
            cur.execute("EXPLAIN " + sql, params)
        rows = cur.fetchall()
        cols = cur.column_names
    except mysql.connector.Error as e:
        return f"<EXPLAIN failed: {e}>"
    out_lines = []
    for r in rows:
        d = dict(zip(cols, r))
        out_lines.append(
            f"  id={d.get('id')} type={d.get('select_type')} "
            f"table={d.get('table')} access={d.get('type')} "
            f"key={d.get('key')} rows={d.get('rows')} "
            f"extra={d.get('Extra')}"
        )
    return "\n".join(out_lines) if out_lines else "<EXPLAIN empty>"


# --------------------------------------------------------------------------
# Workload definition
# --------------------------------------------------------------------------
def run_engine(socket: str, label: str, engine: str, suffix: str,
                rng: random.Random, args) -> dict:
    users  = f"users_{suffix}"
    orders = f"orders_{suffix}"

    c = make_conn(socket)
    cur = c.cursor()
    cur.execute("SET SESSION sql_mode = 'PIPES_AS_CONCAT,NO_ENGINE_SUBSTITUTION'")
    if engine == "STOOLAP":
        # Mirror the Rust benchmark which has no concept of ci collation.
        cur.execute("SET SESSION stoolap_trust_binary_strings = 1")

    populate_users(cur, users, engine, rng, args.rows)
    populate_orders(cur, orders, engine, rng, args.rows)

    iters_pt   = args.iterations
    iters_med  = max(1, args.iterations // 2)
    iters_heav = max(1, args.iterations // 10)

    out: dict[str, Result] = {}

    def b(name: str, sql: str, *, iters: int = iters_pt,
          params_seq=None, fetch: bool = True, warmup: int = WARMUP):
        r = Result(name, label)
        r.samples = time_each(cur, sql, iters=iters, params_seq=params_seq,
                              fetch=fetch, warmup=warmup)
        sample_params = params_seq[0] if params_seq else None
        r.explain = capture_explain(cur, sql, sample_params)
        out[name] = r

    # ---- core ----
    b("SELECT by ID",
      f"SELECT * FROM {users} WHERE id = %s",
      params_seq=[((i % args.rows) + 1,) for i in range(iters_pt)])

    b("SELECT by index (exact)",
      f"SELECT * FROM {users} WHERE age = %s",
      params_seq=[(((i % 62) + 18),) for i in range(iters_pt)])

    b("SELECT by index (range)",
      f"SELECT * FROM {users} WHERE age >= %s AND age <= %s",
      params_seq=[(30, 40)] * iters_pt)

    b("SELECT complex",
      f"SELECT id, name, balance FROM {users} WHERE age >= 25 AND age <= 45 "
      f"AND active = 1 ORDER BY balance DESC LIMIT 100")

    b("SELECT * (full scan)",
      f"SELECT * FROM {users}", iters=iters_heav)

    b("UPDATE by ID",
      f"UPDATE {users} SET balance = %s WHERE id = %s",
      params_seq=[(rng.uniform(0.0, 100_000.0),
                   ((i % args.rows) + 1)) for i in range(iters_pt)],
      fetch=False)

    b("UPDATE complex",
      f"UPDATE {users} SET balance = %s WHERE age >= %s AND age <= %s "
      f"AND active = 1",
      params_seq=[(rng.uniform(0.0, 100_000.0), 27, 28)
                  for _ in range(iters_pt)],
      fetch=False)

    insert_params = [(args.rows + 1000 + i,
                      f"New_{args.rows + 1000 + i}",
                      f"new{args.rows + 1000 + i}@example.com",
                      rng.randint(18, 79),
                      100.0,
                      1,
                      "2024-01-01 00:00:00") for i in range(iters_pt)]
    b("INSERT single",
      f"INSERT INTO {users} (id, name, email, age, balance, active, "
      "created_at) VALUES (%s,%s,%s,%s,%s,%s,%s)",
      params_seq=insert_params, fetch=False, warmup=0)

    delete_params = [(args.rows + 1000 + i,) for i in range(iters_pt)]
    b("DELETE by ID",
      f"DELETE FROM {users} WHERE id = %s",
      params_seq=delete_params, fetch=False, warmup=0)

    b("DELETE complex",
      f"DELETE FROM {users} WHERE age >= 25 AND age <= 26 AND active = 1",
      fetch=False, warmup=0)

    b("Aggregation (GROUP BY)",
      f"SELECT age, COUNT(*), AVG(balance) FROM {users} GROUP BY age",
      iters=iters_med)

    # ---- advanced ----
    b("INNER JOIN",
      f"SELECT u.name, o.amount FROM {users} u INNER JOIN {orders} o "
      f"ON u.id = o.user_id WHERE o.status = 'completed' LIMIT 100",
      iters=100)

    b("LEFT JOIN + GROUP BY",
      f"SELECT u.name, COUNT(o.id) order_count, SUM(o.amount) total "
      f"FROM {users} u LEFT JOIN {orders} o ON u.id = o.user_id "
      f"GROUP BY u.id, u.name LIMIT 100", iters=100)

    b("Scalar subquery",
      f"SELECT name, balance, (SELECT AVG(balance) FROM {users}) avg_balance "
      f"FROM {users} WHERE balance > (SELECT AVG(balance) FROM {users}) "
      f"LIMIT 100")

    b("IN subquery",
      f"SELECT * FROM {users} WHERE id IN (SELECT user_id FROM {orders} "
      f"WHERE status = 'completed') LIMIT 100",
      iters=10, warmup=2)

    b("EXISTS subquery",
      f"SELECT * FROM {users} u WHERE EXISTS (SELECT 1 FROM {orders} o "
      f"WHERE o.user_id = u.id AND o.amount > 500) LIMIT 100",
      iters=100)

    b("CTE + JOIN",
      f"WITH high_value AS (SELECT user_id, SUM(amount) total FROM {orders} "
      f"GROUP BY user_id HAVING SUM(amount) > 1000) "
      f"SELECT u.name, h.total FROM {users} u INNER JOIN high_value h "
      f"ON u.id = h.user_id LIMIT 100", iters=20, warmup=2)

    b("Window ROW_NUMBER",
      f"SELECT name, balance, ROW_NUMBER() OVER (ORDER BY balance DESC) rnk "
      f"FROM {users} LIMIT 100")

    b("Window ROW_NUMBER (PK)",
      f"SELECT name, ROW_NUMBER() OVER (ORDER BY id) rnk FROM {users} LIMIT 100")

    b("Window PARTITION BY",
      f"SELECT name, age, balance, RANK() OVER "
      f"(PARTITION BY age ORDER BY balance DESC) age_rank "
      f"FROM {users} LIMIT 100")

    b("UNION ALL",
      f"SELECT name, 'high' category FROM {users} WHERE balance > 50000 "
      f"UNION ALL SELECT name, 'low' FROM {users} WHERE balance <= 50000 "
      f"LIMIT 100")

    b("CASE expression",
      f"SELECT name, CASE WHEN balance > 75000 THEN 'platinum' "
      f"WHEN balance > 50000 THEN 'gold' "
      f"WHEN balance > 25000 THEN 'silver' ELSE 'bronze' END tier "
      f"FROM {users} LIMIT 100")

    b("Complex JOIN+GROUP+HAVING",
      f"SELECT u.name, COUNT(DISTINCT o.id) orders, SUM(o.amount) total "
      f"FROM {users} u INNER JOIN {orders} o ON u.id = o.user_id "
      f"WHERE u.active = 1 AND o.status IN ('completed','shipped') "
      f"GROUP BY u.id, u.name HAVING COUNT(o.id) > 1 LIMIT 50",
      iters=20, warmup=2)

    # Batch INSERT in transaction (100 rows per BEGIN/COMMIT cycle).
    counter = [0]
    def batch_run():
        counter[0] += 1
        c.start_transaction()
        rows = [(args.rows * 10 + counter[0] * 100 + i,
                 1, 100.0, "pending", "2024-02-01") for i in range(100)]
        cur.executemany(
            f"INSERT INTO {orders} (id, user_id, amount, status, order_date) "
            "VALUES (%s,%s,%s,%s,%s)", rows)
        c.commit()

    r = Result("Batch INSERT (100 rows)", label)
    r.samples = time_callable(batch_run, iters=iters_pt, warmup=0)
    r.explain = "<batch INSERT, no EXPLAIN>"
    out["Batch INSERT (100 rows)"] = r

    # ---- bottleneck hunters ----
    b("DISTINCT (no ORDER)", f"SELECT DISTINCT age FROM {users}")
    b("DISTINCT + ORDER BY", f"SELECT DISTINCT age FROM {users} ORDER BY age")
    b("COUNT DISTINCT",      f"SELECT COUNT(DISTINCT age) FROM {users}")
    b("LIKE prefix (User_1%)",
      f"SELECT * FROM {users} WHERE name LIKE 'User_1%' LIMIT 100")
    b("LIKE contains (%50%)",
      f"SELECT * FROM {users} WHERE email LIKE '%50%' LIMIT 100")
    b("OR conditions (3 vals)",
      f"SELECT * FROM {users} WHERE age = 25 OR age = 50 OR age = 75 LIMIT 100")
    b("IN list (7 values)",
      f"SELECT * FROM {users} WHERE age IN (20,25,30,35,40,45,50) LIMIT 100")
    b("NOT IN subquery",
      f"SELECT * FROM {users} WHERE id NOT IN "
      f"(SELECT user_id FROM {orders} WHERE status = 'cancelled') LIMIT 100",
      iters=10, warmup=2)
    b("NOT EXISTS subquery",
      f"SELECT * FROM {users} u WHERE NOT EXISTS "
      f"(SELECT 1 FROM {orders} o WHERE o.user_id = u.id "
      f"AND o.status = 'cancelled') LIMIT 100",
      iters=100)
    b("OFFSET pagination (5000)",
      f"SELECT * FROM {users} ORDER BY id LIMIT 100 OFFSET 5000")
    b("Multi-col ORDER BY (3)",
      f"SELECT * FROM {users} ORDER BY age DESC, balance ASC, name LIMIT 100")
    b("Self JOIN (same age)",
      f"SELECT u1.name, u2.name, u1.age FROM {users} u1 INNER JOIN {users} u2 "
      f"ON u1.age = u2.age AND u1.id < u2.id LIMIT 100",
      iters=100)
    b("Multi window funcs (3)",
      f"SELECT name, balance, ROW_NUMBER() OVER (ORDER BY balance DESC) rn, "
      f"RANK() OVER (ORDER BY balance DESC) rnk, "
      f"LAG(balance) OVER (ORDER BY balance DESC) prev_bal "
      f"FROM {users} LIMIT 100")
    b("Nested subquery (3 lvl)",
      f"SELECT * FROM {users} WHERE id IN "
      f"(SELECT user_id FROM {orders} WHERE amount > "
      f"(SELECT AVG(amount) FROM {orders})) LIMIT 100",
      iters=20, warmup=2)
    b("Multi aggregates (6)",
      f"SELECT COUNT(*), SUM(balance), AVG(balance), MIN(balance), "
      f"MAX(balance), COUNT(DISTINCT age) FROM {users}")
    b("COALESCE + IS NOT NULL",
      f"SELECT name, COALESCE(balance, 0) bal FROM {users} "
      f"WHERE balance IS NOT NULL LIMIT 100")
    b("Expr in WHERE (funcs)",
      f"SELECT * FROM {users} WHERE LENGTH(name) > 7 "
      f"AND UPPER(name) LIKE 'USER_%' LIMIT 100")
    b("Math expressions",
      f"SELECT name, balance * 1.1 new_bal, ROUND(balance / 1000, 2) k_bal, "
      f"ABS(balance - 50000) diff FROM {users} LIMIT 100")
    b("String concat (||)",
      f"SELECT name || ' (' || email || ')' full_info FROM {users} LIMIT 100")
    b("Large result (no LIMIT)",
      f"SELECT id, name, balance FROM {users} WHERE active = 1",
      iters=20, warmup=2)
    b("Multiple CTEs (2)",
      f"WITH young AS (SELECT * FROM {users} WHERE age < 30), "
      f"rich AS (SELECT * FROM {users} WHERE balance > 70000) "
      f"SELECT y.name, r.name FROM young y INNER JOIN rich r ON y.id = r.id "
      f"LIMIT 50", iters=100)
    b("Correlated in SELECT",
      f"SELECT u.name, (SELECT COUNT(*) FROM {orders} o WHERE o.user_id = u.id) "
      f"order_count FROM {users} u LIMIT 100",
      iters=100, warmup=5)
    b("BETWEEN (non-indexed)",
      f"SELECT * FROM {users} WHERE balance BETWEEN 25000 AND 75000 LIMIT 100")
    b("GROUP BY (2 columns)",
      f"SELECT age, active, COUNT(*), AVG(balance) FROM {users} "
      f"GROUP BY age, active")
    b("CROSS JOIN (limited)",
      f"SELECT u.name, o.status FROM {users} u CROSS JOIN "
      f"(SELECT DISTINCT status FROM {orders}) o LIMIT 100")
    b("Derived table (FROM sub)",
      f"SELECT t.age_group, COUNT(*) FROM "
      f"(SELECT CASE WHEN age < 30 THEN 'young' WHEN age < 50 THEN 'middle' "
      f"ELSE 'senior' END age_group FROM {users}) t GROUP BY t.age_group")
    b("Window ROWS frame",
      f"SELECT name, balance, SUM(balance) OVER "
      f"(ORDER BY balance ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) "
      f"rolling_sum FROM {users} LIMIT 100")
    b("HAVING complex",
      f"SELECT age FROM {users} GROUP BY age HAVING COUNT(*) > 100 "
      f"AND AVG(balance) > 40000")
    b("Compare with subquery",
      f"SELECT * FROM {users} WHERE balance > "
      f"(SELECT AVG(amount) * 100 FROM {orders}) LIMIT 100")

    cur.close()
    c.close()
    return out


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------
def fmt_us(v: float) -> str:
    """Plain microseconds with three decimals, right-aligned in 15
    chars to match stoolap-go/example/benchmark/main.go."""
    return f"{v:>15.3f}"


def fmt_ratio(stoolap_us: float, innodb_us: float) -> str:
    """Match the Go bench's ratio convention:
       Stoolap faster -> "  X.XXx"
       InnoDB faster  -> "  X.XXx*"  (asterisk marks the loser)
    """
    if stoolap_us <= 0 or innodb_us <= 0:
        return f"{'-':>10}"
    ratio = innodb_us / stoolap_us
    if ratio >= 1.0:
        return f"{ratio:>9.2f}x"
    return f"{1.0 / ratio:>8.2f}x*"


_HEADER_RULE = "=" * 80
_DIVIDER     = "-" * 80


def _print_section_header(name: str):
    print()
    print(_HEADER_RULE)
    print(name)
    print(_HEADER_RULE)
    print(f"{'Operation':<28} | {'Stoolap (μs)':>15} | "
          f"{'InnoDB (μs)':>15} | {'Ratio':>10}")
    print(_DIVIDER)


def report(results: dict, args):
    # Tally totals while we render. Mirrors Go bench's `stoolapWins` /
    # `sqliteWins` tally: a per-row asterisk indicates a Stoolap loss.
    stoolap_wins = 0
    innodb_wins  = 0
    losses: List[Tuple[str, float, "Result", "Result"]] = []

    print()
    print("Stoolap (MariaDB plugin) vs InnoDB - Benchmark")
    print(f"Configuration: {args.rows} rows, {args.iterations} iterations per test")
    print("Both engines under MariaDB 11.4 (same query parser / executor frame).")
    print("Ratio > 1x = Stoolap faster  |  * = InnoDB faster")

    # Track which ops were rendered so we can append any leftovers in
    # an "OTHER" section (defensive: a future op added to run_engine
    # but not to SECTIONS won't disappear from the output).
    rendered: set = set()
    sections = SECTIONS + [
        ("OTHER",
         [op for op in results["STOOLAP"].keys()
          if not any(op in s_ops for _, s_ops in SECTIONS)]),
    ]

    for section_name, ops in sections:
        live_ops = [op for op in ops if op in results["STOOLAP"]]
        if not live_ops:
            continue
        _print_section_header(section_name)
        for op in live_ops:
            rendered.add(op)
            st  = results["STOOLAP"][op]
            ino = results["InnoDB"][op]
            ratio_str = fmt_ratio(st.median_us, ino.median_us)
            print(f"{op:<28} | {fmt_us(st.median_us)} | "
                  f"{fmt_us(ino.median_us)} | {ratio_str}")
            if st.median_us > 0 and ino.median_us > 0:
                if st.median_us < ino.median_us:
                    stoolap_wins += 1
                elif ino.median_us < st.median_us:
                    innodb_wins += 1
                    losses.append((op, ino.median_us / st.median_us, st, ino))

    # ---- final score line, Go-style ----
    print()
    print(_HEADER_RULE)
    print(f"SCORE: Stoolap {stoolap_wins} wins  |  InnoDB {innodb_wins} wins")
    print()
    print("NOTES:")
    print("- Both engines under MariaDB 11.4 (same parser / executor / row pump)")
    print("- Stoolap: ha_stoolap.so plugin (Apache 2.0)")
    print("- InnoDB: bundled storage engine")
    print("- Ratio > 1x = Stoolap faster  |  * = InnoDB faster")
    print(_HEADER_RULE)

    # ---- focused EXPLAIN dump for the loss cases ----
    if losses and args.explain_losses:
        print()
        print(f"Loss cases ({len(losses)}) -- diagnose with EXPLAIN below:")
        print(_DIVIDER)
        losses.sort(key=lambda t: t[1])
        for op, ratio, st, ino in losses:
            delta_us = st.median_us - ino.median_us
            print(f"\n{op}")
            print(f"  ratio:          {ratio:.2f}x* "
                  f"(STOOLAP {st.median_us:.2f}us vs "
                  f"InnoDB {ino.median_us:.2f}us, "
                  f"delta +{delta_us:.2f}us)")
            print(f"  STOOLAP EXPLAIN:")
            for line in (st.explain or "").splitlines():
                print(f"    {line}")
            print(f"  InnoDB  EXPLAIN:")
            for line in (ino.explain or "").splitlines():
                print(f"    {line}")

    # ---- CSV dump of every sample (optional) ----
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["operation", "engine", "iter", "us"])
            for engine in ("STOOLAP", "InnoDB"):
                for op in results[engine]:
                    r = results[engine][op]
                    for i, us in enumerate(r.samples):
                        w.writerow([op, engine, i, f"{us:.3f}"])
        print(f"\nWrote raw samples to {args.csv}")


# --------------------------------------------------------------------------
# Baseline / loss-budget enforcement (CI)
# --------------------------------------------------------------------------
def write_baseline(results: dict, path: str):
    """Snapshot the current run's medians as a JSON baseline. CI loads this
    and compares future runs against it."""
    out = {}
    for op in results["STOOLAP"]:
        st  = results["STOOLAP"][op]
        ino = results["InnoDB"][op]
        if st.median_us > 0 and ino.median_us > 0:
            ratio = ino.median_us / st.median_us
        else:
            ratio = 1.0
        out[op] = {
            "stoolap_median_us": round(st.median_us, 3),
            "innodb_median_us":  round(ino.median_us, 3),
            "ratio":             round(ratio, 4),
        }
    with open(path, "w") as f:
        json.dump(out, f, indent=2, sort_keys=True)
    print(f"\nWrote baseline to {path}")


def check_against_baseline(results: dict, baseline_path: str,
                            win_regress_pct: float,
                            loss_worsen_pct: float) -> int:
    """Compare current medians against a baseline.

    A 'win' in the baseline (ratio >= 1.0) becomes an alert if the current
    STOOLAP median regresses by more than `win_regress_pct` percent.

    A 'loss' in the baseline (ratio < 1.0) becomes an alert if the current
    ratio drops by more than `loss_worsen_pct` percent (so a loss getting
    worse trips the alert; a loss recovering does not)."""
    if not os.path.exists(baseline_path):
        print(f"error: baseline {baseline_path} not found.", file=sys.stderr)
        return 2
    with open(baseline_path) as f:
        baseline = json.load(f)

    alerts = []
    for op, base in baseline.items():
        if op not in results["STOOLAP"]:
            continue   # operation removed -- not a perf alert
        st_now  = results["STOOLAP"][op].median_us
        in_now  = results["InnoDB"][op].median_us
        st_base = base["stoolap_median_us"]
        ratio_base = base.get("ratio", 1.0)
        if st_now <= 0 or st_base <= 0:
            continue

        # Win in baseline -> check STOOLAP median for slowdown.
        if ratio_base >= 1.0:
            slowdown_pct = (st_now - st_base) / st_base * 100.0
            if slowdown_pct > win_regress_pct:
                alerts.append((
                    "WIN-REGRESSION", op,
                    f"STOOLAP median {st_base:.1f}us -> {st_now:.1f}us "
                    f"(+{slowdown_pct:.1f}%, threshold +{win_regress_pct:.0f}%)"))
        # Loss in baseline -> check the InnoDB/STOOLAP ratio drop.
        else:
            ratio_now = (in_now / st_now) if st_now > 0 else 0.0
            ratio_drop_pct = (ratio_base - ratio_now) / ratio_base * 100.0
            if ratio_drop_pct > loss_worsen_pct:
                alerts.append((
                    "LOSS-WORSENED", op,
                    f"ratio {ratio_base:.2f}x -> {ratio_now:.2f}x "
                    f"(-{ratio_drop_pct:.1f}%, threshold -{loss_worsen_pct:.0f}%)"))

    if alerts:
        print(f"\n{len(alerts)} perf alert(s) vs baseline {baseline_path}:")
        for kind, op, detail in alerts:
            print(f"  [{kind}] {op}: {detail}")
        return 1
    print(f"\nperf check OK ({len(baseline)} ops vs baseline).")
    return 0


# --------------------------------------------------------------------------
def parse_args():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", default=SOCKET_DEFAULT,
                    help=f"mariadbd UNIX socket (default: {SOCKET_DEFAULT})")
    ap.add_argument("--rows", type=int, default=ROW_COUNT_DEFAULT,
                    help=f"users table row count (default: {ROW_COUNT_DEFAULT})")
    ap.add_argument("--iterations", type=int, default=ITERATIONS_DEFAULT,
                    help=f"samples per point query (default: {ITERATIONS_DEFAULT}); "
                         "medium / heavy tiers scale to 1/2 and 1/10 of this")
    ap.add_argument("--seed", type=int, default=0xC0FFEE,
                    help="RNG seed (so both engines see the same data)")
    ap.add_argument("--csv", default=None,
                    help="write per-iteration samples to this CSV file")
    ap.add_argument("--explain-losses", action="store_true",
                    help="after the main table, dump EXPLAIN for each "
                         "operation where InnoDB beat STOOLAP")
    ap.add_argument("--write-baseline", default=None,
                    help="snapshot current medians + ratios to this JSON path")
    ap.add_argument("--check", default=None,
                    metavar="BASELINE.json",
                    help="compare against a baseline. Exits 1 if a known "
                         "win regressed beyond --win-regress-pct, or a "
                         "known loss worsened beyond --loss-worsen-pct.")
    ap.add_argument("--win-regress-pct", type=float, default=15.0,
                    help="alert if a known WIN's STOOLAP median grew by "
                         "more than this percent (default: 15)")
    ap.add_argument("--loss-worsen-pct", type=float, default=10.0,
                    help="alert if a known LOSS's ratio dropped by more "
                         "than this percent (default: 10)")
    ap.add_argument("--runs", type=int, default=1,
                    help="repeat the whole bench N times and keep the best "
                         "median per (engine, op). Filters one-off scheduler "
                         "noise on shared hosts. Default: 1.")
    ap.add_argument("--mode", choices=("fail", "warn"), default="fail",
                    help="when --check trips a budget: 'fail' exits 1 (CI "
                         "gate, dedicated host); 'warn' prints alerts and "
                         "exits 0 (noisy host). Default: fail.")
    return ap.parse_args()


def merge_best(per_run: List[dict]) -> dict:
    """Given results from N runs, return one merged set of Result objects
    where each (engine, op) is the run whose median was the smallest. We
    pick by median rather than by min so the merged samples reflect a
    representative distribution, not a tail-trimmed one."""
    if len(per_run) == 1:
        return per_run[0]
    out = {}
    for engine in per_run[0]:
        out[engine] = {}
        for op in per_run[0][engine]:
            best = per_run[0][engine][op]
            for r in per_run[1:]:
                cand = r.get(engine, {}).get(op)
                if cand and cand.median_us > 0 and (
                        best.median_us <= 0 or cand.median_us < best.median_us):
                    best = cand
            out[engine][op] = best
    return out


def main():
    args = parse_args()
    if not os.path.exists(args.socket):
        print(f"error: socket {args.socket} not found.", file=sys.stderr)
        print("Bring up the test server first:", file=sys.stderr)
        print("  KEEP_RUNNING=1 tests/run_all.sh 14_scale", file=sys.stderr)
        sys.exit(2)

    if args.runs < 1:
        print("error: --runs must be >= 1.", file=sys.stderr)
        sys.exit(2)

    per_run: List[dict] = []
    for run_idx in range(args.runs):
        if args.runs > 1:
            print(f"\n=== run {run_idx + 1}/{args.runs} ===")
        setup_db(args.socket)
        run_results = {}
        for label, engine, suffix in ENGINES:
            print(f">>> running {label} ({engine}) ...", flush=True)
            # Same seed for every engine in every run -> byte-identical data.
            rng = random.Random(args.seed)
            run_results[label] = run_engine(
                args.socket, label, engine, suffix, rng, args)
        per_run.append(run_results)
        # Tear down between runs so the next iteration starts clean.
        boot = mysql.connector.connect(
            user="root", unix_socket=args.socket, autocommit=True)
        boot.cursor().execute(f"DROP DATABASE {DB}")
        boot.close()

    results = merge_best(per_run)

    report(results, args)

    if args.write_baseline:
        write_baseline(results, args.write_baseline)

    rc = 0
    if args.check:
        rc = check_against_baseline(
            results, args.check,
            win_regress_pct=args.win_regress_pct,
            loss_worsen_pct=args.loss_worsen_pct)
        if rc == 1 and args.mode == "warn":
            print("(--mode warn: alerts above are non-fatal; exiting 0)")
            rc = 0

    sys.exit(rc)


if __name__ == "__main__":
    main()
