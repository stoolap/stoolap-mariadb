# Finds MariaDB server-internal headers needed to build a storage engine.
#
# Probe order:
#   1. MariaDB_ROOT (modern explicit override)
#   2. MARIADB_PREFIX (legacy project override)
#   3. Homebrew ARM / Intel mariadb@11.4
#   4. Debian/Ubuntu apt layout
#   5. Red Hat/Fedora layout
#
# Exports:
#   MariaDB_FOUND
#   MariaDB_ROOT
#   MariaDB_INCLUDE_DIRS
#   MariaDB_PLUGIN_DIR
#   MariaDB_CONFIG

include(FindPackageHandleStandardArgs)

set(_MariaDB_ROOT_CANDIDATES)

if(DEFINED MariaDB_ROOT)
    list(APPEND _MariaDB_ROOT_CANDIDATES "${MariaDB_ROOT}")
endif()

if(DEFINED MARIADB_PREFIX)
    # Backwards-compatible alias for existing build scripts. Prefer
    # -DMariaDB_ROOT= for new callers, but do not warn yet.
    list(APPEND _MariaDB_ROOT_CANDIDATES "${MARIADB_PREFIX}")
endif()

list(APPEND _MariaDB_ROOT_CANDIDATES
    "/opt/homebrew/opt/mariadb@11.4"
    "/usr/local/opt/mariadb@11.4"
)

foreach(_root IN LISTS _MariaDB_ROOT_CANDIDATES)
    if(NOT MariaDB_FOUND AND
       EXISTS "${_root}/include/mysql/server/private/handler.h")
        set(MariaDB_FOUND TRUE)
        set(MariaDB_ROOT "${_root}")
        set(MariaDB_INCLUDE_DIRS
            "${_root}/include/mysql/server"
            "${_root}/include/mysql/server/private")
    endif()
endforeach()

if(NOT MariaDB_FOUND AND
   EXISTS "/usr/include/mysql/server/private/handler.h")
    set(MariaDB_FOUND TRUE)
    set(MariaDB_ROOT "/usr")
    set(MariaDB_INCLUDE_DIRS
        "/usr/include/mysql/server"
        "/usr/include/mysql/server/private")
endif()

if(NOT MariaDB_FOUND AND
   EXISTS "/usr/include/mariadb/server/private/handler.h")
    set(MariaDB_FOUND TRUE)
    set(MariaDB_ROOT "/usr")
    set(MariaDB_INCLUDE_DIRS
        "/usr/include/mariadb/server"
        "/usr/include/mariadb/server/private")
endif()

find_program(MariaDB_CONFIG
    NAMES mariadb_config mysql_config
    HINTS "${MariaDB_ROOT}/bin" /opt/homebrew/opt/mariadb@11.4/bin
          /usr/local/opt/mariadb@11.4/bin /opt/homebrew/bin /usr/local/bin
)

if(MariaDB_CONFIG)
    if(NOT MariaDB_FOUND)
        execute_process(COMMAND "${MariaDB_CONFIG}" --prefix
                        OUTPUT_VARIABLE _mariadb_config_prefix
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(EXISTS
           "${_mariadb_config_prefix}/include/mysql/server/private/handler.h")
            set(MariaDB_FOUND TRUE)
            set(MariaDB_ROOT "${_mariadb_config_prefix}")
            set(MariaDB_INCLUDE_DIRS
                "${_mariadb_config_prefix}/include/mysql/server"
                "${_mariadb_config_prefix}/include/mysql/server/private")
        endif()
    endif()
    execute_process(COMMAND "${MariaDB_CONFIG}" --plugindir
                    OUTPUT_VARIABLE MariaDB_PLUGIN_DIR
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()

if(NOT MariaDB_PLUGIN_DIR)
    foreach(_plugin_dir
            "${MariaDB_ROOT}/lib/plugin"
            "/usr/lib/mysql/plugin"
            "/usr/lib64/mysql/plugin"
            "/usr/lib/mariadb/plugin"
            "/usr/lib64/mariadb/plugin")
        if(EXISTS "${_plugin_dir}")
            set(MariaDB_PLUGIN_DIR "${_plugin_dir}")
            break()
        endif()
    endforeach()
endif()

find_package_handle_standard_args(MariaDB
    REQUIRED_VARS MariaDB_ROOT MariaDB_INCLUDE_DIRS
    FAIL_MESSAGE
        "Could not find MariaDB server headers. Set -DMariaDB_ROOT=/path/to/prefix containing include/mysql/server/private/handler.h.")

mark_as_advanced(MariaDB_CONFIG MariaDB_PLUGIN_DIR)
