#!/usr/bin/env bash
set -euo pipefail

cmake --build build -j
clang-format --dry-run --Werror src/*.{cc,h}
python3 tests/runner.py
trap 'if [ -f /tmp/stoolap-test.pid ]; then kill "$(cat /tmp/stoolap-test.pid)" 2>/dev/null || true; fi' EXIT
KEEP_RUNNING=1 python3 tests/runner.py 14_scale
python3 benchmarks/bench.py --iterations 50 --runs 3 --mode warn
