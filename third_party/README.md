# Vendored upstream headers

These headers are vendored to make the plugin buildable against Homebrew's
MariaDB 11.4 distribution, which ships server-side headers but omits the
`wsrep` dev headers that `sql_class.h` (and the entire `select_handler.h`
include cascade) transitively requires.

## wsrep-lib/

Source: <https://github.com/codership/wsrep-lib> commit
`7010f0ab584ab9cdebb285272a0fb0ff0a5a791d` (the commit pinned by MariaDB
11.4.10).

License: GPL-2.0 (see `wsrep-lib/COPYING` and `wsrep-lib/LICENSE`).

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
