# Vendored upstream headers

These headers are vendored to make the plugin buildable against Homebrew's
MariaDB 11.4 distribution, which ships server-side headers but omits the
`wsrep` dev headers that `sql_class.h` (and the entire `select_handler.h`
include cascade) transitively requires.

## wsrep-lib/

Source: <https://github.com/codership/wsrep-lib> commit
`5eeef4009d52455c44b607e852cb23fbb6e84c99` (the commit pinned by MariaDB
11.4.13).

License: GPL-2.0 (see `wsrep-lib/COPYING` and `wsrep-lib/LICENSE`).

Local patch: `client_service.hpp` declares the pure virtual
`notify_state_change()` (new in this commit) only under
`WSREP_LIB_HAS_NOTIFY_STATE_CHANGE`, which `CMakeLists.txt` defines for
MariaDB 11.4.13+. MariaDB 11.4.10 - 11.4.12 pin wsrep-lib `7010f0a`, which
lacks it; their `Wsrep_client_service` doesn't implement it and would
otherwise become abstract.

Only the public `include/wsrep/*.hpp` files are vendored. Source files,
tests, and CMake glue are not.

## wsrep-API/

Source: <https://github.com/codership/wsrep-API> commit
`65608d3f503ba9f4c170fc4e01c539be9fafd46c` (the commit pinned by
wsrep-lib at the version above).

License: GPL-2.0-only (see `wsrep-API/COPYING`). The current upstream
distribution headers carry the same GPL-2.0 notice as wsrep-lib.

Only the public `*.h` files are vendored.

## Why vendor?

These headers are ABI-stable enough that the plugin's `select_handler`
subclass compiles against the brewed server's binary without surprises,
and avoiding a build-time clone keeps the build offline-friendly.
