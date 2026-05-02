#!/usr/bin/env bash
set -euo pipefail

cmake --build build -j
# Pin clang-format-18 to match CI; fall back to plain `clang-format` if
# only one is installed (most contributor machines have one or the other).
if command -v clang-format-18 >/dev/null 2>&1; then
    clang-format-18 --dry-run --Werror src/*.{cc,h}
else
    clang-format --dry-run --Werror src/*.{cc,h}
fi
python3 tests/runner.py
trap 'if [ -f /tmp/stoolap-test.pid ]; then kill "$(cat /tmp/stoolap-test.pid)" 2>/dev/null || true; fi' EXIT
KEEP_RUNNING=1 python3 tests/runner.py 14_scale
python3 benchmarks/bench.py --iterations 50 --runs 3 --mode warn
