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
# STOOLAP_DIR     Path to the stoolap repo root.
# STOOLAP_LIB_DIR Directory holding libstoolap.{dylib,so}; defaults to
#                 ${STOOLAP_DIR}/target/release.
# ---------------------------------------------------------------------------

find_package(MariaDB REQUIRED)
set(MARIADB_PREFIX "${MariaDB_ROOT}")
set(MARIADB_INCLUDE_DIRS ${MariaDB_INCLUDE_DIRS})
set(MARIADB_PLUGIN_DIR "${MariaDB_PLUGIN_DIR}")
message(STATUS "MariaDB prefix: ${MARIADB_PREFIX}")

if(NOT DEFINED STOOLAP_DIR)
    set(STOOLAP_DIR "${CMAKE_SOURCE_DIR}/../stoolap")
endif()

if(NOT DEFINED STOOLAP_LIB_DIR)
    set(STOOLAP_LIB_DIR "${STOOLAP_DIR}/target/release")
endif()

if(NOT EXISTS "${STOOLAP_DIR}/include/stoolap.h")
    message(FATAL_ERROR
        "stoolap.h not found at ${STOOLAP_DIR}/include/stoolap.h. "
        "Set -DSTOOLAP_DIR= to the stoolap repo root.")
endif()

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

target_include_directories(ha_stoolap PRIVATE
    ${WSREP_LIB_INCLUDE_DIR}
    ${WSREP_API_INCLUDE_DIR}
    ${MARIADB_INCLUDE_DIRS}
    "${STOOLAP_DIR}/include"
    src
)

target_link_directories(ha_stoolap PRIVATE
    ${STOOLAP_LIB_DIR}
)

target_link_libraries(ha_stoolap PRIVATE
    stoolap
)

set_target_properties(ha_stoolap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ha_stoolap"
)

if(APPLE)
    set_target_properties(ha_stoolap PROPERTIES
        SUFFIX ".so"
        INSTALL_RPATH "@loader_path;${STOOLAP_LIB_DIR}"
        BUILD_WITH_INSTALL_RPATH TRUE
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
    set_target_properties(ha_stoolap PROPERTIES
        INSTALL_RPATH "$ORIGIN;${STOOLAP_LIB_DIR}"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
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
