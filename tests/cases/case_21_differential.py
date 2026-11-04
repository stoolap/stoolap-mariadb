"""Differential scenarios: every tests/diff/*.sql runs statement by
statement on STOOLAP and on InnoDB, in two fresh databases, and each
statement must give the same outcome on both: the same rows (in order
when the statement has ORDER BY, as a multiset otherwise), the same
errno, or the same affected-row count. After the last statement every
table's contents must match too.

Scenario format:
- `{E}` is replaced by the engine under test; write InnoDB literally for
  a table that must stay on InnoDB (a mixed-engine driver).
- A statement ends with ';' at the end of a line.
- `-- known-diff: <reason>` before a statement records a divergence we
  accept for now (with the issue that tracks it). It is reported, not
  failed; a known-diff statement that starts to match fails, so the
  note gets removed. `-- known-diff-final: <reason>` anywhere does the
  same for the final table contents, and `-- known-diff-file: <reason>`
  for the whole scenario (an open finding from the audit backlog: it
  fails once the scenario matches, so the note gets removed).
- Other `--` lines are comments."""

import datetime
import os
import re
from pathlib import Path

import mysql.connector

from harness import _format_cell

# STOOLAP_DIFF_DIR points the case at another directory of scenarios
# (e.g. candidates being triaged) without touching tests/diff.
# Scenarios include statements stoolap answers with an error MariaDB maps
# differently; the per-statement errno comparison already reports those.
STOOLAP_COUNTERS_ALLOW_DELTA = {"Stoolap_unmapped_errors"}

DIFF_DIR = Path(os.environ.get(
    "STOOLAP_DIFF_DIR", Path(__file__).resolve().parent.parent / "diff"))
ENGINES = ("STOOLAP", "InnoDB")


def _parse(path):
    """Yield (sql, known_diff_reason_or_None) per statement."""
    buf, known = [], None
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if not buf and stripped.startswith("--"):
            if re.match(r"--\s*known-diff-(final|file):", stripped):
                continue
            m = re.match(r"--\s*known-diff:\s*(.+)", stripped)
            if m:
                known = m.group(1)
            continue
        if not stripped and not buf:
            continue
        buf.append(line)
        if stripped.endswith(";"):
            sql = "\n".join(buf).strip()[:-1].strip()
            yield sql, known
            buf, known = [], None
    if buf:
        raise ValueError(f"{path.name}: trailing statement without ';'")


def _cell(v):
    """Lossless comparison key for one value: NULL, bytes and text stay
    distinct ('NULL' text is not NULL; b'\\xff' is not b'\\xfe').
    _format_cell is for display only."""
    if v is None:
        return ("null",)
    if isinstance(v, (bytes, bytearray)):
        return ("bytes", bytes(v))
    if isinstance(v, str):
        return ("str", v)
    if isinstance(v, datetime.timedelta):
        # TIME: _format_cell renders -0.5 s and +0.5 s alike.
        return ("time", v.days, v.seconds, v.microseconds)
    if isinstance(v, float):
        # repr keeps -0.0 apart from 0.0, which _format_cell prints as "0".
        return ("float", repr(v))
    return ("val", _format_cell(v))


def _show_cell(c):
    if c[0] == "null":
        return "NULL"
    if c[0] == "bytes":
        return "0x" + c[1].hex()
    return c[1]


def _outcome(cur, sql):
    try:
        cur.execute(sql)
        if cur.with_rows:
            rows = [tuple(_cell(c) for c in r) for r in cur.fetchall()]
            if not re.search(r"\border\s+by\b", sql, re.IGNORECASE):
                rows.sort()
            return ("rows", rows)
        return ("ok", cur.rowcount)
    except mysql.connector.Error as e:
        return ("err", e.errno)


