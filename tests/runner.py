#!/usr/bin/env python3
"""stoolap-mariadb test runner (Python).

Spawns a private mariadbd against /tmp/stoolap-test-data with the freshly
built ha_stoolap.so plugin loaded, then runs every cases/case_*.py file in
order. Each case file exposes a `run(harness)` function the runner invokes
with a Harness bound to its own private database.

Mirrors run_all.sh:
  python3 tests/runner.py                   # run everything
  python3 tests/runner.py 02_indexes        # only one case (basename match)

Environment:
  MARIADB_PREFIX        Default /opt/homebrew/opt/mariadb@11.4
  STOOLAP_PLUGIN        Default build/ha_stoolap.so under repo root
  STOOLAP_DSN           Stoolap-side DSN. Default `memory://`. Set
                        `file:///tmp/stoolap-test-stoolap` for an
                        apples-to-apples InnoDB comparison.
  MARIADBD_BIN          Override mariadbd path.
  MARIADB_BIN           Override mariadb client path.
  MARIADB_INSTALL_DB_BIN Override mariadb-install-db path.
  MARIADB_PLUGIN_DIR    Override plugin directory.
  KEEP_RUNNING=1        Leave mariadbd running after the run.
  REUSE_RUNNING=1       Don't start/stop a server -- reuse one at the
                        socket; useful for fast iteration.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent
CASES_DIR = REPO_ROOT / "tests" / "cases"
LIB_DIR   = REPO_ROOT / "tests" / "lib"

SOCK    = "/tmp/stoolap-test.sock"
DATA    = "/tmp/stoolap-test-data"
ERRLOG  = "/tmp/stoolap-test.err"
PIDFILE = "/tmp/stoolap-test.pid"

sys.path.insert(0, str(LIB_DIR))
from harness import Harness  # noqa: E402


DEFAULT_COUNTER_DELTA_ALLOWLIST = {
    "Stoolap_pushdown_hits",
    "Stoolap_pushdown_misses",
    "Stoolap_direct_modify_hits",
    "Stoolap_records_live_counts",
    "Stoolap_buffered_scans",
    "Stoolap_buffered_rows",
    # Stoolap-side typed-error gap signal: stoolap returned
    # STOOLAP_ERR_GENERIC for an error class our prose pattern still
    # classifies. The plugin behaviour stays correct (right HA_ERR_*,
    # right SQLSTATE), so this is not a plugin regression -- it's an
    # upstream signal to file. Read the counter via SHOW STATUS LIKE
    # 'Stoolap_typed_fallback_hits' to spot which run produced it.
    "Stoolap_typed_fallback_hits",
}


def _run_sql(mariadb_bin: str, sql: str) -> str:
    proc = subprocess.run(
        [mariadb_bin, "--no-defaults", "-S", SOCK, "-uroot", "-N", "-e", sql],
        capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or proc.stdout.strip())
    return proc.stdout or ""


def snapshot_stoolap_counters(mariadb_bin: str) -> Dict[str, int]:
    """Read the current Stoolap_* SHOW STATUS surface."""
    out: Dict[str, int] = {}
    for line in _run_sql(mariadb_bin, "SHOW STATUS LIKE 'Stoolap_%'").splitlines():
        parts = line.split("\t", 1)
        if len(parts) != 2:
            continue
        try:
            out[parts[0]] = int(parts[1])
        except ValueError:
            out[parts[0]] = 0
    return out


def unexpected_counter_deltas(
        before: Dict[str, int], after: Dict[str, int],
        allowed: Iterable[str]) -> List[Tuple[str, int]]:
    allowed_set = set(allowed)
    unexpected = []
    for name in sorted(set(before) | set(after)):
        delta = after.get(name, 0) - before.get(name, 0)
        if delta != 0 and name not in allowed_set:
            unexpected.append((name, delta))
    return unexpected


def read_global_var(mariadb_bin: str, name: str) -> str:
    for line in _run_sql(
            mariadb_bin, f"SHOW GLOBAL VARIABLES LIKE '{name}'").splitlines():
        parts = line.split("\t", 1)
        if len(parts) == 2:
            return parts[1]
    return ""


def set_global_bool(mariadb_bin: str, name: str, value: str) -> None:
    normalized = "ON" if value.upper() in {"ON", "1", "TRUE"} else "OFF"
    _run_sql(mariadb_bin, f"SET GLOBAL {name} = {normalized}")


class ServerCtx:
    """Owns the spawned mariadbd. No-op if REUSE_RUNNING=1."""

    def __init__(self, mariadb_prefix: str, plugin: str, dsn: str,
                 mariadbd_bin: Optional[str] = None,
                 mariadb_bin: Optional[str] = None,
                 install_db_bin: Optional[str] = None,
                 plugin_dir: Optional[str] = None):
        self.prefix = mariadb_prefix
        self.plugin = plugin
        self.dsn    = dsn
        self._mariadbd = mariadbd_bin
        self._mariadb = mariadb_bin
        self._install_db = install_db_bin
        self._plugin_dir = plugin_dir
        self.proc:  Optional[subprocess.Popen] = None
        self.reuse = os.environ.get("REUSE_RUNNING", "0") == "1"
        self.keep  = os.environ.get("KEEP_RUNNING",  "0") == "1"

    @property
    def mariadbd(self) -> str:
        return self._mariadbd or f"{self.prefix}/bin/mariadbd"

    @property
    def mariadb(self) -> str:
        return self._mariadb or f"{self.prefix}/bin/mariadb"

    @property
    def install_db(self) -> str:
        return self._install_db or f"{self.prefix}/bin/mariadb-install-db"

    @property
    def plugin_dir(self) -> str:
        return self._plugin_dir or f"{self.prefix}/lib/plugin"

    def start(self) -> None:
        if self.reuse:
            if not Path(SOCK).is_socket() and not os.path.exists(SOCK):
                print(f"runner: REUSE_RUNNING=1 but no socket at {SOCK}",
                      file=sys.stderr)
                sys.exit(2)
            print(f"runner: reusing existing server at {SOCK}")
            return

        # Install the plugin into MariaDB's plugin dir. Skip if the
        # destination is at least as new as our build artifact -- that's
        # the CI flow, where the workflow has already done a `sudo
        # install` before invoking the runner and the runner itself
        # doesn't have sudo. Comparing mtimes catches "you forgot to
        # re-copy after rebuilding" without needing a content hash;
        # byte-size used to be the gate but two different builds can
        # produce identically-sized .so files (different inline expansion,
        # same total bytes), silently masking edits.
        dst = f"{self.plugin_dir}/ha_stoolap.so"
        try:
            if (os.path.exists(dst) and
                    os.path.getmtime(dst) >= os.path.getmtime(self.plugin)):
                pass  # destination is up-to-date; CI flow
            else:
                shutil.copyfile(self.plugin, dst)
        except (PermissionError, OSError) as e:
            print(f"runner: cannot install plugin to {self.plugin_dir}: {e}",
                  file=sys.stderr)
            print("runner: pre-install with sudo, or set REUSE_RUNNING=1",
                  file=sys.stderr)
            sys.exit(2)

        for f in (SOCK, PIDFILE):
            try:
                os.unlink(f)
            except FileNotFoundError:
                pass
        Path(DATA).mkdir(parents=True, exist_ok=True)

        # First-time datadir bootstrap. mariadb-install-db reads
        # /etc/my.cnf by default, which on Homebrew can carry unknown
        # variables (e.g. mysqlx-bind-address) that abort the spawned
        # mariadbd. Pass --no-defaults so install runs in a clean
        # config the same way the test mariadbd below does.
        if not any(Path(DATA).iterdir()):
            rc = subprocess.run(
                [self.install_db,
                 "--no-defaults",
                 f"--datadir={DATA}",
                 "--auth-root-authentication-method=normal"],
                capture_output=True, text=True)
            if rc.returncode != 0:
                print("runner: mariadb-install-db failed",
                      file=sys.stderr)
                print(rc.stdout, file=sys.stderr)
                print(rc.stderr, file=sys.stderr)
                sys.exit(2)

        # Pre-create file:// dir if requested.
        if self.dsn.startswith("file://"):
            stoolap_dir = self.dsn[len("file://"):].split("?", 1)[0]
            stoolap_dir = stoolap_dir.split("#", 1)[0]
            Path(stoolap_dir).mkdir(parents=True, exist_ok=True)

        argv = [
            self.mariadbd,
            "--no-defaults",
            f"--datadir={DATA}",
            f"--socket={SOCK}",
            "--port=33370",
            "--skip-networking",
            f"--pid-file={PIDFILE}",
            f"--log-error={ERRLOG}",
            f"--plugin-dir={self.plugin_dir}",
            "--plugin-maturity=alpha",
            "--plugin-load-add=ha_stoolap.so",
            f"--loose-stoolap-dsn={self.dsn}",
            "--innodb-buffer-pool-size=64M",
            "--innodb-flush-log-at-trx-commit=2",
            # Pin server charset/collation for portability across distros.
            # Brew MariaDB defaults to utf8mb4/utf8mb4_general_ci; Debian/
            # Ubuntu apt builds default to latin1/latin1_swedish_ci, which
            # silently changes how the ci-collation fixtures behave (the
            # accent-fold tests in case_02 expect 'e' = 'é' under
            # utf8mb4_general_ci's MariaDB-specific folding rules).
            "--character-set-server=utf8mb4",
            "--collation-server=utf8mb4_general_ci",
        ]
        # mariadbd refuses to run as root unless --user is explicit;
        # GitHub-hosted runners use the `runner` user so they don't trip
        # this guard, but Docker containers (and any local dev setup
        # that runs as root) do. Allow --user override via env so the
        # same runner.py works in both worlds.
        if os.geteuid() == 0:
            argv.append(f"--user={os.environ.get('MARIADBD_USER', 'root')}")
        self.proc = subprocess.Popen(argv,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL,
                                     start_new_session=True)

        # Wait up to ~5s for the socket.
        for _ in range(50):
            if os.path.exists(SOCK):
                break
            time.sleep(0.1)
        else:
            print("runner: server did not come up within 5s; tail of "
                  f"{ERRLOG}:", file=sys.stderr)
            self._dump_errlog()
            self.stop()
            sys.exit(2)

        # Verify plugin loaded.
        engines = subprocess.run(
            [self.mariadb, "--no-defaults", "-S", SOCK, "-uroot",
             "-ss", "-e", "SHOW ENGINES"],
            capture_output=True, text=True)
        if "STOOLAP" not in engines.stdout.upper():
            print("runner: STOOLAP engine missing after plugin load; "
                  f"tail of {ERRLOG}:", file=sys.stderr)
            self._dump_errlog()
            self.stop()
            sys.exit(2)

    def stop(self) -> None:
        if self.proc is None or self.reuse:
            return
        if self.keep:
            print(f"runner: leaving mariadbd running (PID={self.proc.pid})")
            self.proc = None
            return
        try:
            self.proc.send_signal(signal.SIGTERM)
            self.proc.wait(timeout=10)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            self.proc.kill()
            self.proc.wait()
        for f in (SOCK, PIDFILE):
            try:
                os.unlink(f)
            except FileNotFoundError:
                pass
        self.proc = None

    def _dump_errlog(self) -> None:
        try:
            with open(ERRLOG, "r", errors="replace") as f:
                tail = f.readlines()[-30:]
                sys.stderr.writelines(tail)
        except FileNotFoundError:
            pass


def discover_cases(filter_substr: str) -> List[Path]:
    found = sorted(CASES_DIR.glob("case_*.py"))
    if filter_substr:
        found = [p for p in found if filter_substr in p.stem]
    return found


def load_case(path: Path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"can't load {path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if not hasattr(mod, "run"):
        raise AttributeError(f"{path}: missing top-level run(harness)")
    return mod


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("filter", nargs="?", default="",
                        help="substring filter against case basenames")
    args = parser.parse_args()

    plugin = os.environ.get(
        "STOOLAP_PLUGIN",
        str(REPO_ROOT / "build" / "ha_stoolap.so"))
    if not Path(plugin).exists():
        print(f"runner: plugin not built at {plugin}", file=sys.stderr)
        print(f"  run: cmake --build {REPO_ROOT}/build", file=sys.stderr)
        return 2

    server = ServerCtx(
        mariadb_prefix=os.environ.get(
            "MARIADB_PREFIX", "/opt/homebrew/opt/mariadb@11.4"),
        plugin=plugin,
        dsn=os.environ.get("STOOLAP_DSN", "memory://"),
        mariadbd_bin=os.environ.get("MARIADBD_BIN"),
        mariadb_bin=os.environ.get("MARIADB_BIN"),
        install_db_bin=os.environ.get("MARIADB_INSTALL_DB_BIN"),
        plugin_dir=os.environ.get("MARIADB_PLUGIN_DIR"))
    server.start()

    cases = discover_cases(args.filter)
    if not cases:
        print(f"runner: no cases matched filter {args.filter!r}",
              file=sys.stderr)
        server.stop()
        return 2

    total_pass = 0
    total_fail = 0
    total_skip = 0
    failed_files: List[str] = []
    suite_counter_allowlist: Set[str] = set(DEFAULT_COUNTER_DELTA_ALLOWLIST)
    suite_counter_start = snapshot_stoolap_counters(server.mariadb)

    try:
        for case in cases:
            print(f"\n========== {case.stem} ==========")
            db = case.stem.replace("case_", "stoolap_test_")
            harness = Harness(socket=SOCK, db=db,
                              mariadb_bin=server.mariadb,
                              errlog_path=ERRLOG)
            mod = None
            case_counter_start = snapshot_stoolap_counters(server.mariadb)
            perf_trace_before = read_global_var(
                server.mariadb, "stoolap_perf_trace")
            try:
                mod = load_case(case)
                case_allow = set(getattr(
                    mod, "STOOLAP_COUNTERS_ALLOW_DELTA", set()))
                suite_counter_allowlist.update(case_allow)
                mod.run(harness)
            except Exception as e:                 # pylint: disable=broad-except
                # Mirror bash's "subshell rc != 0" rollup.
                print(f"runner: {case.stem} raised: {e!r}",
                      file=sys.stderr)
                failed_files.append(case.stem)
                harness.failed += 1
            finally:
                # Safe because the runner is serial; revisit this restore if
                # case execution ever becomes parallel.
                perf_trace_after = read_global_var(
                    server.mariadb, "stoolap_perf_trace")
                if perf_trace_before and perf_trace_after != perf_trace_before:
                    set_global_bool(server.mariadb, "stoolap_perf_trace",
                                    perf_trace_before)

                case_allow = set(DEFAULT_COUNTER_DELTA_ALLOWLIST)
                if mod is not None:
                    case_allow.update(getattr(
                        mod, "STOOLAP_COUNTERS_ALLOW_DELTA", set()))
                case_counter_end = snapshot_stoolap_counters(server.mariadb)
                unexpected = unexpected_counter_deltas(
                    case_counter_start, case_counter_end, case_allow)
                for name, delta in unexpected:
                    harness._fail(  # pylint: disable=protected-access
                        f"unexpected Stoolap counter delta: {name}",
                        f"delta: {delta}",
                        "declare STOOLAP_COUNTERS_ALLOW_DELTA in this case "
                        "only for intentional counter movement")

                harness.summary(case.stem)
                total_pass += harness.passed
                total_fail += harness.failed
                total_skip += harness.skipped
                if harness.failed > 0 and case.stem not in failed_files:
                    failed_files.append(case.stem)
                harness.close()

        suite_counter_end = snapshot_stoolap_counters(server.mariadb)
        suite_unexpected = unexpected_counter_deltas(
            suite_counter_start, suite_counter_end, suite_counter_allowlist)
        if suite_unexpected:
            print("\n========== suite counter drift ==========")
            for name, delta in suite_unexpected:
                print(f"  FAIL  unexpected Stoolap counter delta: {name}")
                print(f"          delta: {delta}")
            total_fail += len(suite_unexpected)
            failed_files.append("suite_counters")
    finally:
        server.stop()

    print("\n========== runner summary ==========")
    print(f"{total_pass} passed, {total_fail} failed, "
          f"{total_skip} skipped across {len(cases)} files")
    if not failed_files:
        print("All test files passed.")
        return 0
    print(f"Failures in: {', '.join(failed_files)}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
