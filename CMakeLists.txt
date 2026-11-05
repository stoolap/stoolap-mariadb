cmake_minimum_required(VERSION 3.16)
project(stoolap_mariadb CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release. The bridge does enough small-allocation / Field /
# string work per row that an unoptimized -O0 plugin can run several times
# slower than the same code at -O3, which would silently make stoolap look
# slower than it is in benchmarks. Pass `-DCMAKE_BUILD_TYPE=Debug` to
# override for development.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Default build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# Inputs
#
# MariaDB_ROOT    MariaDB install prefix. MARIADB_PREFIX is accepted as a
#                 backwards-compatible alias.
# STOOLAP_DIR     Optional: a local stoolap checkout to build against
#                 instead of the git dependency in rust/Cargo.toml.
# ---------------------------------------------------------------------------

find_package(MariaDB REQUIRED)
set(MARIADB_PREFIX "${MariaDB_ROOT}")
set(MARIADB_INCLUDE_DIRS ${MariaDB_INCLUDE_DIRS})
set(MARIADB_PLUGIN_DIR "${MariaDB_PLUGIN_DIR}")
message(STATUS "MariaDB prefix: ${MARIADB_PREFIX}")

# ---------------------------------------------------------------------------
# The engine: rust/ is the plugin's Rust crate, which links stoolap as a
# Rust crate and is linked into ha_stoolap.so as a static library.
# ---------------------------------------------------------------------------

find_program(CARGO cargo REQUIRED)
set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
set(RUST_STATICLIB
    "${RUST_TARGET_DIR}/release/${CMAKE_STATIC_LIBRARY_PREFIX}stoolap_mariadb${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(CARGO_ARGS build --release --manifest-path "${CMAKE_SOURCE_DIR}/rust/Cargo.toml"
    --target-dir "${RUST_TARGET_DIR}")
# A PATH cache entry: a relative -DSTOOLAP_DIR becomes absolute against
# the directory cmake ran in, not rust/ where cargo runs.
set(STOOLAP_DIR "" CACHE PATH "Local stoolap checkout to build against")
if(STOOLAP_DIR)
    message(STATUS "stoolap: local checkout ${STOOLAP_DIR}")
    list(APPEND CARGO_ARGS --config
        "patch.'https://github.com/stoolap/stoolap'.stoolap.path='${STOOLAP_DIR}'")
else()
    message(STATUS "stoolap: git main (rust/Cargo.toml)")
endif()
# Always runs: cargo decides what is out of date.
add_custom_target(stoolap_mariadb_rust ALL
    COMMAND ${CARGO} ${CARGO_ARGS}
    BYPRODUCTS "${RUST_STATICLIB}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/rust"
    COMMENT "Building the Rust crate (cargo)"
    USES_TERMINAL
    VERBATIM)
add_library(stoolap_mariadb_rs STATIC IMPORTED)
set_target_properties(stoolap_mariadb_rs PROPERTIES
    IMPORTED_LOCATION "${RUST_STATICLIB}")
add_dependencies(stoolap_mariadb_rs stoolap_mariadb_rust)

# ---------------------------------------------------------------------------
# Compile flags expected by MariaDB plugin headers
# ---------------------------------------------------------------------------

add_compile_definitions(
    MYSQL_DYNAMIC_PLUGIN
    HAVE_CONFIG_H
    DBUG_OFF      # match release-build server: no _db_keyword_ etc.
    NDEBUG
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(
        -fno-strict-aliasing
        -Wall
        -Wno-unused-parameter
    )
endif()

# ---------------------------------------------------------------------------
# Plugin target: ha_stoolap.so
# ---------------------------------------------------------------------------

add_library(ha_stoolap MODULE
    src/ha_stoolap.cc
    src/ha_stoolap_select.cc
    src/stoolap_sqlgen.cc
    src/stoolap_bridge.cc
    src/stoolap_thd_context.cc
    src/stoolap_thd_inspect.cc
)

# Brew's MariaDB 11.4 dev tarball ships sql_class.h but omits the wsrep-lib
# / wsrep-API headers it transitively requires (the actual `wsrep::` C++
# types live in a separate upstream repo MariaDB pulls as a submodule).
# Without these the select_handler subclass won't even parse, so we vendor
# the pinned-version headers under third_party/ and extend the include path.
set(WSREP_LIB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/wsrep-lib/include")
set(WSREP_API_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/wsrep-API")

# The vendored headers are wsrep-lib 5eeef40 (MariaDB 11.4.13's pin). It
# added the pure virtual client_service::notify_state_change(), which
# 11.4.13+ servers override and older 11.4 servers don't implement, so
# the declaration is gated on the server version (see third_party/README.md).
find_file(MARIADB_MYSQL_VERSION_H mysql_version.h
    PATHS ${MARIADB_INCLUDE_DIRS} NO_DEFAULT_PATH)
if(NOT MARIADB_MYSQL_VERSION_H)
    message(FATAL_ERROR "mysql_version.h not found in ${MARIADB_INCLUDE_DIRS}")
endif()
file(STRINGS "${MARIADB_MYSQL_VERSION_H}" _mariadb_version_line
    REGEX "^#define[ \t]+MYSQL_VERSION_ID[ \t]+[0-9]+")
string(REGEX MATCH "[0-9]+$" MARIADB_VERSION_ID "${_mariadb_version_line}")
message(STATUS "MariaDB version id: ${MARIADB_VERSION_ID}")
if(MARIADB_VERSION_ID VERSION_LESS 110400 OR
   MARIADB_VERSION_ID VERSION_GREATER_EQUAL 110500)
    message(WARNING "Only MariaDB 11.4.x is supported (found ${MARIADB_VERSION_ID})")
endif()
if(MARIADB_VERSION_ID VERSION_GREATER_EQUAL 110413)
    target_compile_definitions(ha_stoolap PRIVATE
        WSREP_LIB_HAS_NOTIFY_STATE_CHANGE)
endif()

target_include_directories(ha_stoolap PRIVATE
    ${WSREP_LIB_INCLUDE_DIR}
    ${WSREP_API_INCLUDE_DIR}
    ${MARIADB_INCLUDE_DIRS}
    "${CMAKE_SOURCE_DIR}/rust/include"
    src
)

add_dependencies(ha_stoolap stoolap_mariadb_rust)
# The system libraries Rust's std needs (rustc --print native-static-libs).
if(APPLE)
    target_link_libraries(ha_stoolap PRIVATE stoolap_mariadb_rs
        iconv "-framework CoreFoundation" c m)
    target_link_options(ha_stoolap PRIVATE -Wl,-dead_strip)
else()
    target_link_libraries(ha_stoolap PRIVATE stoolap_mariadb_rs
        gcc_s util rt pthread m dl c)
    target_link_options(ha_stoolap PRIVATE -Wl,--gc-sections)
endif()

set_target_properties(ha_stoolap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ha_stoolap"
)

if(APPLE)
    set_target_properties(ha_stoolap PROPERTIES
        SUFFIX ".so"
    )
    # Server symbols (handler, sql_print_*, etc.) are resolved by mariadbd at
    # dlopen() time; defer them past static link.
    target_link_options(ha_stoolap PRIVATE -undefined dynamic_lookup)
    # Re-sign with an explicit ad-hoc codesign. ld's "linker-signed" cdhash
    # (flags=0x20002) is rejected by macOS 26.x at dlopen with SIGKILL "Code
    # Signature Invalid"; only an explicit `codesign --sign -` adhoc
    # (flags=0x2) loads cleanly. Strip first to drop the linker hash, then
    # resign so the cdhash is recomputed.
    add_custom_command(TARGET ha_stoolap POST_BUILD
        COMMAND codesign --remove-signature $<TARGET_FILE:ha_stoolap>
        COMMAND codesign --sign - $<TARGET_FILE:ha_stoolap>
        COMMENT "Re-signing plugin with adhoc codesign (macOS dyld requirement)"
        VERBATIM
    )
else()
    # Defer ALL unresolved server symbols (handler::ha_thd, sql_print_*,
    # thd_test_options, my_free, etc.) to dlopen() time inside mariadbd.
    # `ignore-in-shared-libs` only relaxes symbols imported via -lfoo
    # entries; the server symbols are referenced directly from our .o
    # files and need `ignore-all` to skip the link-time check. Modern
    # Ubuntu's default linker policy is --no-undefined for shared
    # modules, which surfaces these as link errors without this flag.
    target_link_options(ha_stoolap PRIVATE -Wl,--unresolved-symbols=ignore-all)
endif()

# ---------------------------------------------------------------------------
# Optional install target: copies the plugin into MariaDB's plugin_dir.
# Defaults to mariadb_config --plugindir.
# ---------------------------------------------------------------------------

if(MARIADB_PLUGIN_DIR)
    message(STATUS "Plugin dir (install target): ${MARIADB_PLUGIN_DIR}")
    install(TARGETS ha_stoolap LIBRARY DESTINATION "${MARIADB_PLUGIN_DIR}")
endif()
