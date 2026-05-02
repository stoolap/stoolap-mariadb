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
from typing import List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
CASES_DIR = REPO_ROOT / "tests" / "cases"
LIB_DIR   = REPO_ROOT / "tests" / "lib"

SOCK    = "/tmp/stoolap-test.sock"
DATA    = "/tmp/stoolap-test-data"
ERRLOG  = "/tmp/stoolap-test.err"
PIDFILE = "/tmp/stoolap-test.pid"

sys.path.insert(0, str(LIB_DIR))
from harness import Harness  # noqa: E402


class ServerCtx:
    """Owns the spawned mariadbd. No-op if REUSE_RUNNING=1."""

    def __init__(self, mariadb_prefix: str, plugin: str, dsn: str):
        self.prefix = mariadb_prefix
        self.plugin = plugin
        self.dsn    = dsn
        self.proc:  Optional[subprocess.Popen] = None
        self.reuse = os.environ.get("REUSE_RUNNING", "0") == "1"
        self.keep  = os.environ.get("KEEP_RUNNING",  "0") == "1"

    @property
    def mariadbd(self) -> str:
        return f"{self.prefix}/bin/mariadbd"

    @property
    def mariadb(self) -> str:
        return f"{self.prefix}/bin/mariadb"

    @property
    def plugin_dir(self) -> str:
        return f"{self.prefix}/lib/plugin"

    def start(self) -> None:
        if self.reuse:
            if not Path(SOCK).is_socket() and not os.path.exists(SOCK):
                print(f"runner: REUSE_RUNNING=1 but no socket at {SOCK}",
                      file=sys.stderr)
                sys.exit(2)
            print(f"runner: reusing existing server at {SOCK}")
            return

        # Install the plugin into MariaDB's plugin dir.
        try:
            shutil.copyfile(self.plugin,
                            f"{self.plugin_dir}/ha_stoolap.so")
        except (PermissionError, OSError) as e:
            print(f"runner: cannot install plugin to {self.plugin_dir}: {e}",
                  file=sys.stderr)
            print("runner: try sudo or REUSE_RUNNING=1", file=sys.stderr)
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
                [f"{self.prefix}/bin/mariadb-install-db",
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
        ]
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
        dsn=os.environ.get("STOOLAP_DSN", "memory://"))
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

    try:
        for case in cases:
            print(f"\n========== {case.stem} ==========")
            db = case.stem.replace("case_", "stoolap_test_")
            harness = Harness(socket=SOCK, db=db,
                              mariadb_bin=server.mariadb,
                              errlog_path=ERRLOG)
            try:
                mod = load_case(case)
                mod.run(harness)
                harness.summary(case.stem)
            except Exception as e:                 # pylint: disable=broad-except
                # Mirror bash's "subshell rc != 0" rollup.
                print(f"runner: {case.stem} raised: {e!r}",
                      file=sys.stderr)
                failed_files.append(case.stem)
                harness.failed += 1
            finally:
                total_pass += harness.passed
                total_fail += harness.failed
                total_skip += harness.skipped
                if harness.failed > 0 and case.stem not in failed_files:
                    failed_files.append(case.stem)
                harness.close()
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