def _show(outcome, limit=6):
    kind, val = outcome
    if kind == "rows":
        more = f" ... ({len(val)} rows)" if len(val) > limit else ""
        shown = [tuple(_show_cell(c) for c in r) for r in val[:limit]]
        return f"rows {shown}{more}"
    if kind == "err":
        return f"errno {val}"
    return f"ok, {val} row(s) affected"


def _table_dump(cur):
    cur.execute("SHOW TABLES")
    out = {}
    for (t,) in cur.fetchall():
        cur.execute(f"SELECT * FROM `{t}`")
        out[t] = sorted(tuple(_cell(c) for c in r) for r in cur.fetchall())
    return out


def _fresh(h, db):
    boot = h._connect()
    c = boot.cursor()
    c.execute(f"DROP DATABASE IF EXISTS `{db}`")
    c.execute(f"CREATE DATABASE `{db}`")
    boot.close()
    return h._connect(database=db)


def _directive(path, name):
    m = re.search(rf"^--\s*known-diff-{name}:\s*(.+)$", path.read_text(),
                  re.MULTILINE)
    return m.group(1) if m else None


def _run_scenario(h, path):
    final_known = _directive(path, "final")
    file_known = _directive(path, "file")
    conns = {e: _fresh(h, f"{h.db}_{e.lower()}") for e in ENGINES}
    curs = {e: conns[e].cursor() for e in ENGINES}
    mismatches, known_seen, stale = [], [], []
    try:
        for i, (sql, known) in enumerate(_parse(path), 1):
            out = {e: _outcome(curs[e], sql.replace("{E}", e))
                   for e in ENGINES}
            same = out["STOOLAP"] == out["InnoDB"]
            if known:
                (stale if same else known_seen).append((i, sql, known, out))
            elif not same:
                mismatches.append((i, sql, out))
        dumps = {e: _table_dump(curs[e]) for e in ENGINES}
    finally:
        for e in ENGINES:
            conns[e].close()

    label = f"{path.stem}"
    if dumps["STOOLAP"] != dumps["InnoDB"]:
        diff_tables = sorted(t for t in set(dumps["STOOLAP"]) |
                             set(dumps["InnoDB"])
                             if dumps["STOOLAP"].get(t) != dumps["InnoDB"].get(t))
        out = {e: ("rows", [r for t in diff_tables for r in
                            dumps[e].get(t, [])]) for e in ENGINES}
        sql = f"final contents of {diff_tables}"
        if final_known:
            known_seen.append(("final", sql, final_known, out))
        else:
            mismatches.append((0, sql, out))
    elif final_known:
        stale.append(("final", "final contents", final_known, None))
    if file_known:
        if mismatches or known_seen:
            h.skipped += 1
            print(f"  KNOWN {label}: {file_known}")
        else:
            h._fail(label, f"known-diff-file no longer differs: {file_known}")
        return
    for i, sql, reason, out in known_seen:
        h.skipped += 1
        print(f"  KNOWN {label} #{i}: {reason}")
    if mismatches or stale:
        details = []
        for i, sql, out in mismatches:
            details += [f"#{i}: {sql}",
                        f"    STOOLAP: {_show(out['STOOLAP'])}",
                        f"    InnoDB:  {_show(out['InnoDB'])}"]
        for i, sql, reason, out in stale:
            details += [f"#{i}: known-diff no longer differs ({reason}): {sql}"]
        h._fail(label, *details)
    else:
        h._pass(label)


def run(h):
    # A missing or empty directory (a mistyped STOOLAP_DIFF_DIR) must not
    # pass with zero comparisons.
    if not DIFF_DIR.is_dir():
        h._fail(f"scenario directory {DIFF_DIR} does not exist")
        return
    scenarios = sorted(DIFF_DIR.glob("*.sql"))
    if not scenarios:
        h._fail(f"no *.sql scenarios in {DIFF_DIR}")
        return
    h.section(f"{len(scenarios)} scenarios from {DIFF_DIR}")
    for path in scenarios:
        _run_scenario(h, path)
