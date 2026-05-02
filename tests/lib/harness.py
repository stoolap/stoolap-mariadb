"""Shared test harness for stoolap-mariadb cases.

Mirrors the assertion API of the old `lib/harness.sh` so the per-case
files port one-to-one. Each Harness instance owns a mysql.connector
connection bound to a single test database, prints PASS/FAIL lines as
the bash runner did, and rolls counts up via the runner.
"""

from __future__ import annotations

import datetime as _dt
import decimal as _dec
import os
import re
import subprocess
import sys
from typing import Any, Iterable, List, Optional, Sequence, Tuple

import mysql.connector


# ---------------------------------------------------------------------------
# Helpers used by every case
# ---------------------------------------------------------------------------
def _clean_label(s: str) -> str:
    return " ".join(s.split())


class Harness:
    """One test database, one connection. Counts PASS/FAIL/SKIP locally
    and bubbles totals up through `runner.py` via the public attrs."""

    def __init__(self,
                 socket: str,
                 db: str,
                 engine: str = "STOOLAP",
                 mariadb_bin: Optional[str] = None,
                 errlog_path: Optional[str] = None):
        self.socket  = socket
        self.db      = db
        self.engine  = engine
        self.errlog  = errlog_path
        self.mariadb = mariadb_bin or "mariadb"

        self.passed  = 0
        self.failed  = 0
        self.skipped = 0
        self.current_test = ""

        # Each Harness owns its own root connection on the test DB.
        # Cases reuse it via the assertion helpers; if a test needs a
        # second session (concurrency tests), it opens its own.
        self.reset_db()
        self.conn = self._connect(database=db)
        self.cur  = self.conn.cursor()

    # ------------------------------------------------------------------
    # Connection helpers
    # ------------------------------------------------------------------
    def _connect(self, database: Optional[str] = None,
                 autocommit: bool = True) -> mysql.connector.MySQLConnection:
        kw = dict(user="root", unix_socket=self.socket, autocommit=autocommit)
        if database is not None:
            kw["database"] = database
        return mysql.connector.connect(**kw)

    def open_session(self,
                     autocommit: bool = True
                     ) -> Tuple[mysql.connector.MySQLConnection, Any]:
        """Return a fresh (conn, cursor) pair on the test DB.
        Used by tests that need multiple concurrent sessions."""
        c = self._connect(database=self.db, autocommit=autocommit)
        return c, c.cursor()

    def reset_db(self) -> None:
        """Drop and recreate the test database."""
        boot = self._connect()
        cur  = boot.cursor()
        cur.execute(f"DROP DATABASE IF EXISTS {self.db}")
        cur.execute(f"CREATE DATABASE {self.db}")
        cur.close()
        boot.close()

    # ------------------------------------------------------------------
    # Output helpers
    # ------------------------------------------------------------------
    def section(self, name: str) -> None:
        print(f"\n--- {name} ---")

    def _pass(self, label: str) -> None:
        self.passed += 1
        print(f"  PASS  {_clean_label(label)}")

    def _fail(self, label: str, *detail_lines: str) -> None:
        self.failed += 1
        print(f"  FAIL  {_clean_label(label)}")
        for line in detail_lines:
            print(f"          {line}")

    def summary(self, current_test: str) -> None:
        print(f"\n[{current_test}] {self.passed} passed, "
              f"{self.failed} failed, {self.skipped} skipped")

    # ------------------------------------------------------------------
    # SQL execution
    # ------------------------------------------------------------------
    def sql(self, stmt: str, *, db: Optional[str] = None) -> str:
        """Run a single statement on the test DB (or another db).
        Returns stdout-style text -- one line per row, tabs between
        columns -- so callers can compare against literal strings the
        same way the shell harness did via `mariadb -ss -e ...`."""
        if db is None:
            cur = self.cur
        else:
            cur = self._tmp_cursor(db)
        try:
            cur.execute(stmt)
            try:
                rows = cur.fetchall()
            except mysql.connector.errors.InterfaceError:
                return ""
            cols = cur.column_names
            if rows is None:
                return ""
            out = []
            for r in rows:
                out.append("\t".join(_format_cell(c) for c in r))
            return "\n".join(out)
        finally:
            if db is not None:
                cur.close()

    def exec_stmt(self, stmt: str) -> None:
        """Run a statement we don't care about output of (DDL, INSERT,
        UPDATE, DELETE). Errors propagate as Python exceptions."""
        self.cur.execute(stmt)
        try:
            self.cur.fetchall()
        except mysql.connector.errors.InterfaceError:
            pass

    def exec_script(self, script: str, *,
                    use_db: bool = True,
                    ignore_errors: bool = False) -> None:
        """Run a multi-statement SQL script via the `mariadb` client.
        Mirrors the `$MARIADB -e "..."` shell pattern: a fresh
        connection, USE $TEST_DB prepended automatically, errors
        printed but optionally ignored. Lets us reuse the existing
        bash-style table-bootstrap scripts verbatim."""
        argv = [self.mariadb, "--no-defaults",
                "-S", self.socket, "-uroot"]
        if ignore_errors:
            argv.append("--force")
        prefix = f"USE {self.db};\n" if use_db else ""
        proc = subprocess.run(
            argv, input=prefix + script, text=True,
            capture_output=True, check=False)
        # Echo any stderr to give the shell-style "noisy bootstrap"
        # output for debugging without aborting the test.
        if proc.stderr.strip() and not ignore_errors:
            for line in proc.stderr.splitlines():
                if line.strip():
                    print(line, file=sys.stderr)

    # ------------------------------------------------------------------
    # Assertion helpers (mirror harness.sh)
    # ------------------------------------------------------------------
    def assert_eq(self, label: str, expected: Any, actual: Any) -> None:
        if str(expected) == str(actual):
            self._pass(label)
        else:
            self._fail(label,
                       f"expected: <{expected}>",
                       f"actual:   <{actual}>")

    def assert_scalar(self, label: str, sql: str, expected: Any) -> None:
        actual = self.sql(sql)
        self.assert_eq(label, expected, actual)

    def assert_ok(self, label: str, sql: str) -> None:
        try:
            self.cur.execute(sql)
            try:
                self.cur.fetchall()
            except mysql.connector.errors.InterfaceError:
                pass
            self._pass(label)
        except mysql.connector.Error as e:
            self._fail(label, f"sql:    {sql}", f"server: {e}")

    def assert_err(self, label: str, sql: str, pattern: str) -> None:
        """Run sql and assert the resulting error matches `pattern`
        (regex, case-insensitive to mirror `grep -qE` semantics in the
        bash harness, which used patterns like 'Got error|ERROR')."""
        compiled = re.compile(pattern, re.IGNORECASE)
        try:
            self.cur.execute(sql)
            try:
                self.cur.fetchall()
            except mysql.connector.errors.InterfaceError:
                pass
            self._fail(label,
                       f"sql:      {sql}",
                       f"pattern:  {pattern}",
                       f"server:   <no error>")
        except mysql.connector.Error as e:
            # Match against repr (errno + sqlstate + msg) so patterns
            # like "1062" or "Duplicate entry" both work.
            text = f"{e!r}"
            if compiled.search(text):
                self._pass(label)
            else:
                self._fail(label,
                           f"sql:      {sql}",
                           f"pattern:  {pattern}",
                           f"server:   {text}")

    # ------------------------------------------------------------------
    # EXPLAIN-based assertions
    # ------------------------------------------------------------------
    def _explain_text(self, sql: str, setup: Iterable[str] = ()) -> str:
        """Return the EXPLAIN result as one big string the way `mariadb
        -ss -e 'EXPLAIN ...'` did -- tab-separated, newline per row."""
        if setup:
            return self.sql_with_session(setup, f"EXPLAIN {sql}")
        return self.sql(f"EXPLAIN {sql}")

    def _status_int(self, var_name: str) -> int:
        value = self.status(var_name)
        try:
            return int(value)
        except (TypeError, ValueError):
            return 0

    def _assert_pushdown_counter_contract(
            self, label: str, sql: str, text: str, marker: str,
            before_hits: int, before_misses: int, *,
            allow_misses: bool = False) -> None:
        hit_delta = self._status_int("Stoolap_pushdown_hits") - before_hits
        miss_delta = (
            self._status_int("Stoolap_pushdown_misses") - before_misses)
        miss_ok = allow_misses or miss_delta == 0
        if marker in text and hit_delta >= 1 and miss_ok:
            self._pass(label)
        else:
            self._fail(label,
                       f"sql:    {sql}",
                       f"explain: {text or '<empty>'}",
                       f"hit_delta:  {hit_delta}",
                       f"miss_delta: {miss_delta}")

    def assert_pushed(self, label: str, sql: str, *,
                      setup: Iterable[str] = ()) -> None:
        before_hits = self._status_int("Stoolap_pushdown_hits")
        before_misses = self._status_int("Stoolap_pushdown_misses")
        text = self._explain_text(sql, setup)
        self._assert_pushdown_counter_contract(
            label, sql, text, "PUSHED SELECT", before_hits, before_misses)

    def assert_not_pushed(self, label: str, sql: str, *,
                          setup: Iterable[str] = ()) -> None:
        before_hits = self._status_int("Stoolap_pushdown_hits")
        text = self._explain_text(sql, setup)
        hit_delta = self._status_int("Stoolap_pushdown_hits") - before_hits
        if "PUSHED SELECT" not in text and "PUSHED UNION" not in text \
                and "PUSHED DERIVED" not in text and hit_delta == 0:
            self._pass(label)
        else:
            self._fail(label,
                       f"sql:    {sql}",
                       f"explain (was pushed): {text}",
                       f"hit_delta: {hit_delta}")

    def assert_pushed_union(self, label: str, sql: str, *,
                            setup: Iterable[str] = ()) -> None:
        before_hits = self._status_int("Stoolap_pushdown_hits")
        before_misses = self._status_int("Stoolap_pushdown_misses")
        text = self._explain_text(sql, setup)
        self._assert_pushdown_counter_contract(
            label, sql, text, "PUSHED UNION", before_hits, before_misses)

    def assert_pushed_derived(self, label: str, sql: str, *,
                              setup: Iterable[str] = ()) -> None:
        before_hits = self._status_int("Stoolap_pushdown_hits")
        before_misses = self._status_int("Stoolap_pushdown_misses")
        text = self._explain_text(sql, setup)
        self._assert_pushdown_counter_contract(
            label, sql, text, "PUSHED DERIVED", before_hits, before_misses,
            allow_misses=True)

    # ------------------------------------------------------------------
    # Async / subprocess helpers (used by concurrent cases)
    # ------------------------------------------------------------------
    class _AsyncClient:
        """Handle for an async mariadb client started via run_async()."""

        def __init__(self, proc: subprocess.Popen, script: str):
            self.proc = proc
            self.script = script
            self._output: Optional[str] = None
            try:
                if proc.stdin is not None:
                    proc.stdin.write(script)
                    proc.stdin.close()
            except BrokenPipeError:
                pass

        def wait(self, timeout: Optional[float] = None) -> str:
            if self._output is None:
                # Don't use Popen.communicate() here. We intentionally
                # closed stdin in __init__ (so the subprocess sees EOF
                # and starts processing while the test sleeps). On
                # Linux, communicate() iterates stdin in its poll loop
                # and raises ValueError("I/O operation on closed file");
                # macOS is more forgiving and lets it through. Read
                # stdout directly and wait on the process instead.
                try:
                    out = self.proc.stdout.read() if self.proc.stdout else ""
                    self.proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
                    out = ""
                self._output = out or ""
            return self._output

    def run_async(self, script: str, *,
                  use_db: bool = True,
                  force: bool = False) -> "Harness._AsyncClient":
        """Spawn a mariadb client subprocess and return a handle. Mirrors
        bash's `$MARIADB ... <<SQL ... SQL &` pattern. Call .wait() to
        block on the script and collect its combined output."""
        argv = [self.mariadb, "--no-defaults", "-S", self.socket, "-uroot",
                "-ss"]
        if force:
            argv.append("--force")
        if use_db:
            argv.append(self.db)
        proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        return Harness._AsyncClient(proc, script)

    def run_client(self, script: str, *, use_db: bool = True,
                   force: bool = False) -> Tuple[int, str]:
        """Synchronous wrapper: feed `script` to a fresh mariadb client
        and return (returncode, combined-output). stderr is merged into
        stdout in arrival order (mirroring bash's `2>&1`) so callers
        that `tail -1` to grab the final SELECT result still work even
        when an earlier statement printed an ERROR line."""
        argv = [self.mariadb, "--no-defaults", "-S", self.socket, "-uroot",
                "-ss"]
        if force:
            argv.append("--force")
        if use_db:
            argv.append(self.db)
        proc = subprocess.run(argv, input=script, text=True,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              check=False)
        return proc.returncode, (proc.stdout or "")

    def sql_with_session(self, setup: Iterable[str], stmt: str) -> str:
        """Run `stmt` after applying SET-style `setup` lines on a fresh
        connection. Used by 06_pushdown for SET stoolap_trust_binary_strings
        + EXPLAIN. Returns the same tab-joined text `sql()` does."""
        conn = self._connect(database=self.db)
        try:
            cur = conn.cursor()
            for s in setup:
                cur.execute(s)
                try:
                    cur.fetchall()
                except mysql.connector.errors.InterfaceError:
                    pass
            cur.execute(stmt)
            try:
                rows = cur.fetchall()
            except mysql.connector.errors.InterfaceError:
                rows = []
            cur.close()
            out = []
            for r in rows or []:
                out.append("\t".join(_format_cell(c) for c in r))
            return "\n".join(out)
        finally:
            conn.close()

    # ------------------------------------------------------------------
    # Misc
    # ------------------------------------------------------------------
    def status(self, var_name: str) -> str:
        """Read a SHOW STATUS variable's value (single column)."""
        rows = self._fetch(f"SHOW STATUS LIKE '{var_name}'")
        if not rows:
            return ""
        return str(rows[0][1])

    def _fetch(self, sql: str) -> List[Sequence[Any]]:
        self.cur.execute(sql)
        try:
            return self.cur.fetchall() or []
        except mysql.connector.errors.InterfaceError:
            return []

    def _tmp_cursor(self, database: str):
        c = self._connect(database=database)
        return c.cursor()

    # ------------------------------------------------------------------
    # Cleanup
    # ------------------------------------------------------------------
    def close(self) -> None:
        try:
            self.cur.close()
        except Exception:
            pass
        try:
            self.conn.close()
        except Exception:
            pass


