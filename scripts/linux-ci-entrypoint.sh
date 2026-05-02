#!/bin/bash
# Drop-in for the CI Linux flow: build the plugin against /staging's
# libstoolap.so + stoolap.h, install both, run the test suite (or a
# filtered subset). Designed for the Dockerfile.linux-ci image; not
# meant to run on a host directly.
set -euo pipefail

FILTER="${1:-}"

if [ ! -f /staging/target/release/libstoolap.so ]; then
    echo "/staging/target/release/libstoolap.so missing" >&2
    echo "  pull via: gh run download <run-id> --name stoolap-main-x86_64-unknown-linux-gnu --dir /tmp/stoolap-ci-artifact" >&2
    exit 2
fi

# Stage the layout the plugin's CMakeLists expects.
rm -rf /tmp/stoolap-fake
mkdir -p /tmp/stoolap-fake/include /tmp/stoolap-fake/target/release
cp /staging/include/stoolap.h /tmp/stoolap-fake/include/
cp /staging/target/release/libstoolap.so /tmp/stoolap-fake/target/release/

# Install libstoolap to the system loader path (matches CI).
install -m 0755 /staging/target/release/libstoolap.so /usr/local/lib/
ldconfig

# Build the plugin in a fresh dir so old caches can't mask issues.
rm -rf /tmp/build
cmake -S /work -B /tmp/build -DSTOOLAP_DIR=/tmp/stoolap-fake >/tmp/cmake-cfg.log
cmake --build /tmp/build -j"$(nproc)"

# CI install step.
PLUGIN_DIR=/usr/lib/mysql/plugin
[ -d "$PLUGIN_DIR" ] || PLUGIN_DIR=/usr/lib/mariadb/plugin
install -m 0755 /tmp/build/ha_stoolap.so "$PLUGIN_DIR/"

# Same env vars the runner.py expects (mirror ci.yml).
export MARIADB_PREFIX=/usr
export MARIADBD_BIN=/usr/sbin/mariadbd
export MARIADB_BIN=/usr/bin/mariadb
export MARIADB_INSTALL_DB_BIN=/usr/bin/mariadb-install-db
export MARIADB_PLUGIN_DIR="$PLUGIN_DIR"
export STOOLAP_PLUGIN=/tmp/build/ha_stoolap.so

echo "=== format ==="
clang-format-18 --dry-run --Werror /work/src/*.cc /work/src/*.h && echo "format OK"

echo "=== tests ==="
cd /work
exec python3 tests/runner.py "$FILTER"