def _format_cell(v: Any) -> str:
    """Mirror `mariadb -ss` output. NULL -> 'NULL'. bytes decoded as
    UTF-8 with replacement. Datetimes formatted to MariaDB's wire form
    (' ' separator, six-digit microseconds when present) so cases that
    compare against literal '2026-04-28 12:34:56.654321' still match."""
    if v is None:
        return "NULL"
    if isinstance(v, bytes):
        return v.decode("utf-8", errors="replace")
    if isinstance(v, _dt.datetime):
        if v.microsecond:
            return v.strftime("%Y-%m-%d %H:%M:%S.") + f"{v.microsecond:06d}"
        return v.strftime("%Y-%m-%d %H:%M:%S")
    if isinstance(v, _dt.date):
        return v.strftime("%Y-%m-%d")
    if isinstance(v, _dt.timedelta):
        # MariaDB renders TIME as HH:MM:SS[.ffffff].
        secs = int(v.total_seconds())
        h, rem = divmod(secs, 3600)
        m, s = divmod(rem, 60)
        if v.microseconds:
            return f"{h:02d}:{m:02d}:{s:02d}.{v.microseconds:06d}"
        return f"{h:02d}:{m:02d}:{s:02d}"
    if isinstance(v, _dec.Decimal):
        # Decimal('1.50') renders as '1.50' already; this is just to
        # block the trailing exponent that `Decimal('1E+1')` would emit.
        return format(v, "f")
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, float):
        # MariaDB formats integer-valued doubles as "10" not "10.0";
        # mysql.connector hands them back as Python floats. Strip the
        # trailing ".0" to match.
        if v.is_integer():
            return str(int(v))
        return repr(v) if abs(v) >= 1e16 else str(v)
    return str(v)
