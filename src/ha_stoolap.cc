/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#define MYSQL_SERVER 1

#include "ha_stoolap.h"

#include "my_global.h"
#include "mysql/plugin.h"
#include "field.h"
#include "key.h"
#include "sql_priv.h"  // OPTION_NOT_AUTOCOMMIT, OPTION_BEGIN
#include "sql_string.h"
#include "log.h"
#include "mysqld_error.h"  // ER_GET_ERRMSG (used by report_stoolap_error)

#include "stoolap_thd_context.h"
#include "stoolap_packet.h"
#include "ha_stoolap_select.h"
#include "stoolap_thd_inspect.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

// MariaDB exposes the original SQL text via this server-side function.
// We declare it locally to avoid pulling in sql_class.h (which would drag
// wsrep headers brew doesn't ship).
extern "C" LEX_STRING* thd_query_string(MYSQL_THD thd);

// Externally visible so the direct DML helper in ha_stoolap_select.cc can
// gate its eligibility check on `tl->table->file->ht == stoolap_hton`.
handlerton* stoolap_hton = nullptr;

// Process-wide engine handle. ha_stoolap_select.cc reaches in here so the
// direct UPDATE/DELETE path can clone a thread-local handle on demand.
stoolap_mariadb::Engine g_engine;

/* ---------- Per-THD context ---------- */

// External linkage on purpose: ha_stoolap_select.cc's pushdown factories
// call register_trx() defensively before the eager query, since
// create_select can fire before any handler's external_lock when we're
// installed as the whole-statement executor.
stoolap_mariadb::ThdContext* get_thd_ctx(THD* thd) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    return ctx;
}

/**
 * Register the engine in the current statement's transaction list and start
 * a stoolap Tx if the THD is inside an explicit BEGIN block. Auto-commit
 * statements skip the Tx so each DML stays as a stoolap auto-commit exec.
 *
 * Picks the stoolap isolation level off the THD's @@tx_isolation:
 *   REPEATABLE-READ / SERIALIZABLE  -> STOOLAP_ISOLATION_SNAPSHOT
 *   READ-COMMITTED  / READ-UNCOMMITTED -> STOOLAP_ISOLATION_READ_COMMITTED
 * Without this, MariaDB's default REPEATABLE-READ tx silently ran as
 * read-committed and saw post-BEGIN commits from other sessions.
 */
int register_trx(THD* thd) {
    auto* ctx = get_thd_ctx(thd);

    trans_register_ha(thd, /*all=*/false, stoolap_hton, /*flags=*/0);

    const bool in_explicit_txn =
        thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (in_explicit_txn) {
        trans_register_ha(thd, /*all=*/true, stoolap_hton, /*flags=*/0);
        if (!ctx->has_tx()) {
            const int iso = thd_tx_isolation(thd);
            const int32_t stoolap_iso =
                (iso == ISO_REPEATABLE_READ || iso == ISO_SERIALIZABLE)
                    ? STOOLAP_ISOLATION_SNAPSHOT
                    : STOOLAP_ISOLATION_READ_COMMITTED;
            int rc = ctx->begin(stoolap_iso);
            if (rc != STOOLAP_OK) {
                sql_print_error("stoolap: BEGIN failed: %s",
                                stoolap_errmsg(ctx->db()));
                return HA_ERR_GENERIC;
            }
        }
    }
    return 0;
}

namespace {

/** System variable: DSN passed to stoolap_open() at plugin init. */
char* stoolap_dsn_var = nullptr;

handler* stoolap_create_handler(handlerton* hton, TABLE_SHARE* table,
                                MEM_ROOT* mem_root) {
    return new (mem_root) ha_stoolap(hton, table);
}

/* ---------- Handlerton callbacks ---------- */

int stoolap_commit_cb(handlerton* /*hton*/, THD* thd, bool all) {
    // Real COMMIT is signalled either by `all=true` (explicit COMMIT) or by
    // the end-of-statement marker for an autocommit session.
    const bool real_commit =
        all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
    if (!real_commit) return 0;

    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx || !ctx->has_tx()) return 0;

    int rc = ctx->commit();
    if (rc != STOOLAP_OK) {
        // stoolap_tx_commit consumes the tx handle, so we can't query the
        // error message after it returns. The dominant cause of a
        // commit-time failure is concurrent-write rollback, so map to
        // ER_LOCK_DEADLOCK (40001) — the SQLSTATE class apps know to retry.
        const char* msg = stoolap_errmsg(ctx->db());
        sql_print_error(
            "stoolap: COMMIT failed: %s",
            (msg && *msg) ? msg : "(no detail; tx already consumed)");
        return HA_ERR_LOCK_DEADLOCK;
    }
    return 0;
}

int stoolap_rollback_cb(handlerton* /*hton*/, THD* thd, bool all) {
    const bool real_rollback =
        all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
    if (!real_rollback) return 0;

    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx || !ctx->has_tx()) return 0;

    int rc = ctx->rollback();
    if (rc != STOOLAP_OK) {
        sql_print_error("stoolap: ROLLBACK failed: %s",
                        stoolap_errmsg(ctx->db()));
        return HA_ERR_GENERIC;
    }
    return 0;
}

int stoolap_close_connection_cb(handlerton* /*hton*/, THD* thd) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (ctx) {
        delete ctx;
        thd_set_ha_data(thd, stoolap_hton, nullptr);
    }
    return 0;
}

// START TRANSACTION WITH CONSISTENT SNAPSHOT runs this hook for every
// engine. Without it the tx wouldn't open until the first statement's
// external_lock fires, so a concurrent commit between BEGIN and the
// first SELECT would slide into our snapshot. Open the stoolap tx now
// so the snapshot is anchored at BEGIN time, the way InnoDB does.
int stoolap_start_consistent_snapshot_cb(handlerton* /*hton*/, THD* thd) {
    auto* ctx = get_thd_ctx(thd);
    if (ctx->has_tx()) return 0;  // already open
    int rc = ctx->begin(STOOLAP_ISOLATION_SNAPSHOT);
    if (rc != STOOLAP_OK) {
        sql_print_error("stoolap: consistent snapshot BEGIN failed: %s",
                        stoolap_errmsg(ctx->db()));
        return HA_ERR_GENERIC;
    }
    trans_register_ha(thd, /*all=*/false, stoolap_hton, /*flags=*/0);
    trans_register_ha(thd, /*all=*/true, stoolap_hton, /*flags=*/0);
    return 0;
}

/* ---------- Tx-aware execution helpers ---------- */

// Pick the StoolapDB the auto-commit path should use. The per-handler db_
// is reset every time MariaDB closes the handler instance (which can be
// once per statement in tight benchmark loops); each clone has its own
// executor and parsed-query cache, so routing through it leaves stoolap's
// semantic cache cold across statements. The THD context's db() is
// cloned once per connection and lives for the connection's lifetime, so
// repeated INSERT/SELECT/UPDATE templates from the same session keep
// hitting a warm parse cache.
static StoolapDB* warm_db(stoolap_mariadb::ThdContext* ctx,
                          StoolapDB* fallback) {
    if (ctx) {
        if (StoolapDB* d = ctx->db()) return d;
    }
    return fallback;
}

int exec_via(stoolap_mariadb::ThdContext* ctx, StoolapDB* fallback,
             const char* sql, const StoolapValue* params, int32_t nparams,
             int64_t* affected) {
    if (ctx && ctx->has_tx()) {
        return stoolap_tx_exec_params(ctx->tx(), sql, params, nparams,
                                      affected);
    }
    return stoolap_exec_params(warm_db(ctx, fallback), sql, params, nparams,
                               affected);
}

int query_via(stoolap_mariadb::ThdContext* ctx, StoolapDB* fallback,
              const char* sql, StoolapRows** out_rows) {
    if (ctx && ctx->has_tx()) {
        return stoolap_tx_query(ctx->tx(), sql, out_rows);
    }
    return stoolap_query(warm_db(ctx, fallback), sql, out_rows);
}

int query_params_via(stoolap_mariadb::ThdContext* ctx, StoolapDB* fallback,
                     const char* sql, const StoolapValue* params,
                     int32_t nparams, StoolapRows** out_rows) {
    if (ctx && ctx->has_tx()) {
        return stoolap_tx_query_params(ctx->tx(), sql, params, nparams,
                                       out_rows);
    }
    return stoolap_query_params(warm_db(ctx, fallback), sql, params, nparams,
                                out_rows);
}

const char* errmsg_via(stoolap_mariadb::ThdContext* ctx, StoolapDB* fallback) {
    if (ctx && ctx->has_tx()) return stoolap_tx_errmsg(ctx->tx());
    return stoolap_errmsg(warm_db(ctx, fallback));
}

}  // namespace (close the anonymous namespace so the helpers below
// have external linkage and ha_stoolap_select.cc can call them
// for direct UPDATE / DELETE error mapping)

/**
 * Find the index whose first key part is the named column. Returns MAX_KEY
 * if no match. Used to translate stoolap "unique constraint failed ... on
 * column X" errors into a MariaDB key index for `errkey`, which ON
 * DUPLICATE KEY UPDATE and REPLACE need to look up the conflicting row.
 *
 * Composite UNIQUE is refused at CREATE (see build_create_sql), so every
 * KEY here has exactly one user-defined part; matching against
 * `key_part[0].field` is sufficient. If composite UNIQUE ever lands,
 * this loop should walk every key part.
 */
uint find_key_for_column(TABLE_SHARE* share, std::string_view col) {
    if (!share) return MAX_KEY;
    for (uint i = 0; i < share->keys; ++i) {
        KEY& k = share->key_info[i];
        if (k.user_defined_key_parts == 0) continue;
        Field* f = k.key_part[0].field;
        if (f && col.size() == f->field_name.length &&
            std::memcmp(col.data(), f->field_name.str, col.size()) == 0) {
            return i;
        }
    }
    return MAX_KEY;
}

/**
 * Extract a quoted or unquoted token from `msg` starting at `pos`. The
 * stoolap error strings use single-quotes around column names ("column
 * 'pid' in table 'cfk'") and unquoted bareword tokens for other places
 * ("on column u with value", "for index unique_t_0"). Returns the bare
 * token (no surrounding quotes), advancing nothing of the caller's state.
 */
std::string_view extract_token(std::string_view msg, size_t pos) {
    if (pos >= msg.size()) return {};
    if (msg[pos] == '\'') {
        size_t end = msg.find('\'', pos + 1);
        if (end == std::string_view::npos) return {};
        return msg.substr(pos + 1, end - pos - 1);
    }
    size_t end = pos;
    while (end < msg.size() &&
           !std::isspace(static_cast<unsigned char>(msg[end])) &&
           msg[end] != '\'' && msg[end] != ',' && msg[end] != ':') {
        ++end;
    }
    return msg.substr(pos, end - pos);
}

/**
 * Inspect a stoolap constraint error and figure out which MariaDB key
 * index was violated, so the handler can set `errkey`. Stoolap's UNIQUE
 * error format (from ../stoolap/src/core/error.rs):
 *   "unique constraint failed for index NAME on column COL with value V"
 * Stoolap's PK error format:
 *   "primary key constraint failed with ROWID already exists in this table"
 * Falls back to `primary_key` (or 0) when the message can't be parsed.
 */
uint guess_errkey(const char* msg, TABLE_SHARE* share) {
    auto starts_with = [](std::string_view s, std::string_view prefix) {
        return s.size() >= prefix.size() &&
               s.compare(0, prefix.size(), prefix) == 0;
    };
    if (msg && share) {
        std::string_view m(msg);
        if (starts_with(m, "primary key constraint failed")) {
            if (share->primary_key < share->keys) return share->primary_key;
        } else if (starts_with(m, "unique constraint failed")) {
            const auto col = m.find("on column ");
            if (col != std::string_view::npos) {
                std::string_view name =
                    extract_token(m, col + sizeof("on column ") - 1);
                if (!name.empty()) {
                    uint k = find_key_for_column(share, name);
                    if (k != MAX_KEY) return k;
                }
            }
        }
    }
    if (share && share->primary_key < share->keys) return share->primary_key;
    return 0;
}

/**
 * Map a stoolap error message to a MariaDB handler error code.
 *
 * Source of truth: stoolap's `Error` enum in
 * ../stoolap/src/core/error.rs. Each pattern below is anchored at the
 * known position in the canonical format string. Wording drift on
 * stoolap's side surfaces as a bumped `Stoolap_unmapped_errors` status
 * counter (see report_stoolap_error) and a degraded ER_GET_ERRMSG
 * (1296) to the user — the message text still reaches the client, only
 * the error number is generic. Tests in case_18_error_mapping.py pin
 * the round-trip from stoolap text to MariaDB errno; run them against
 * a stoolap upgrade to detect drift.
 */
int map_stoolap_error(const char* msg) {
    if (!msg) return HA_ERR_GENERIC;
    std::string_view m(msg);

    // Anchor enum: PREFIX matches at position 0, CONTAINS finds the
    // needle anywhere. Most stoolap errors are prefix-anchored on the
    // class name; CONTAINS is reserved for tail-message markers that
    // can't be anchored (e.g. the truncate-blocked variant of
    // "uncommitted changes" appears as the second clause).
    enum Anchor : uint8_t { PREFIX, CONTAINS };
    struct Pattern {
        std::string_view needle;
        int ha_err;
        Anchor anchor;
    };
    auto matches = [&](const Pattern& p) {
        if (p.anchor == PREFIX) {
            return m.size() >= p.needle.size() &&
                   m.compare(0, p.needle.size(), p.needle) == 0;
        }
        return m.find(p.needle) != std::string_view::npos;
    };

    // Order: most-specific first. The constraint violations and the
    // FK violation are anchored prefixes (no risk of cross-matching
    // each other's tail messages -- the FK message contains the words
    // "does not exist" which would otherwise route to NO_SUCH_TABLE).
    static constexpr Pattern kTable[] = {
        // ---- Constraint violations (DUP_KEY) ---------------------------
        {"primary key constraint failed", HA_ERR_FOUND_DUPP_KEY, PREFIX},
        {"unique constraint failed", HA_ERR_FOUND_DUPP_KEY, PREFIX},

        // ---- Foreign key violation -------------------------------------
        {"foreign key constraint violation", HA_ERR_NO_REFERENCED_ROW, PREFIX},

        // ---- NOT NULL / CHECK (no specific HA_ERR; surface as 1296
        //      with the stoolap text via report_stoolap_error). Keep
        //      anchored so future similar messages don't match by accident.
        {"not null constraint failed", HA_ERR_GENERIC, PREFIX},
        {"CHECK constraint failed", HA_ERR_GENERIC, PREFIX},

        // ---- Table / view lifecycle ------------------------------------
        // "table 'X' already exists" / "view 'X' already exists" /
        // "index 'X' already exists" all map to the same MariaDB error.
        {"' already exists", HA_ERR_TABLE_EXIST, CONTAINS},
        // "table 'X' not found" / "view 'X' not found" /
        // "table or view 'X' not found"; "doesn't exist" doesn't appear
        // in stoolap's error.rs but is plumbed in via MariaDB's frame.
        {"' not found", HA_ERR_NO_SUCH_TABLE, CONTAINS},

        // ---- Concurrency / locking -------------------------------------
        // "row N has uncommitted changes from transaction M" (write
        // conflict against an in-flight tx) and the truncate-blocked
        // variant "cannot truncate table: active transactions have
        // uncommitted changes". Both map to deadlock-class for client retry.
        {"uncommitted changes", HA_ERR_LOCK_DEADLOCK, CONTAINS},
        {"write conflict", HA_ERR_LOCK_DEADLOCK, CONTAINS},
        // "failed to acquire lock: ..." -- lock-wait timeout class. (We
        // don't currently distinguish DEADLOCK vs LOCK_WAIT_TIMEOUT
        // because stoolap doesn't expose the wait/abort distinction over
        // the FFI yet; both surface as 40001 SQLSTATE.)
        {"failed to acquire lock", HA_ERR_LOCK_DEADLOCK, PREFIX},

        // ---- Capability errors (stoolap declines a request that
        //      MariaDB asked for, e.g. "only DML supported in tx").
        //      Surface as ER_ILLEGAL_HA so apps can distinguish from
        //      runtime failures.
        {"not supported", HA_ERR_UNSUPPORTED, CONTAINS},
        {"unsupported", HA_ERR_UNSUPPORTED, CONTAINS},
    };
    for (const Pattern& p : kTable) {
        if (matches(p)) return p.ha_err;
    }
    return HA_ERR_GENERIC;
}

// Brewed mariadbd exports my_error / my_printf_error directly, but the
// plugin's `mysql/plugin.h` rewrites them to a service-pointer dispatch
// (`my_print_error_service->...`) that isn't shipped in this build. Undef
// the macros and declare the underlying symbols so we can hand a real
// stoolap message to the user instead of MariaDB's generic "Got error 168"
// fallback.
#undef my_error
#undef my_printf_error
extern "C" void my_error(unsigned int nr, unsigned long MyFlags, ...);
extern "C" void my_printf_error(unsigned int nr, const char* fmt,
                                unsigned long MyFlags, ...);

/**
 * Map+publish: classify a stoolap error message into a HA_ERR_* code and,
 * for the generic-class codes that MariaDB would otherwise print as
 * "Got error 168 \"Unknown (generic) error from engine\"", stash the real
 * stoolap text via my_printf_error so the client sees the underlying
 * cause. Specific HA_ERR_* codes (HA_ERR_FOUND_DUPP_KEY,
 * HA_ERR_NO_REFERENCED_ROW, HA_ERR_TABLE_EXIST, etc.) get descriptive
 * server-side messages from MariaDB itself, so we don't override those.
 */
int report_stoolap_error(const char* msg) {
    int rc = map_stoolap_error(msg);
    const bool needs_text = (rc == HA_ERR_GENERIC || rc == HA_ERR_UNSUPPORTED);
    if (needs_text && msg && *msg) {
        my_printf_error(ER_GET_ERRMSG, "stoolap: %s", MYF(0), msg);
    }
    // Telemetry: when map_stoolap_error degrades a non-empty stoolap
    // message to HA_ERR_GENERIC, the pattern table has fallen behind
    // stoolap's wording. Surfaced via SHOW STATUS LIKE
    // 'Stoolap_unmapped_errors'; the case_18 smoke test asserts this
    // counter stays at 0 across all known error classes, so a stoolap
    // upgrade that reworded one of them fails the test.
    //
    // HA_ERR_UNSUPPORTED maps cleanly via the "not supported" /
    // "unsupported" patterns, so it's NOT counted as unmapped even
    // though it shares the my_printf_error path.
    if (rc == HA_ERR_GENERIC && msg && *msg) {
        stoolap_mariadb::g_stats.unmapped_errors.fetch_add(
            1, std::memory_order_relaxed);
    }
    return rc;
}

namespace {

/**
 * Derive a stoolap-side table identifier from MariaDB's per-table path.
 * MariaDB passes paths like "./test/t1" -- we strip the "./" and need
 * an injective encoding of (db, tbl) since database and table names
 * can each contain '_' and '/'. Naive `/` -> `__` collides:
 *   db `p`,  tbl `q__r`     -> `p__q__r`
 *   db `p__q`, tbl `r`      -> `p__q__r`     (same flat name!)
 * Escape `_` as `_0` and `/` as `_1`. Decoding is unambiguous because
 * `_` only ever appears followed by `0` or `1`. Other path chars pass
 * through untouched. Stoolap CREATE on a colliding pair now produces
 * distinct backing names.
 */
std::string stoolap_table_from_path(const char* mariadb_path) {
    if (!mariadb_path) return std::string();
    std::string_view s(mariadb_path);
    if (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.remove_prefix(2);
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '_')
            out.append("_0");
        else if (c == '/')
            out.append("_1");
        else
            out.push_back(c);
    }
    return out;
}

/** Stoolap-side flat name from explicit (db, tbl). Same encoding as
 *  stoolap_table_from_path so CREATE-time names line up with FK
 *  target names and DML-time names. */
std::string stoolap_flat_name(std::string_view db, std::string_view tbl) {
    auto append_escaped = [](std::string& out, std::string_view s) {
        for (char c : s) {
            if (c == '_')
                out.append("_0");
            else if (c == '/')
                out.append("_1");
            else
                out.push_back(c);
        }
    };
    std::string out;
    out.reserve(db.size() + tbl.size() + 4);
    if (!db.empty()) {
        append_escaped(out, db);
        out.append("_1");
    }
    append_escaped(out, tbl);
    return out;
}

std::string quote_ident(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/** Map a MariaDB Field to a stoolap column type, or nullptr if unsupported. */
const char* stoolap_type_for(const Field* f) {
    switch (f->real_type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_BIT:  // packed bitmap, val_int() gives the integer
            return "INTEGER";
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            return "FLOAT";
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_NEWDECIMAL:  // round-trip via decimal string
        case MYSQL_TYPE_DATE:        // YYYY-MM-DD as text
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_TIME:  // HH:MM:SS[.ffffff] as text
        case MYSQL_TYPE_TIME2:
        case MYSQL_TYPE_ENUM:       // val_str() gives the label
        case MYSQL_TYPE_SET:        // val_str() gives "a,b,c"
        case MYSQL_TYPE_TINY_BLOB:  // TEXT family stored as BLOB type
        case MYSQL_TYPE_BLOB:       // ditto
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            return "TEXT";
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
            return "TIMESTAMP";
        default:
            return nullptr;
    }
}

/** Skip whitespace, return new position. */
size_t fk_skip_ws(std::string_view s, size_t p) {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
        ++p;
    return p;
}

/** Match keyword case-insensitively at position p with word boundaries.
 *  Returns position past the word, or npos. */
size_t fk_match_kw(std::string_view s, size_t p, std::string_view word) {
    if (p + word.size() > s.size()) return std::string_view::npos;
    for (size_t i = 0; i < word.size(); ++i) {
        char a = std::toupper(static_cast<unsigned char>(s[p + i]));
        char b = std::toupper(static_cast<unsigned char>(word[i]));
        if (a != b) return std::string_view::npos;
    }
    if (p > 0) {
        char pr = s[p - 1];
        if (std::isalnum(static_cast<unsigned char>(pr)) || pr == '_')
            return std::string_view::npos;
    }
    char nx = (p + word.size() < s.size()) ? s[p + word.size()] : ' ';
    if (std::isalnum(static_cast<unsigned char>(nx)) || nx == '_')
        return std::string_view::npos;
    return p + word.size();
}

/** Read an SQL identifier (bare word or `backticked`). Returns position
 *  past the identifier and stores the unescaped name in `out`. */
size_t fk_read_ident(std::string_view s, size_t p, std::string& out) {
    out.clear();
    if (p >= s.size()) return std::string_view::npos;
    if (s[p] == '`') {
        ++p;
        while (p < s.size() && s[p] != '`') {
            out.push_back(s[p++]);
        }
        if (p >= s.size()) return std::string_view::npos;
        return p + 1;
    }
    while (p < s.size() &&
           (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) {
        out.push_back(s[p++]);
    }
    return out.empty() ? std::string_view::npos : p;
}

/** Read "(col[, col...])" at position p. Stores the column list (without
 *  parens) verbatim in `out`. Returns position past the closing paren. */
size_t fk_read_paren_list(std::string_view s, size_t p, std::string& out) {
    if (p >= s.size() || s[p] != '(') return std::string_view::npos;
    int depth = 0;
    size_t open = p;
    for (; p < s.size(); ++p) {
        if (s[p] == '(')
            ++depth;
        else if (s[p] == ')') {
            if (--depth == 0) {
                out.assign(s.data() + open + 1, p - open - 1);
                return p + 1;
            }
        }
    }
    return std::string_view::npos;
}

/** Count comma separators in `s`, treating identifier whitespace as ignorable.
 *  Used to detect multi-column FK (stoolap supports single-column only). */
size_t count_commas(std::string_view s) {
    size_t n = 0;
    for (char c : s)
        if (c == ',') ++n;
    return n;
}

/**
 * Parse one FOREIGN KEY clause starting at `fk_pos` and append a stoolap-
 * compatible rewrite to `out`. The rewrite renames the parent table from
 * `tbl` (or `db.tbl`) to `db__tbl` so it matches our table-naming convention.
 *
 * Returns position past the consumed clause, or npos on parse failure.
 *
 * Skips (returns past-clause without appending) when the clause references
 * the current table (self-referencing FK — stoolap can't resolve the
 * parent until the table is fully created) or has multiple key parts
 * (stoolap's parser is single-column only).
 */
size_t copy_one_fk_clause(std::string_view sql, size_t fk_pos,
                          const std::string& current_db,
                          const std::string& current_tbl, std::string& out,
                          bool& unsupported) {
    // Confirm "FOREIGN KEY"
    size_t p = fk_match_kw(sql, fk_pos, "FOREIGN");
    if (p == std::string_view::npos) return std::string_view::npos;
    p = fk_skip_ws(sql, p);
    p = fk_match_kw(sql, p, "KEY");
    if (p == std::string_view::npos) return std::string_view::npos;

    // Optional index name before '(': skip it.
    p = fk_skip_ws(sql, p);
    if (p < sql.size() && sql[p] != '(') {
        std::string ignored;
        size_t np = fk_read_ident(sql, p, ignored);
        if (np == std::string_view::npos) return std::string_view::npos;
        p = fk_skip_ws(sql, np);
    }

    // Child column list
    std::string child_cols;
    p = fk_read_paren_list(sql, p, child_cols);
    if (p == std::string_view::npos) return std::string_view::npos;
    p = fk_skip_ws(sql, p);

    // REFERENCES
    p = fk_match_kw(sql, p, "REFERENCES");
    if (p == std::string_view::npos) return std::string_view::npos;
    p = fk_skip_ws(sql, p);

    // Parent table name: either `name`, name, or db.name (any quoting combo).
    std::string parent_db, parent_tbl;
    size_t np = fk_read_ident(sql, p, parent_tbl);
    if (np == std::string_view::npos) return std::string_view::npos;
    p = np;
    if (p < sql.size() && sql[p] == '.') {
        parent_db = std::move(parent_tbl);
        ++p;
        np = fk_read_ident(sql, p, parent_tbl);
        if (np == std::string_view::npos) return std::string_view::npos;
        p = np;
    }
    p = fk_skip_ws(sql, p);

    // Optional parent column list
    std::string parent_cols;
    if (p < sql.size() && sql[p] == '(') {
        p = fk_read_paren_list(sql, p, parent_cols);
        if (p == std::string_view::npos) return std::string_view::npos;
        p = fk_skip_ws(sql, p);
    }

    // Optional ON DELETE / ON UPDATE actions: extract verbatim.
    std::string actions;
    for (int i = 0; i < 2; ++i) {
        size_t after_on = fk_match_kw(sql, p, "ON");
        if (after_on == std::string_view::npos) break;
        size_t kwp = fk_skip_ws(sql, after_on);
        size_t after_event = fk_match_kw(sql, kwp, "DELETE");
        if (after_event == std::string_view::npos)
            after_event = fk_match_kw(sql, kwp, "UPDATE");
        if (after_event == std::string_view::npos) break;
        size_t aw = fk_skip_ws(sql, after_event);
        size_t end = aw;
        while (end < sql.size() &&
               (std::isalpha(static_cast<unsigned char>(sql[end])) ||
                sql[end] == ' '))
            ++end;
        while (end > aw &&
               std::isspace(static_cast<unsigned char>(sql[end - 1])))
            --end;
        if (!actions.empty()) actions.push_back(' ');
        actions += std::string_view(sql.data() + p, end - p);
        p = fk_skip_ws(sql, end);
    }

    // Reject multi-column FKs — stoolap's grammar is single-column only.
    // Silently dropping them used to let CREATE TABLE succeed and then
    // accept FK-violating child rows with no client-visible warning.
    if (count_commas(child_cols) > 0 || count_commas(parent_cols) > 0) {
        my_printf_error(ER_GET_ERRMSG,
                        "stoolap: composite FOREIGN KEY (%s) is not "
                        "supported -- stoolap enforces single-column "
                        "foreign keys only.",
                        MYF(0), child_cols.c_str());
        unsupported = true;
        return p;
    }
    // Reject self-referencing FK — stoolap looks up the parent table at
    // CREATE-time, before our just-being-created table is registered.
    const std::string& db = parent_db.empty() ? current_db : parent_db;
    std::string parent_full = stoolap_flat_name(db, parent_tbl);
    if (parent_full == current_tbl) {
        my_printf_error(ER_GET_ERRMSG,
                        "stoolap: self-referencing FOREIGN KEY on '%s' is "
                        "not supported -- stoolap resolves the parent at "
                        "CREATE-time, before the table is registered.",
                        MYF(0), current_tbl.c_str());
        unsupported = true;
        return p;
    }

    // Emit the rewritten clause. Parent table name uses our db__tbl form.
    out += ", FOREIGN KEY (";
    out += child_cols;
    out += ") REFERENCES ";
    out += quote_ident(parent_full);
    if (!parent_cols.empty()) {
        out += " (";
        out += parent_cols;
        out.push_back(')');
    }
    if (!actions.empty()) {
        out.push_back(' ');
        out += actions;
    }
    return p;
}

/** Scan the THD's current SQL for FOREIGN KEY clauses and append them to
 *  `extra` (each prefixed with ", ", suitable for splicing into the body of
 *  a CREATE TABLE statement). Parent table names are rewritten to our
 *  `<db>__<tbl>` convention.
 *
 *  Returns true on success. Returns false when the user's CREATE contains
 *  a FOREIGN KEY shape stoolap can't enforce (multi-column or self-ref);
 *  in that case my_printf_error has already been raised so the caller can
 *  bail with an empty SQL string. */
bool collect_foreign_keys(THD* thd, const std::string& current_db,
                          const std::string& current_tbl, std::string& extra) {
    if (!thd) return true;
    LEX_STRING* qs = thd_query_string(thd);
    if (!qs || !qs->str || qs->length == 0) return true;

    std::string_view sql(qs->str, qs->length);
    bool unsupported = false;
    for (size_t i = 0; i < sql.size();) {
        if (sql[i] == '\'' || sql[i] == '"' || sql[i] == '`') {
            char q = sql[i++];
            while (i < sql.size() && sql[i] != q) {
                if (sql[i] == '\\' && i + 1 < sql.size())
                    i += 2;
                else
                    ++i;
            }
            if (i < sql.size()) ++i;
            continue;
        }
        if (fk_match_kw(sql, i, "FOREIGN") != std::string_view::npos) {
            size_t end = copy_one_fk_clause(sql, i, current_db, current_tbl,
                                            extra, unsupported);
            if (unsupported) return false;
            if (end == std::string_view::npos)
                ++i;
            else
                i = end;
            continue;
        }
        ++i;
    }
    return true;
}

/**
 * Build a stoolap CREATE TABLE statement for `form`'s schema.
 *
 * - Single-column PRIMARY KEY is emitted inline on the column (`id INT
 *   NOT NULL PRIMARY KEY`). Stoolap only creates its PkIndex from the
 *   column-level form; table-level `PRIMARY KEY (col)` is parsed but
 *   ignored, so it would not enforce uniqueness.
 * - UNIQUE keys are emitted as table-level constraints (stoolap honors
 *   these and rejects duplicates).
 * - Multi-column PKs and non-unique indexes are skipped for now.
 *
 * Returns empty string and logs if a column type is unsupported.
 */
// Inspect every PK / UNIQUE key part for a non-binary string column.
// Stoolap's TEXT equality is byte-wise, so a UNIQUE on a ci VARCHAR
// would accept 'a' and 'A' as distinct rows -- the engine's enforcement
// would silently diverge from MariaDB's ci semantics. Returns the
// offending field name (caller emits the error). Empty string means OK.
std::string ci_string_unique_part(TABLE* form) {
    auto is_ci_string = [](Field* f) -> bool {
        if (!f || !f->has_charset()) return false;
        switch (f->real_type()) {
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB:
            case MYSQL_TYPE_ENUM:
            case MYSQL_TYPE_SET:
                break;
            default:
                return false;
        }
        CHARSET_INFO* cs = f->charset();
        return cs && !(cs->state & MY_CS_BINSORT);
    };
    for (uint i = 0; i < form->s->keys; ++i) {
        KEY& k = form->key_info[i];
        if (!(k.flags & HA_NOSAME)) continue;  // PRIMARY and UNIQUE only
        for (uint j = 0; j < k.user_defined_key_parts; ++j) {
            Field* f = k.key_part[j].field;
            if (is_ci_string(f)) {
                return std::string(f->field_name.str, f->field_name.length);
            }
        }
    }
    return std::string();
}

std::string build_create_sql(const std::string& table_name, TABLE* form,
                             bool if_not_exists) {
    // Refuse PK / UNIQUE on ci-collated string columns. Stoolap's
    // bytewise equality would accept 'a' and 'A' as distinct, silently
    // weakening the constraint vs. what MariaDB users expect on the
    // default `_general_ci` collations. The user has two clean fixes:
    // change the column collation to `_bin` (e.g. `utf8mb4_bin`), or
    // drop the UNIQUE / PRIMARY KEY. Failing CREATE up front is
    // preferable to silently accepting case-different duplicates.
    if (std::string bad = ci_string_unique_part(form); !bad.empty()) {
        my_printf_error(ER_GET_ERRMSG,
                        "stoolap: PRIMARY KEY / UNIQUE on column '%s' uses "
                        "a case-insensitive collation, but stoolap compares "
                        "TEXT bytewise. Use a `_bin` collation (e.g. "
                        "utf8mb4_bin) on the column, or drop the "
                        "constraint.",
                        MYF(0), bad.c_str());
        return std::string();
    }

    // Identify which column (if any) carries a single-column PK so we can
    // emit it inline. Reject multi-column PRIMARY KEY here: stoolap only
    // builds its uniqueness index from the column-level PRIMARY KEY form,
    // so a table-level PRIMARY KEY (a, b) parses but isn't enforced --
    // duplicates of (a, b) would silently be accepted.
    int pk_col = -1;
    if (form->s->primary_key < form->s->keys) {
        KEY& pk = form->key_info[form->s->primary_key];
        if (pk.user_defined_key_parts == 1) {
            pk_col = static_cast<int>(pk.key_part[0].field->field_index);
        } else {
            my_printf_error(ER_GET_ERRMSG,
                            "stoolap: composite PRIMARY KEY (%u columns) is "
                            "not supported -- stoolap only enforces "
                            "single-column primary keys. Drop the constraint "
                            "or pick a single column.",
                            MYF(0), pk.user_defined_key_parts);
            return std::string();
        }
    }
    // Same story for table-level UNIQUE: only single-column UNIQUE is
    // enforced by stoolap. A multi-column UNIQUE (a, b) would be parsed
    // and ignored, accepting duplicate (a, b) rows.
    for (uint i = 0; i < form->s->keys; ++i) {
        if (static_cast<uint>(i) == form->s->primary_key) continue;
        KEY& k = form->key_info[i];
        if (!(k.flags & HA_NOSAME)) continue;
        if (k.user_defined_key_parts > 1) {
            my_printf_error(ER_GET_ERRMSG,
                            "stoolap: composite UNIQUE key '%s' (%u columns) "
                            "is not supported -- stoolap only enforces "
                            "single-column UNIQUE constraints. Drop the "
                            "constraint or split it into single-column ones.",
                            MYF(0), k.name.str ? k.name.str : "(unnamed)",
                            k.user_defined_key_parts);
            return std::string();
        }
    }

    std::string sql = "CREATE TABLE ";
    if (if_not_exists) sql += "IF NOT EXISTS ";
    sql += quote_ident(table_name);
    sql += " (";
    for (uint i = 0; i < form->s->fields; ++i) {
        Field* f = form->field[i];
        const char* type = stoolap_type_for(f);
        if (!type) {
            sql_print_error(
                "stoolap: unsupported column type for '%s' (mysql type=%d)",
                f->field_name.str, (int)f->real_type());
            return std::string();
        }
        if (i) sql += ", ";
        sql += quote_ident(
            std::string_view(f->field_name.str, f->field_name.length));
        sql.push_back(' ');
        sql += type;
        if (!f->real_maybe_null()) sql += " NOT NULL";
        if (static_cast<int>(i) == pk_col) sql += " PRIMARY KEY";
        if (f->flags & AUTO_INCREMENT_FLAG) sql += " AUTO_INCREMENT";
    }
    for (uint i = 0; i < form->s->keys; ++i) {
        if (static_cast<uint>(i) == form->s->primary_key) continue;
        KEY& k = form->key_info[i];
        if (!(k.flags & HA_NOSAME)) continue;  // skip non-unique
        sql += ", UNIQUE (";
        for (uint j = 0; j < k.user_defined_key_parts; ++j) {
            if (j) sql += ", ";
            Field* f = k.key_part[j].field;
            sql += quote_ident(
                std::string_view(f->field_name.str, f->field_name.length));
        }
        sql.push_back(')');
    }
    // FOREIGN KEY clauses aren't in form->key_info — MariaDB strips them when
    // the engine doesn't claim FK support. Recover them by scanning the
    // current THD's original CREATE TABLE text and rewrite the parent table
    // name into our `db__tbl` convention. Empty for ALTER ADD FK (which is a
    // follow-up).
    {
        std::string db_name(form->s->db.str, form->s->db.length);
        if (!collect_foreign_keys(current_thd, db_name, table_name, sql)) {
            return std::string();  // unsupported FK shape; caller bails
        }
    }
    sql.push_back(')');
    return sql;
}

int stoolap_init_func(void* p) {
    stoolap_hton = static_cast<handlerton*>(p);
    stoolap_hton->db_type = DB_TYPE_AUTOASSIGN;
    stoolap_hton->create = stoolap_create_handler;
    // HTON_CAN_RECREATE would tell MariaDB to implement TRUNCATE as
    // DROP+CREATE, which collides with the .frm and stoolap-side state.
    // We expose a real `delete_all_rows()` that runs stoolap's native
    // TRUNCATE, so we let MariaDB take that path instead.
    stoolap_hton->flags = 0;
    stoolap_hton->commit = stoolap_commit_cb;
    stoolap_hton->rollback = stoolap_rollback_cb;
    stoolap_hton->close_connection = stoolap_close_connection_cb;
    stoolap_hton->start_consistent_snapshot =
        stoolap_start_consistent_snapshot_cb;
    // Whole-SELECT pushdown. The factories return NULL when the query
    // isn't pushdown-eligible (cross-engine join, SP context, prepare
    // phase, side effects, etc.), in which case MariaDB falls through to
    // the row-pump path. That's the documented contract of create_select
    // and create_unit, not an error. See ha_stoolap_select.cc for the
    // eligibility predicate and SQL emission.
    stoolap_hton->create_select =
        stoolap_pushdown::create_stoolap_select_handler;
    stoolap_hton->create_unit = stoolap_pushdown::create_stoolap_unit_handler;
    // Partial pushdown for hybrid stoolap+InnoDB queries: when the outer
    // SELECT mixes engines, select_handler bails. derived_handler picks
    // up any fully-stoolap derived inside.
    stoolap_hton->create_derived =
        stoolap_pushdown::create_stoolap_derived_handler;
    // Savepoints are deliberately not registered: stoolap's `tx_exec` C ABI
    // currently rejects non-DML statements inside a transaction, so we
    // cannot route `SAVEPOINT name` through it. The Rust-side
    // `Transaction::create_savepoint` exists but isn't exposed through the
    // FFI. Wiring it up is a stoolap-side change.

    const char* dsn =
        (stoolap_dsn_var && *stoolap_dsn_var) ? stoolap_dsn_var : "memory://";
    int rc = g_engine.open(dsn);
    if (rc != STOOLAP_OK) {
        sql_print_error("stoolap: failed to open DSN '%s': %s", dsn,
                        g_engine.last_error().c_str());
        return HA_ERR_INITIALIZATION;
    }
    sql_print_information(
        "stoolap: storage engine initialized (dsn=%s, version=%s)", dsn,
        stoolap_version());
    return 0;
}

int stoolap_done_func(void*) {
    g_engine.close();
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// ha_stoolap implementation
// ---------------------------------------------------------------------------

ha_stoolap::ha_stoolap(handlerton* hton, TABLE_SHARE* table_arg)
    : handler(hton, table_arg) {
    // MariaDB constructs schema-only handler instances with a NULL share
    // (e.g. mysql_create_frm_image), so guard the deref.
    if (table_arg) {
        // We have no native row id; use the full record bytes as our position
        // cookie. position()/rnd_pos() memcpy through `ref`.
        ref_length = table_arg->reclength;
    }
}

ulonglong ha_stoolap::table_flags() const {
    // We do support transactions (commit/rollback callbacks are wired). Not
    // declaring HA_NO_TRANSACTIONS lets MariaDB use transactional recovery
    // paths like ON DUPLICATE KEY UPDATE and REPLACE INTO.
    //
    // Deliberately no HA_HAS_RECORDS: MariaDB's optimized-away COUNT(*) path
    // turns records() into a user-visible result, and Stoolap's bare
    // table-count metadata is not MVCC-safe enough for that contract.
    // records() remains available for optimizer stats.
    return HA_REC_NOT_IN_SEQ | HA_NULL_IN_KEY |
           HA_BINLOG_ROW_CAPABLE
           // Tell MariaDB our update_row / delete_row need the PK columns
           // populated -- our SQL builder uses the PK to identify the row.
           // Without these flags, the optimizer may strip PK columns from
           // read_set when only filter columns are obviously used, which
           // would leave the PK as zero in the row buffer and cause
           // UPDATE/DELETE to silently target the wrong (or no) row.
           | HA_PRIMARY_KEY_REQUIRED_FOR_DELETE |
           HA_PRIMARY_KEY_REQUIRED_FOR_POSITION
           // Hand whole UPDATE / DELETE statements to stoolap in one call
           // instead of having MariaDB scan matching rows and call back per
           // row. See direct_update_rows / direct_delete_rows.
           | HA_CAN_DIRECT_UPDATE_AND_DELETE;
}

ulong ha_stoolap::index_flags(uint inx, uint part, bool /*all_parts*/) const {
    // For numeric / timestamp / boolean key parts we can take the full
    // range-scan plan (>, >=, <, <=) and ordered iteration -- stoolap
    // sorts those types deterministically and our binary comparison
    // matches MariaDB's expectations.
    //
    // String / binary / decimal columns need conservative flags. MariaDB
    // by default uses a case-insensitive collation (e.g. utf8mb3_general_ci)
    // for VARCHAR columns and rewrites LIKE / BETWEEN into encoded key
    // ranges that assume that collation. Stoolap compares strings byte-wise,
    // so forwarding those ranges silently drops case-folded matches. Drop
    // HA_READ_RANGE / HA_READ_ORDER for string parts so the optimizer
    // skips index-range plans on them and falls back to a full scan with
    // a per-row WHERE check (which MariaDB evaluates with its own
    // collation, getting correct results).
    // HA_KEY_SCAN_NOT_ROR tells the optimizer our index scans are NOT
    // in rowid order. That disables index_merge_intersect plans, which
    // assume ROR and would produce wrong results otherwise: our handler
    // returns rows in stoolap's own order (typically index order, but
    // we have no rowid concept anyway), not in the rowid order MariaDB's
    // intersect logic expects.
    if (inx < table_share->keys &&
        part < table_share->key_info[inx].user_defined_key_parts) {
        Field* f = table_share->key_info[inx].key_part[part].field;
        if (f) {
            switch (f->real_type()) {
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_VAR_STRING:
                case MYSQL_TYPE_STRING:
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB: {
                    // The default ci collation (utf8mb3_general_ci etc.)
                    // case-folds; stoolap compares strings byte-wise.
                    // We keep range support ONLY when the column is
                    // binary-collated (BINARY, VARBINARY, or *_bin /
                    // _binary collation), where MariaDB and stoolap
                    // agree on byte-wise compare.
                    //
                    // For ci collations we return 0 -- not even
                    // HA_READ_NEXT. Without HA_READ_NEXT MariaDB won't
                    // pick the index for ref / range / iter, so a
                    // ci-string predicate forces a full scan with the
                    // server applying its own collation per row. With
                    // HA_READ_NEXT (no HA_READ_RANGE) MariaDB still
                    // picks ref access for `name = 'bob'` and our
                    // bytewise index_read_map predicate would silently
                    // miss case-folded matches like 'Bob' / 'BOb'.
                    CHARSET_INFO* cs = f->charset();
                    if (cs && (cs->state & MY_CS_BINSORT)) {
                        return HA_READ_NEXT | HA_READ_RANGE | HA_READ_ORDER |
                               HA_KEY_SCAN_NOT_ROR;
                    }
                    return 0;
                }
                case MYSQL_TYPE_NEWDECIMAL:
                case MYSQL_TYPE_ENUM:
                case MYSQL_TYPE_SET:
                    // Stoolap stores these as TEXT. ref access on the
                    // index would build a numeric-vs-text predicate
                    // (e.g. `dec_v = 1.50` against text '1.50') which
                    // silently misses. Drop all index access methods.
                    return 0;
                default:
                    break;
            }
        }
    }
    return HA_READ_NEXT | HA_READ_RANGE | HA_READ_ORDER | HA_KEY_SCAN_NOT_ROR;
}

StoolapDB* ha_stoolap::db_ensure() {
    if (!db_) db_ = g_engine.clone_handle();
    return db_.get();
}

// Forward decl: defined below alongside create(). Used by both create()
// (after a fresh CREATE TABLE) and open()'s reconcile path.
void create_secondary_indexes(StoolapDB* db, const std::string& tbl,
                              TABLE* form);

int ha_stoolap::open(const char* name, int /*mode*/, uint /*test_if_locked*/) {
    stoolap_table_ = stoolap_table_from_path(name);

    // Detect BLOB/TEXT columns once per open. rnd_pos() needs this to refuse
    // restore-from-ref when any field's in-record bytes are a (length, ptr)
    // trio whose ptr lifetime is bound to the prior rnd_next's value-store
    // (see has_blob_field_ comment in ha_stoolap.h).
    has_blob_field_ = false;
    if (table) {
        for (uint i = 0; i < table->s->fields; ++i) {
            switch (table->field[i]->real_type()) {
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB:
                    has_blob_field_ = true;
                    break;
                default:
                    break;
            }
            if (has_blob_field_) break;
        }
    }

    // Reconcile schema: if MariaDB has a .frm but the stoolap side is missing
    // the table (fresh datadir, manual wipe, DSN switch, etc.), recreate it
    // from the TABLE_SHARE schema. CREATE TABLE IF NOT EXISTS is a no-op when
    // the table already exists.
    //
    // Cache the verified-exists state on the process-wide engine so the
    // second+ open of the same table short-circuits. MariaDB calls open()
    // for every fresh handler instance (per statement in tight benchmark
    // loops); re-running a CREATE TABLE IF NOT EXISTS that's a no-op
    // ~99% of the time still pays SQL construction + stoolap parse +
    // catalog lookup. The cache invalidates on DROP / TRUNCATE / RENAME.
    //
    // Note we no longer eagerly clone db_ here. The handler routes
    // through the THD-context clone in normal operation (see warm_db);
    // db_ is allocated lazily on first call to ha_stoolap::db() if some
    // path can't reach a THD ctx, so a tight statement loop never pays
    // the per-statement clone cost.
    if (!g_engine.is_reconciled(stoolap_table_)) {
        // Schema reconcile DOES need a stoolap handle now. Prefer the
        // THD ctx (warm) and fall back to the lazy per-handler clone.
        auto* ctx = ha_thd() ? get_thd_ctx(ha_thd()) : nullptr;
        StoolapDB* d = (ctx && ctx->db()) ? ctx->db() : db_ensure();
        if (!d) return HA_ERR_OUT_OF_MEM;

        std::string sql = build_create_sql(stoolap_table_, table,
                                           /*if_not_exists=*/true);
        if (sql.empty()) return HA_ERR_UNSUPPORTED;
        if (stoolap_exec(d, sql.c_str(), nullptr) != STOOLAP_OK) {
            const char* msg = stoolap_errmsg(d);
            sql_print_error("stoolap: schema reconcile failed for '%s': %s",
                            stoolap_table_.c_str(), msg);
            return report_stoolap_error(msg);
        }
        // Re-emit non-unique secondary indexes too. CREATE TABLE IF NOT
        // EXISTS only re-creates the table; without the matching CREATE
        // INDEX calls, a stoolap-side table that was wiped (fresh
        // datadir, manual cleanup, DSN swap) would come back without
        // its KEY indexes, leaving queries that MariaDB believes are
        // index-served permanently degraded to full scans. CREATE
        // INDEX IF NOT EXISTS makes the call idempotent for the
        // already-reconciled-elsewhere case.
        create_secondary_indexes(d, stoolap_table_, table);
        g_engine.mark_reconciled(stoolap_table_);
    }
    return 0;
}

int ha_stoolap::close() {
    // Release both streaming and packed scan state immediately on handler
    // close. A large fetch_all buffer can otherwise live until the handler
    // object itself is destroyed, which is too long for memory-heavy scans.
    reset_scan_state();
    bulk_insert_stmt_.reset();
    insert_sql_.clear();
    db_.reset();
    stoolap_table_.clear();
    return 0;
}

// Issue CREATE INDEX IF NOT EXISTS for every non-unique secondary KEY
// MariaDB declared on `form`. Called from create() (after a fresh
// CREATE TABLE) AND from open()'s reconcile path (after a CREATE
// TABLE IF NOT EXISTS that may have actually re-created a missing
// stoolap-side table). Using IF NOT EXISTS makes the call idempotent
// so the no-op-table path doesn't error per index.
//
// Stoolap's CREATE INDEX is single-column only; for composite KEY
// (a, b) we still create an index on the LEADING column. That's a
// usable prefix index: equality on a -- with or without a trailing
// condition on b -- gets a real index lookup (b filters per row).
// Without this, MariaDB would advertise a composite index that has
// no physical storage backing, and any WHERE on the leading column
// would degrade to a full scan inside stoolap.
void create_secondary_indexes(StoolapDB* db, const std::string& tbl,
                              TABLE* form) {
    for (uint i = 0; i < form->s->keys; ++i) {
        if (i == form->s->primary_key) continue;
        KEY& k = form->key_info[i];
        if (k.flags & HA_NOSAME) continue;  // UNIQUE handled inline
        if (k.user_defined_key_parts == 0 || !k.key_part) continue;
        Field* f = k.key_part[0].field;
        const bool composite = k.user_defined_key_parts > 1;
        std::string idx_sql = "CREATE INDEX IF NOT EXISTS ";
        idx_sql +=
            quote_ident(tbl + "__" + std::string(k.name.str, k.name.length));
        idx_sql += " ON ";
        idx_sql += quote_ident(tbl);
        idx_sql += " (";
        idx_sql += quote_ident(
            std::string_view(f->field_name.str, f->field_name.length));
        idx_sql += ")";
        if (stoolap_exec(db, idx_sql.c_str(), nullptr) != STOOLAP_OK) {
            sql_print_warning(
                "stoolap: failed to create index '%s': %s -- "
                "queries on this column will full-scan",
                k.name.str, stoolap_errmsg(db));
        } else if (composite) {
            sql_print_information(
                "stoolap: composite KEY '%s' indexed by leading column '%s' "
                "(stoolap CREATE INDEX is single-column); trailing key "
                "parts filter per row",
                k.name.str, f->field_name.str);
        }
    }
}

// Resolve a per-thread StoolapDB* for DDL calls. The Stoolap C ABI
// requires that a single StoolapDB handle not be used concurrently;
// concurrent DDL from different THDs through `g_engine.raw()` would
// race both the executor and the per-handle error buffer, so attached
// error messages could leak across threads. Prefer the THD-context
// clone (warm, already used for DML on this connection); fall back to
// the lazy per-handler clone for code paths that arrive without a
// THD (none today, but defensive).
StoolapDB* ha_stoolap::ddl_db() {
    auto* ctx = ha_thd() ? get_thd_ctx(ha_thd()) : nullptr;
    StoolapDB* d = (ctx && ctx->db()) ? ctx->db() : db_ensure();
    return d;
}

int ha_stoolap::create(const char* name, TABLE* form,
                       HA_CREATE_INFO* /*info*/) {
    const std::string tbl = stoolap_table_from_path(name);
    std::string sql = build_create_sql(tbl, form, /*if_not_exists=*/false);
    if (sql.empty()) return HA_ERR_UNSUPPORTED;

    StoolapDB* db = ddl_db();
    if (!db) return HA_ERR_OUT_OF_MEM;
    if (stoolap_exec(db, sql.c_str(), nullptr) != STOOLAP_OK) {
        const char* msg = stoolap_errmsg(db);
        sql_print_error("stoolap: CREATE TABLE failed: %s", msg);
        return report_stoolap_error(msg);
    }
    g_engine.mark_reconciled(tbl);
    create_secondary_indexes(db, tbl, form);
    return 0;
}

int ha_stoolap::delete_table(const char* name) {
    const std::string flat = stoolap_table_from_path(name);
    std::string sql = "DROP TABLE IF EXISTS ";
    sql += quote_ident(flat);
    StoolapDB* db = ddl_db();
    if (!db) return HA_ERR_OUT_OF_MEM;
    int rc = stoolap_exec(db, sql.c_str(), nullptr);
    if (rc != STOOLAP_OK) {
        const char* msg = stoolap_errmsg(db);
        sql_print_error("stoolap: DROP TABLE failed: %s", msg);
        return report_stoolap_error(msg);
    }
    g_engine.drop_reconciled(flat);
    g_engine.records_drop(flat);
    // Drop process-wide AUTO_INCREMENT reservations. A subsequent CREATE
    // TABLE with the same name must start fresh, not from the dropped
    // table's leftover counter.
    g_engine.ai_invalidate();
    return 0;
}

int ha_stoolap::rename_table(const char* from, const char* to) {
    const std::string flat_from = stoolap_table_from_path(from);
    const std::string flat_to = stoolap_table_from_path(to);
    std::string sql = "ALTER TABLE ";
    sql += quote_ident(flat_from);
    sql += " RENAME TO ";
    sql += quote_ident(flat_to);
    StoolapDB* db = ddl_db();
    if (!db) return HA_ERR_OUT_OF_MEM;
    int rc = stoolap_exec(db, sql.c_str(), nullptr);
    if (rc != STOOLAP_OK) {
        const char* msg = stoolap_errmsg(db);
        sql_print_error("stoolap: RENAME TABLE failed: %s", msg);
        return report_stoolap_error(msg);
    }
    // Old name is gone; new name is now reconciled (we just verified it
    // exists by renaming to it).
    g_engine.drop_reconciled(flat_from);
    g_engine.mark_reconciled(flat_to);
    // Move the records cache entry too, if present, so the renamed
    // table doesn't pay a fresh COUNT just because its key changed.
    uint64_t cnt = 0;
    if (g_engine.records_lookup(flat_from, &cnt)) {
        g_engine.records_set(flat_to, cnt);
    }
    g_engine.records_drop(flat_from);
    // Clear AUTO_INCREMENT reservations: the old name is gone, and the new
    // name may have stale reservations from an earlier create/drop cycle.
    g_engine.ai_invalidate();
    return 0;
}

namespace {

/** Shift every Field's read pointer by `d` bytes. Used to point Fields at a
 *  caller-supplied row buffer that isn't `table->record[0]`. */
void move_fields(TABLE* t, my_ptrdiff_t d) {
    if (!d) return;
    for (uint i = 0; i < t->s->fields; ++i)
        t->field[i]->move_field_offset(d);
}

/**
 * Extract a Field's current value into a StoolapValue. For TEXT columns the
 * caller must own a std::string (`text_holder`) for the duration of the API
 * call to keep the StoolapValue.text.ptr valid.
 *
 * Returns true on success, false if the column type is unsupported.
 */
bool extract_field(Field* f, StoolapValue& v, std::string& text_holder) {
    if (f->is_null()) {
        v.value_type = STOOLAP_TYPE_NULL;
        return true;
    }
    switch (f->real_type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_BIT: {
            const longlong raw = f->val_int();
            // Stoolap's INTEGER is i64. All unsigned types narrower than
            // BIGINT UNSIGNED fit (their max <= UINT32_MAX < INT64_MAX),
            // so the bit pattern from val_int() round-trips faithfully
            // and the read path preserves UNSIGNED_FLAG through
            // Field::store. BIGINT UNSIGNED values > INT64_MAX are the
            // only case that *can't* be represented: val_int() hands us
            // the bit pattern as a negative longlong, which would land
            // in stoolap as a negative i64 and compare wrong on every
            // range / order operation. Reject those rather than silently
            // corrupting the data.
            if (f->real_type() == MYSQL_TYPE_LONGLONG && f->is_unsigned() &&
                raw < 0) {
                return false;
            }
            v.value_type = STOOLAP_TYPE_INTEGER;
            v.v.integer = static_cast<int64_t>(raw);
            return true;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            v.value_type = STOOLAP_TYPE_FLOAT;
            v.v.float64 = f->val_real();
            return true;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
            // DECIMAL: val_str() formats as the canonical decimal string
            // (e.g. "123.450"), preserving precision through stoolap TEXT.
            char tmp[256];
            String tmp_str(tmp, sizeof(tmp), f->charset());
            String* s = f->val_str(&tmp_str);
            text_holder.assign(s->ptr(), s->length());
            v.value_type = STOOLAP_TYPE_TEXT;
            v.v.text.ptr = text_holder.data();
            v.v.text.len = static_cast<int64_t>(text_holder.size());
            return true;
        }
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2: {
            // get_timestamp() works only for TIMESTAMP fields (which store
            // Unix seconds); DATETIME fields store packed Y/M/D h:m:s.us.
            // get_date() works for both — it returns parsed components.
            MYSQL_TIME mt;
            std::memset(&mt, 0, sizeof(mt));
            f->get_date(&mt, date_mode_t(0));
            struct tm tm_;
            std::memset(&tm_, 0, sizeof(tm_));
            tm_.tm_year = static_cast<int>(mt.year) - 1900;
            tm_.tm_mon = static_cast<int>(mt.month) - 1;
            tm_.tm_mday = static_cast<int>(mt.day);
            tm_.tm_hour = static_cast<int>(mt.hour);
            tm_.tm_min = static_cast<int>(mt.minute);
            tm_.tm_sec = static_cast<int>(mt.second);
            time_t secs = timegm(&tm_);
            // Stoolap stores TIMESTAMP as int64 nanoseconds since epoch,
            // which can only represent ~1678..2262. Reject dates outside
            // that range explicitly to avoid silent overflow corruption
            // (e.g. 9999-12-31 wrapping to 1816-03-30).
            constexpr int64_t kMaxSecs = INT64_MAX / 1000000000LL;
            constexpr int64_t kMinSecs = INT64_MIN / 1000000000LL;
            const int64_t s64 = static_cast<int64_t>(secs);
            if (s64 > kMaxSecs || s64 < kMinSecs) {
                sql_print_error(
                    "stoolap: DATETIME '%04u-%02u-%02u %02u:%02u:%02u' is "
                    "outside the representable range (~1678-2262)",
                    mt.year, mt.month, mt.day, mt.hour, mt.minute, mt.second);
                return false;
            }
            v.value_type = STOOLAP_TYPE_TIMESTAMP;
            v.v.timestamp_nanos = s64 * 1000000000LL +
                                  static_cast<int64_t>(mt.second_part) * 1000LL;
            return true;
        }
        default:
            return false;
    }
}

/**
 * Build a WHERE clause that uniquely identifies a row in `table->record[0]`
 * (after move_fields has been applied). When the table has a single-column
 * PRIMARY KEY this emits `WHERE pk = $N` — much faster than
 * compare-every-column on stoolap's side because stoolap can use its PK
 * index instead of a full scan. Falls back to the all-columns form when
 * there's no usable PK.
 *
 * Returns true on success; false if any column type is unsupported.
 */
bool append_where_for_row(std::string& sql, TABLE* table,
                          std::vector<StoolapValue>& params,
                          std::vector<std::string>& text_holders,
                          int& next_param) {
    // Fast path: single-column PK with a non-NULL value. Use only the
    // table->field[] view so the field pointers definitely match the
    // already-applied move_fields offset (the KEY_PART_INFO::field alias
    // can point to a separate copy in some MariaDB internal paths).
    TABLE_SHARE* share = table ? table->s : nullptr;
    if (share && share->primary_key < share->keys && share->key_info) {
        KEY& pk = share->key_info[share->primary_key];
        if (pk.user_defined_key_parts == 1 && pk.key_part) {
            uint pk_idx = pk.key_part[0].fieldnr;
            if (pk_idx > 0) --pk_idx;  // fieldnr is 1-based
            if (pk_idx < share->fields) {
                Field* pkf = table->field[pk_idx];
                if (pkf && !pkf->is_null()) {
                    sql += " WHERE ";
                    sql += quote_ident(std::string_view(
                        pkf->field_name.str, pkf->field_name.length));
                    sql += " = $";
                    sql += std::to_string(next_param++);
                    StoolapValue v{};
                    text_holders.emplace_back();
                    if (!extract_field(pkf, v, text_holders.back()))
                        return false;
                    params.push_back(v);
                    return true;
                }
            }
        }
    }

    // No usable single-column PK: refuse the row-pump path. Identifying
    // a row by every column value is ambiguous when byte-identical
    // duplicates exist -- the WHERE we'd build matches every duplicate,
    // so MariaDB's filesort+LIMIT path (e.g. UPDATE/DELETE ... ORDER BY
    // ... LIMIT N) calls update_row/delete_row N times but each call
    // affects every duplicate in stoolap, silently mutating more rows
    // than the user asked for. Stoolap doesn't expose a row-id we can
    // use as a tiebreaker, and its UPDATE/DELETE grammar has no LIMIT,
    // so there's no byte-side fix. Refuse with a clear error pointing
    // users at the workarounds: add a single-column PRIMARY KEY (the
    // recommended path) or write the statement so the direct-DML hook
    // (cond_push -> stoolap UPDATE/DELETE WHERE) handles it as one
    // engine-side query.
    my_printf_error(
        ER_GET_ERRMSG,
        "stoolap: row-pump UPDATE/DELETE on a table without a "
        "single-column PRIMARY KEY is unsupported. Stoolap has no "
        "stable row-id, so MariaDB's filesort+LIMIT path could "
        "over-mutate byte-identical duplicates. Add a single-column "
        "PRIMARY KEY, or rewrite the statement so the WHERE clause "
        "alone selects the rows (no ORDER BY/LIMIT) -- direct DML "
        "then handles it as one engine-side UPDATE/DELETE.",
        MYF(0));
    return false;
}

}  // namespace

// Threshold (in rows) at which write_row eagerly flushes the bulk
// buffer instead of growing it further. Bounds memory for very large
// INSERT ... VALUES, INSERT ... SELECT, and LOAD DATA without losing
// the per-batch prepare amortisation (the prepared StoolapStmt lives
// on the THD context and survives across flushes). Also keeps every
// chunk well below the int32_t row_count cast that the stoolap batch
// FFI accepts. 50K rows is small enough to bound memory at ~5MB for
// a typical 8-column INSERT but large enough that the per-flush
// fixed cost stays a small fraction of overall throughput.
static constexpr size_t kBulkFlushRows = 50000;

void ha_stoolap::start_bulk_insert(ha_rows rows, uint /*flags*/) {
    bulk_active_ = false;
    bulk_owns_tx_ = false;
    bulk_params_.clear();
    bulk_text_holders_.clear();

    // Two batch flavours below:
    //   - Auto-commit: stoolap_stmt_exec_batch runs N rows in one
    //     stoolap-side transaction. Best path; only one fsync on file://.
    //   - Explicit user tx: stoolap_stmt_exec_batch can't compose with an
    //     outer BEGIN (it begins its own tx), but we can still avoid the
    //     per-row parse cost by preparing the INSERT once at batch start
    //     and looping stoolap_tx_stmt_exec at flush time.
    // Either way we activate the bulk path; end_bulk_insert dispatches
    // based on ctx->has_tx().

    // INSERT IGNORE / REPLACE / INSERT ... ON DUPLICATE KEY UPDATE rely
    // on MariaDB seeing a per-row HA_ERR_FOUND_DUPP_KEY callback so it
    // can run the right recovery (skip the dup, delete-then-reinsert,
    // or update the existing row). Stoolap's stmt_exec_batch is
    // all-or-nothing: a single dup aborts the whole batch and rolls
    // every other row back. Buffering would silently drop every
    // non-conflicting row in a multi-row INSERT IGNORE / REPLACE /
    // ODKU. Stay on the per-row write path for these shapes.
    if (stoolap_thd_needs_per_row_dup_handling(ha_thd())) {
        return;
    }

    // AUTO_INCREMENT tables are handled per-row at write_row time: we let
    // update_auto_increment() stamp the field, then buffer the now-fully-
    // populated row into the bulk vector like any other. get_auto_increment()
    // reserves ids from a process-wide counter, so unflushed bulk rows and
    // concurrent sessions cannot reuse the same values.
    bulk_active_ = true;

    // Pre-size bulk_params_ for the upcoming chunk only -- never beyond
    // the chunked-flush threshold. Without this cap a multi-million-row
    // INSERT ... VALUES would allocate StoolapValues for the whole
    // statement before write_row gets a chance to flush. INSERT...SELECT
    // usually passes 0 (source row count not known up front), so we
    // seed kBulkFlushRows as a sensible default that matches the chunk
    // size used by write_row. bulk_text_holders_ is a std::deque -- it
    // doesn't reallocate, so reserve() isn't needed.
    const ha_rows raw_hint =
        (rows && rows != HA_POS_ERROR) ? rows : ha_rows{kBulkFlushRows};
    const ha_rows hint =
        raw_hint < ha_rows{kBulkFlushRows} ? raw_hint : ha_rows{kBulkFlushRows};
    bulk_params_.reserve(hint * table->s->fields);
}

int ha_stoolap::end_bulk_insert() {
    if (!bulk_active_) return 0;
    bulk_active_ = false;
    return flush_bulk_buffer();
}

int ha_stoolap::flush_bulk_buffer() {
    if (bulk_params_.empty()) return 0;
    if (insert_sql_.empty()) {
        bulk_params_.clear();
        bulk_text_holders_.clear();
        return 0;
    }

    // Bulk INSERT prepare cache lives on the THD context (per-connection)
    // not on the handler -- the handler is created fresh per statement,
    // so a handler-local cache would re-prepare per batch. The THD-level
    // cache keyed by INSERT SQL keeps the same StoolapStmt across
    // statements on the connection, matching the direct stoolap benchmark
    // pattern of one prepare per hot template.
    auto* ctx = get_thd_ctx(ha_thd());
    StoolapDB* db_for_prepare = (ctx && ctx->db()) ? ctx->db() : db_ensure();
    StoolapStmt* stmt_raw = ctx ? ctx->bulk_stmt_get(insert_sql_) : nullptr;
    if (!stmt_raw) {
        StoolapStmt* fresh = nullptr;
        if (stoolap_prepare(db_for_prepare, insert_sql_.c_str(), &fresh) !=
            STOOLAP_OK) {
            const char* msg = stoolap_errmsg(db_for_prepare);
            sql_print_error("stoolap: prepare bulk INSERT failed: %s",
                            msg ? msg : "(no detail)");
            bulk_params_.clear();
            bulk_text_holders_.clear();
            return report_stoolap_error(msg);
        }
        stmt_raw = fresh;
        if (ctx) {
            ctx->bulk_stmt_put(insert_sql_, stoolap_mariadb::StmtPtr(fresh));
        } else {
            // No THD ctx (close-time bulk?) -- fall back to handler-local
            // cache so we at least don't leak the prepared stmt.
            bulk_insert_stmt_.reset(fresh);
        }
    }

    const int32_t cols = static_cast<int32_t>(table->s->fields);
    const int32_t row_count = static_cast<int32_t>(bulk_params_.size() / cols);
    int64_t affected = 0;

    int rc;
    if (ctx && ctx->has_tx()) {
        // Inside an explicit user transaction: stoolap_stmt_exec_batch
        // would start a nested tx, so loop tx_stmt_exec on the prepared
        // stmt. We still skip the per-row parse cost; what we can't skip
        // is per-row stoolap-side row-write work, which is the same as
        // direct write_row would do anyway.
        for (int32_t r = 0; r < row_count; ++r) {
            int64_t one = 0;
            rc = stoolap_tx_stmt_exec(ctx->tx(), stmt_raw,
                                      bulk_params_.data() + size_t(r) * cols,
                                      cols, &one);
            if (rc != STOOLAP_OK) break;
            affected += one;
        }
    } else {
        rc = stoolap_stmt_exec_batch(db_for_prepare, stmt_raw,
                                     bulk_params_.data(), cols, row_count,
                                     &affected);
    }
    bulk_params_.clear();
    bulk_text_holders_.clear();
    if (rc != STOOLAP_OK) {
        const char* msg = (ctx && ctx->has_tx())
                              ? stoolap_tx_errmsg(ctx->tx())
                              : stoolap_stmt_errmsg(stmt_raw);
        if (!msg || !*msg) msg = stoolap_errmsg(db_for_prepare);
        sql_print_error("stoolap: batch INSERT failed: %s",
                        msg ? msg : "(no detail)");
        invalidate_records_cache();
        int err = report_stoolap_error(msg);
        if (err == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = guess_errkey(msg, table->s);
            errkey = last_dup_key_;
        }
        return err;
    }
    apply_count_delta(affected);
    return 0;
}

int ha_stoolap::write_row(const uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;

    // Reset before each write so stale values don't bleed into a later
    // statement's ON DUPLICATE KEY UPDATE / REPLACE recovery.
    last_dup_key_ = MAX_KEY;

    // If the table has an AUTO_INCREMENT column and the user didn't supply
    // a value (or supplied 0), the framework needs to stamp the next value
    // into the field before we serialize. update_auto_increment() routes to
    // our get_auto_increment() implementation.
    if (table->next_number_field && buf == table->record[0]) {
        int err = update_auto_increment();
        if (err) return err;
    }

    // Keep the process-wide AI allocator in sync with explicit-id INSERTs
    // and generated values that successfully reached write_row. This is
    // cross-connection state: a later session must not keep issuing low
    // ids after another session inserted id=100 explicitly.
    //
    // Skip non-positive explicit values: AUTO_INCREMENT only assigns
    // positive ids, so a negative or zero user-supplied value cannot
    // collide with future generated ids and must not advance the
    // cache. Without the guard, val_int() returning -2 on a signed
    // column casts to ULLONG_MAX-1 and the next generated insert
    // fails ER_AUTOINC_READ_FAILED (ER 1467).
    if (table->found_next_number_field) {
        Field* aifield = table->found_next_number_field;
        const longlong sv = aifield->val_int();
        const bool unsigned_col = (aifield->flags & UNSIGNED_FLAG) != 0;
        if (unsigned_col || sv > 0) {
            const ulonglong v = static_cast<ulonglong>(sv);
            g_engine.ai_note_explicit(stoolap_table_, static_cast<uint64_t>(v));
        }
    }

    // Build the INSERT SQL template lazily and stash it on the handler.
    // Stoolap has its own semantic-cache that hashes the SQL string and
    // reuses parsed plans across calls, so we just hand it the raw text
    // each time -- no plugin-side StoolapStmt cache needed.
    if (insert_sql_.empty()) {
        insert_sql_ = "INSERT INTO ";
        insert_sql_ += quote_ident(stoolap_table_);
        insert_sql_ += " VALUES (";
        for (uint i = 0; i < table->s->fields; ++i) {
            if (i) insert_sql_.push_back(',');
            insert_sql_.push_back('$');
            insert_sql_ += std::to_string(i + 1);
        }
        insert_sql_.push_back(')');
    }

    const my_ptrdiff_t row_diff =
        static_cast<my_ptrdiff_t>(buf - table->record[0]);
    move_fields(table, row_diff);

    int err = 0;

    // Bulk path: append this row's params to the per-handler buffer instead
    // of executing now. end_bulk_insert flushes them via
    // stoolap_stmt_exec_batch in a single transaction.
    if (bulk_active_) {
        const size_t holders_base = bulk_text_holders_.size();
        const size_t params_base = bulk_params_.size();
        for (uint i = 0; i < table->s->fields; ++i) {
            bulk_text_holders_.emplace_back();
            Field* f = table->field[i];
            StoolapValue v{};
            if (!extract_field(f, v, bulk_text_holders_.back())) {
                err = HA_ERR_UNSUPPORTED;
                // Roll back BOTH this row's holders AND its already-
                // pushed StoolapValues. Without the params rollback,
                // the cols 0..k-1 entries that succeeded for this row
                // would stay in bulk_params_, misaligning every
                // subsequent row's column indices in the batch and
                // (for INSERT IGNORE / continue-on-error paths)
                // producing a silently-corrupted INSERT. deque::pop_back
                // is O(1) and doesn't move earlier elements, so
                // previously-recorded text.ptr values stay valid.
                while (bulk_text_holders_.size() > holders_base) {
                    bulk_text_holders_.pop_back();
                }
                if (bulk_params_.size() > params_base) {
                    bulk_params_.resize(params_base);
                }
                goto done;
            }
            bulk_params_.push_back(v);
        }
        // Eager flush when the buffered batch crosses the row threshold.
        // Bounds memory for large INSERT ... VALUES / SELECT / LOAD DATA
        // without losing the per-batch prepare amortisation: the
        // prepared StoolapStmt lives on the THD context and survives
        // across flushes so each chunk reuses the same parsed plan.
        //
        // Atomicity: stoolap_stmt_exec_batch opens its own internal
        // transaction per call, which would let earlier chunks commit
        // while a later chunk fails. Force the flush onto a single
        // outer stoolap tx so the whole bulk statement is atomic. We
        // only need to do this in autocommit mode (`!ctx->has_tx()`);
        // an explicit user BEGIN already provides the outer tx, and
        // flush_bulk_buffer's tx_stmt_exec path naturally feeds rows
        // through it.
        if (bulk_params_.size() >= kBulkFlushRows * table->s->fields) {
            auto* ctx = get_thd_ctx(ha_thd());
            if (ctx && !ctx->has_tx()) {
                int brc = ctx->begin();
                if (brc != STOOLAP_OK) {
                    sql_print_error("stoolap: bulk-insert tx open failed: %s",
                                    stoolap_errmsg(ctx->db()));
                    err = HA_ERR_GENERIC;
                    goto done;
                }
                bulk_owns_tx_ = true;
                // Make sure the handlerton commit / rollback callbacks
                // fire at statement end so the tx we just opened is
                // properly closed even if the engine's own register
                // path didn't already fire (some bulk INSERT paths
                // skip external_lock).
                trans_register_ha(ha_thd(), /*all=*/false, stoolap_hton,
                                  /*flags=*/0);
            }
            int frc = flush_bulk_buffer();
            if (frc) {
                if (bulk_owns_tx_ && ctx && ctx->has_tx()) {
                    // Rollback our tx so earlier chunks don't persist.
                    // The handlerton rollback callback will see no tx
                    // and short-circuit, which is fine.
                    ctx->rollback();
                    bulk_owns_tx_ = false;
                }
                err = frc;
                goto done;
            }
        }
        goto done;
    }

    {
        std::vector<StoolapValue> params;
        std::vector<std::string> text_holders;
        params.reserve(table->s->fields);
        text_holders.reserve(table->s->fields);

        for (uint i = 0; i < table->s->fields; ++i) {
            Field* f = table->field[i];
            StoolapValue v{};
            text_holders.emplace_back();
            if (!extract_field(f, v, text_holders.back())) {
                err = HA_ERR_UNSUPPORTED;
                goto done;
            }
            params.push_back(v);
        }

        {
            auto* ctx = get_thd_ctx(ha_thd());
            int64_t affected = 0;
            int rc = exec_via(ctx, db_raw(), insert_sql_.c_str(), params.data(),
                              static_cast<int32_t>(params.size()), &affected);
            if (rc != STOOLAP_OK) {
                const char* msg = errmsg_via(ctx, db_raw());
                sql_print_error("stoolap: INSERT failed: %s", msg);
                err = report_stoolap_error(msg);
                if (err == HA_ERR_FOUND_DUPP_KEY) {
                    last_dup_key_ = guess_errkey(msg, table_share);
                    errkey = last_dup_key_;
                }
            } else {
                apply_count_delta(affected);
            }
        }
    }

done:
    move_fields(table, -row_diff);
    return err;
}

int ha_stoolap::update_row(const uchar* old_data, const uchar* new_data) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;

    // Build "UPDATE t SET col1=$1, col2=$2, ... WHERE old1=$N AND ... LIMIT 1".
    // The SET values come from `new_data`, the WHERE bindings from `old_data`.
    std::string sql = "UPDATE ";
    sql += quote_ident(stoolap_table_);
    sql += " SET ";

    std::vector<StoolapValue> params;
    std::vector<std::string> text_holders;
    // Worst-case: every column is text on both sides + each column is a text
    // SET binding and a text WHERE binding.
    params.reserve(table->s->fields * 2);
    text_holders.reserve(table->s->fields * 2);

    int err = 0;
    int next_param = 1;

    // SET clause from new_data. Two filters:
    //   1) `bitmap_is_set(write_set, i)` — MariaDB's hint about which
    //      columns the statement intends to modify.
    //   2) bytewise compare old vs new — skip columns whose value is
    //      unchanged. This matters for ON DUPLICATE KEY UPDATE / REPLACE,
    //      where MariaDB hands us a merged new_data that re-supplies
    //      every column of the existing row, including the PRIMARY KEY.
    //      Stoolap rejects re-assigning a PK column even when the value
    //      doesn't change ("cannot UPDATE primary key column").
    {
        const my_ptrdiff_t d =
            static_cast<my_ptrdiff_t>(new_data - table->record[0]);
        move_fields(table, d);
        bool first = true;
        for (uint i = 0; i < table->s->fields; ++i) {
            if (!bitmap_is_set(table->write_set, i)) continue;
            Field* f = table->field[i];

            // Skip columns whose value is unchanged: take pointer arithmetic
            // relative to record[0] so we can compare old vs new bytes.
            const uchar* new_p = new_data + (f->ptr - table->record[0]);
            const uchar* old_p = old_data + (f->ptr - table->record[0]);
            const uint pack_len = f->pack_length();
            const bool null_match =
                f->null_ptr
                    ? ((new_data[f->null_ptr - table->record[0]] &
                        f->null_bit) ==
                       (old_data[f->null_ptr - table->record[0]] & f->null_bit))
                    : true;
            if (null_match && std::memcmp(new_p, old_p, pack_len) == 0) {
                continue;
            }

            if (!first) sql.push_back(',');
            first = false;
            sql += quote_ident(
                std::string_view(f->field_name.str, f->field_name.length));
            sql += " = $";
            sql += std::to_string(next_param++);

            StoolapValue v{};
            text_holders.emplace_back();
            if (!extract_field(f, v, text_holders.back())) {
                err = HA_ERR_UNSUPPORTED;
                move_fields(table, -d);
                goto done;
            }
            params.push_back(v);
        }
        move_fields(table, -d);
        if (first) {
            // Nothing changed — UPDATE with empty SET would be invalid SQL,
            // and there's nothing to write. Treat as success.
            err = 0;
            goto done;
        }
    }

    // WHERE clause from old_data.
    {
        const my_ptrdiff_t d =
            static_cast<my_ptrdiff_t>(old_data - table->record[0]);
        move_fields(table, d);
        bool ok =
            append_where_for_row(sql, table, params, text_holders, next_param);
        move_fields(table, -d);
        if (!ok) {
            err = HA_ERR_UNSUPPORTED;
            goto done;
        }
    }

    {
        auto* ctx = get_thd_ctx(ha_thd());
        int64_t affected = 0;
        int rc = exec_via(ctx, db_raw(), sql.c_str(), params.data(),
                          static_cast<int32_t>(params.size()), &affected);
        if (rc != STOOLAP_OK) {
            const char* msg = errmsg_via(ctx, db_raw());
            sql_print_error("stoolap: UPDATE failed: %s", msg);
            err = report_stoolap_error(msg);
        } else if (affected == 0) {
            err =
                HA_ERR_RECORD_DELETED;  // row vanished between scan and update
        }
    }

done:
    return err;
}

int ha_stoolap::delete_row(const uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;

    const my_ptrdiff_t row_diff =
        static_cast<my_ptrdiff_t>(buf - table->record[0]);
    move_fields(table, row_diff);

    std::string sql = "DELETE FROM ";
    sql += quote_ident(stoolap_table_);

    std::vector<StoolapValue> params;
    std::vector<std::string> text_holders;
    params.reserve(table->s->fields);
    text_holders.reserve(table->s->fields);

    int err = 0;
    int next_param = 1;
    if (!append_where_for_row(sql, table, params, text_holders, next_param)) {
        err = HA_ERR_UNSUPPORTED;
        goto done;
    }

    {
        auto* ctx = get_thd_ctx(ha_thd());
        int64_t affected = 0;
        int rc = exec_via(ctx, db_raw(), sql.c_str(), params.data(),
                          static_cast<int32_t>(params.size()), &affected);
        if (rc != STOOLAP_OK) {
            const char* msg = errmsg_via(ctx, db_raw());
            sql_print_error("stoolap: DELETE failed: %s", msg);
            err = report_stoolap_error(msg);
        } else if (affected == 0) {
            err = HA_ERR_RECORD_DELETED;
        } else {
            apply_count_delta(-affected);
        }
    }

done:
    move_fields(table, -row_diff);
    return err;
}

int ha_stoolap::analyze(THD* /*thd*/, HA_CHECK_OPT* /*opt*/) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    std::string sql = "ANALYZE TABLE ";
    sql += quote_ident(stoolap_table_);
    if (stoolap_exec(db_ensure(), sql.c_str(), nullptr) != STOOLAP_OK) {
        const char* msg = stoolap_errmsg(db_raw());
        sql_print_error("stoolap: ANALYZE failed: %s", msg);
        return report_stoolap_error(msg);
    }
    return 0;
}

const COND* ha_stoolap::cond_push(const COND* cond) {
    if (!cond) return nullptr;
    THD* thd = ha_thd();
    if (!thd) return cond;

    // We accept (return null) iff the engine will actually filter the
    // rows. The only path where that holds today is direct DML:
    // try_direct_modify forwards thd->query() (with the WHERE intact)
    // to stoolap, which re-evaluates the predicate itself. For any
    // shape MariaDB might fall back from direct to per-row -- ORDER BY,
    // LIMIT, multi-table, prepared phase, SP context, no-stoolap-leaves
    // -- can_direct_modify already says no, and we must NOT lie to the
    // optimizer here. Returning null on a shape that ends up per-row
    // means MariaDB might trust us to filter while we hand back every
    // row, silently mass-updating/deleting the table.
    //
    // Single source of truth: same predicate as try_direct_modify uses.
    // If can_direct_modify(thd) accepts, AND the comparison is safe
    // byte-wise (or the user opted into byte semantics), we accept.
    if (stoolap_pushdown::can_direct_modify(thd)) {
        Item* it = const_cast<Item*>(static_cast<const Item*>(cond));
        const bool trust_bin = stoolap_thd_trust_binary_strings(thd);
        if (trust_bin || stoolap_pushdown::item_safe_for_byte_comparison(it)) {
            return nullptr;  // accepted; route to direct path
        }
    }
    // SELECT (row pump) still needs a real cond->SQL translator -- TBD.
    return cond;
}

int ha_stoolap::direct_update_rows_init(List<Item>* /*update_fields*/) {
    // Eligibility check fires here (before MariaDB locks tables) so we can
    // bail without committing to the direct path. If we return non-zero,
    // MariaDB falls back to the per-row update_row() loop instead of
    // failing -- exactly what we want for unsupported corner cases.
    return stoolap_pushdown::can_direct_modify(ha_thd()) ? 0
                                                         : HA_ERR_WRONG_COMMAND;
}

int ha_stoolap::direct_update_rows(ha_rows* update_rows, ha_rows* found_rows) {
    // pre_direct_update_rows may have already executed and stashed the
    // affected count -- drain it here without re-running.
    if (direct_modify_in_pre_) {
        direct_modify_in_pre_ = false;
        if (update_rows) *update_rows = direct_modify_affected_pre_;
        if (found_rows) *found_rows = direct_modify_affected_pre_;
        return 0;
    }
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    if (update_rows) *update_rows = affected;
    // stoolap reports affected rows; we don't track found-but-unchanged
    // separately, so report found == affected. MariaDB only uses found_rows
    // for the "Rows matched: N Changed: M" diagnostic.
    if (found_rows) *found_rows = affected;
    return 0;
}

int ha_stoolap::direct_delete_rows_init() {
    return stoolap_pushdown::can_direct_modify(ha_thd()) ? 0
                                                         : HA_ERR_WRONG_COMMAND;
}

int ha_stoolap::direct_delete_rows(ha_rows* delete_rows) {
    if (direct_modify_in_pre_) {
        direct_modify_in_pre_ = false;
        if (delete_rows) *delete_rows = direct_modify_affected_pre_;
        return 0;
    }
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    if (affected > static_cast<ha_rows>(INT64_MAX)) {
        invalidate_records_cache();
    } else {
        apply_count_delta(-static_cast<int64_t>(affected));
    }
    if (delete_rows) *delete_rows = affected;
    return 0;
}

// pre_direct_* hooks: MariaDB 11.4 gates the direct DML path on these
// returning success. Default base implementation returns
// HA_ERR_WRONG_COMMAND, which is why direct DML wasn't firing despite
// direct_*_init / direct_*_rows being overridden. Eligibility delegates
// to the existing direct_*_init helpers; the actual modify runs in
// pre_direct_*_rows (some 11.4 paths use that as the work site, others
// also call direct_*_rows after, which our short-circuit handles).
int ha_stoolap::pre_direct_update_rows_init(List<Item>* fields) {
    return direct_update_rows_init(fields);
}

int ha_stoolap::pre_direct_update_rows() {
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    direct_modify_in_pre_ = true;
    direct_modify_affected_pre_ = affected;
    return 0;
}

int ha_stoolap::pre_direct_delete_rows_init() {
    return direct_delete_rows_init();
}

int ha_stoolap::pre_direct_delete_rows() {
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    direct_modify_in_pre_ = true;
    direct_modify_affected_pre_ = affected;
    if (affected > static_cast<ha_rows>(INT64_MAX)) {
        invalidate_records_cache();
    } else {
        apply_count_delta(-static_cast<int64_t>(affected));
    }
    return 0;
}

int ha_stoolap::delete_all_rows() {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    // stoolap supports `TRUNCATE TABLE` natively (faster than DELETE FROM).
    // Note: stoolap also blocks TRUNCATE inside an explicit transaction, so
    // we route this through the auto-commit `db` handle, not the active Tx.
    //
    // Cache invalidations come AFTER stoolap confirms success. Stoolap
    // can reject TRUNCATE (e.g. when FK children still reference the
    // parent); a pre-emptive `records_set(0)` would leave the
    // optimizer's HA_HAS_RECORDS shortcut returning 0 while rows are
    // still there.
    std::string sql = "TRUNCATE TABLE ";
    sql += quote_ident(stoolap_table_);

    if (stoolap_exec(db_ensure(), sql.c_str(), nullptr) != STOOLAP_OK) {
        const char* msg = stoolap_errmsg(db_raw());
        sql_print_error("stoolap: TRUNCATE failed: %s", msg);
        return report_stoolap_error(msg);
    }
    set_count_exact(0);
    // AUTO_INCREMENT reservations are now stale: stoolap's MAX for this
    // table dropped to 0 (NULL), but the process-wide allocator may still
    // reflect the pre-truncate counter.
    g_engine.ai_invalidate();
    return 0;
}

// Shared packed-buffer parser lives in stoolap_packet.h and is also used
// by the SELECT pushdown handlers in ha_stoolap_select.cc. Bring it into
// scope here for the row-pump full-scan path.
using stoolap_mariadb::pkt_le32;
using stoolap_mariadb::pkt_parse_header;
using stoolap_mariadb::pkt_skip_value;
using stoolap_mariadb::pkt_store_value;

std::string ha_stoolap::build_scan_columns() {
    // Adaptive projection. Earlier we always emitted SELECT *, which kept
    // the row-pump fallback paths simple but materialised every column
    // per row. For queries that fall through to the row pump because of
    // ci-string predicates / cross-engine joins / SP context (the audit
    // case: `LIKE 'User_1%'` over a wide users table) the wasted FFI
    // bandwidth was a measurable cost.
    //
    // The earlier narrowing attempt projected only `read_set` and broke
    // ORDER BY paths because MariaDB's filesort consults sort-key
    // columns from the row buffer rnd_next populates. Today we project
    // `read_set | write_set` instead -- write_set covers the UPDATE
    // path that compares old/new buffers, and ORDER BY columns are
    // already added to read_set by MariaDB's optimizer (they have to
    // be, otherwise the filesort would read garbage). If neither bitmap
    // is set we fall back to `*` -- safe default for paths we haven't
    // traced.
    scan_proj_.assign(table->s->fields, -1);
    if (!table->read_set || !table->write_set) {
        for (uint i = 0; i < table->s->fields; ++i)
            scan_proj_[i] = i;
        return "*";
    }

    std::string cols;
    int proj_pos = 0;
    for (uint i = 0; i < table->s->fields; ++i) {
        const bool need = bitmap_is_set(table->read_set, i) ||
                          bitmap_is_set(table->write_set, i);
        if (!need) continue;
        scan_proj_[i] = proj_pos++;
        if (!cols.empty()) cols += ", ";
        Field* f = table->field[i];
        cols.append(quote_ident(
            std::string_view(f->field_name.str, f->field_name.length)));
    }
    if (cols.empty()) {
        // No bits set -- belt-and-suspenders fallback so we still produce
        // one row per stoolap row (used by COUNT(*)-shape paths).
        for (uint i = 0; i < table->s->fields; ++i)
            scan_proj_[i] = i;
        return "*";
    }
    return cols;
}

int ha_stoolap::rnd_init(bool /*scan*/) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    reset_scan_state();

    std::string sql = "SELECT " + build_scan_columns() + " FROM ";
    sql += quote_ident(stoolap_table_);

    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    int rc = query_via(ctx, db_raw(), sql.c_str(), &rows);
    if (rc != STOOLAP_OK) {
        const char* msg = errmsg_via(ctx, db_raw());
        sql_print_error("stoolap: SELECT failed: %s", msg);
        return report_stoolap_error(msg);
    }

    // Pick between Tier 3 buffered scan and the original streaming path.
    //
    // Buffered (fetch_all) wins when MariaDB will consume every row:
    // ~5x reduction in FFI traffic on large GROUP BYs and similar full-
    // scan aggregations. It loses badly when MariaDB stops early -- a
    // SELECT ... LIMIT 100 over a 10K-row table would still materialise
    // all 10K rows up front. So if the outermost SELECT has an explicit
    // LIMIT, stay on the streaming path; rnd_next stops at HA_ERR_END_OF_FILE
    // when MariaDB has read its limit.
    if (stoolap_thd_has_explicit_limit(ha_thd())) {
        scan_.reset(rows);
        return 0;
    }

    uint8_t* buf = nullptr;
    int64_t blen = 0;
    int frc = stoolap_rows_fetch_all(rows, &buf, &blen);
    stoolap_rows_close(rows);
    if (frc != STOOLAP_OK) {
        if (buf) stoolap_buffer_free(buf, blen);
        sql_print_error("stoolap: rows_fetch_all failed");
        return HA_ERR_GENERIC;
    }
    // Direct ownership of stoolap's allocation -- skip the memcpy that
    // a std::vector::assign(buf, buf+len) would do. StoolapBuffer's
    // destructor calls stoolap_buffer_free on whatever it holds.
    scan_buf_.take(buf, blen);

    if (!pkt_parse_header(scan_buf_.data(), scan_buf_.size(), &scan_buf_pos_,
                          &scan_buf_cols_)) {
        scan_buf_.reset();
        sql_print_error("stoolap: malformed fetch_all header");
        return HA_ERR_GENERIC;
    }
    if (scan_buf_pos_ + 4 > scan_buf_.size()) {
        scan_buf_.reset();
        return HA_ERR_GENERIC;
    }
    scan_buf_rows_left_ = pkt_le32(scan_buf_.data() + scan_buf_pos_);
    scan_buf_pos_ += 4;
    // Build the cell->field inverse of scan_proj_ once. With adaptive
    // projection scan_proj_[i] is the result-cell index for field i, or
    // -1 when the field wasn't projected. Per-row rnd_next consumes
    // cells in order (the packed format is sequential), so the inverse
    // lets it look up the destination field in one indexed access.
    scan_buf_cell_to_field_.assign(scan_buf_cols_, -1);
    for (size_t i = 0; i < scan_proj_.size(); ++i) {
        const int c = scan_proj_[i];
        if (c >= 0 && static_cast<uint32_t>(c) < scan_buf_cols_) {
            scan_buf_cell_to_field_[c] = static_cast<int>(i);
        }
    }
    stoolap_mariadb::g_stats.buffered_scans.fetch_add(
        1, std::memory_order_relaxed);
    stoolap_mariadb::g_stats.buffered_rows.fetch_add(scan_buf_rows_left_,
                                                     std::memory_order_relaxed);
    return 0;
}

int ha_stoolap::rnd_end() {
    // StoolapBuffer's reset() calls stoolap_buffer_free; the stoolap-
    // side allocator (Rust) reuses freed regions itself, so we don't
    // need to keep the buffer around between scans. Direct ownership
    // also means there's no std::vector capacity to manage.
    reset_scan_state();
    return 0;
}

int ha_stoolap::rnd_next(uchar* buf) {
    // Buffered (Tier 3) path: parse the next row out of the packed
    // fetch_all buffer with no FFI calls.
    if (!scan_buf_.empty()) {
        if (scan_buf_rows_left_ == 0) return HA_ERR_END_OF_FILE;

        const my_ptrdiff_t row_diff =
            static_cast<my_ptrdiff_t>(buf - table->record[0]);
        move_fields(table, row_diff);
        if (table->s->null_bytes) {
            std::memset(buf, 0, table->s->null_bytes);
        }

        const uint8_t* p = scan_buf_.data();
        const size_t len = scan_buf_.size();
        int err = 0;
        // For each cell c the buffer carries (in the SELECT-list order
        // we issued), look up the destination MariaDB field via the
        // pre-built inverse map. Cells the table doesn't claim get
        // skipped without storing -- that case shouldn't happen unless
        // the schemas drift mid-flight, but skip cleanly if it does.
        for (uint32_t c = 0; c < scan_buf_cols_ && !err; ++c) {
            const int field_i = (c < scan_buf_cell_to_field_.size())
                                    ? scan_buf_cell_to_field_[c]
                                    : -1;
            if (field_i < 0) {
                if (!pkt_skip_value(p, len, &scan_buf_pos_))
                    err = HA_ERR_GENERIC;
            } else {
                err = pkt_store_value(p, len, &scan_buf_pos_,
                                      table->field[field_i]);
            }
        }
        // Always advance the row counter, even on a parse error -- the
        // buffer offset is now effectively unrecoverable, so we want
        // subsequent rnd_next calls to return EOF rather than parse
        // garbage. Clamping rows_left to zero on error does that cleanly.
        if (err)
            scan_buf_rows_left_ = 0;
        else
            scan_buf_rows_left_--;

        move_fields(table, -row_diff);
        return err;
    }

    // Streaming fallback: index_* paths set up scan_ instead of scan_buf_.
    if (!scan_) return HA_ERR_END_OF_FILE;

    int rc = stoolap_rows_next(scan_.get());
    if (rc == STOOLAP_DONE) return HA_ERR_END_OF_FILE;
    if (rc != STOOLAP_ROW) {
        sql_print_error("stoolap: rows_next: %s",
                        stoolap_rows_errmsg(scan_.get()));
        return HA_ERR_GENERIC;
    }
    // Schema mismatch is caught at rnd_init / index_init when we open the
    // result set; checking column_count() per row here was paying one FFI
    // call per scanned row for a check that can't change mid-scan.

    const my_ptrdiff_t row_diff =
        static_cast<my_ptrdiff_t>(buf - table->record[0]);
    move_fields(table, row_diff);

    if (table->s->null_bytes) {
        std::memset(buf, 0, table->s->null_bytes);
    }

    int err = 0;
    // The optimizer marks the columns it actually needs in `read_set`. For
    // queries like `SELECT COUNT(*) FROM t` or `SELECT id FROM t WHERE x=?`
    // we'd otherwise pay one FFI call per column we don't read; honoring
    // the bitmap is the difference between O(rows*cols) and O(rows*needed)
    // FFI calls per scan -- the dominant cost on full-scan-style aggregates.
    //
    // We also have to read every column in `write_set`, even when it's not
    // in read_set: update_row() does a byte-compare of old vs new buffers
    // to skip unchanged columns and avoid stoolap's "cannot UPDATE primary
    // key" rejection. If the old buffer were left zeroed, an UPDATE SET
    // x = 0 against a row with x = 100 would compare 0 == 0 and silently
    // skip the SET, turning the whole UPDATE into a no-op.
    // Read every column. We tried gating on read_set/write_set but
    // MariaDB's filesort / temp-table / GROUP BY paths sometimes consult
    // columns we'd otherwise skip (the read_set bitmap doesn't always
    // tell the full story for sort keys produced from index plans),
    // which produced empty result sets on `ORDER BY col LIMIT N`-shape
    // queries. Populating every column is the only correct choice
    // without diving into MariaDB's optimizer hooks.
    for (uint i = 0; i < table->s->fields; ++i) {
        const int col = (i < scan_proj_.size()) ? scan_proj_[i] : -1;
        if (col < 0) continue;
        Field* f = table->field[i];
        // Skip the column_is_null FFI probe entirely for NOT NULL columns
        // (the schema guarantees no NULLs there); for nullable columns,
        // set_notnull() needs to fire when the value is present so the
        // row buffer's null bitmap doesn't carry a stale 1.
        if (f->maybe_null()) {
            if (stoolap_rows_column_is_null(scan_.get(),
                                            static_cast<int32_t>(col))) {
                f->set_null();
                continue;
            }
            f->set_notnull();
        }
        switch (f->real_type()) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_YEAR:
            case MYSQL_TYPE_BIT:
                // Preserve UNSIGNED_FLAG: matches the packed-buffer path
                // in stoolap_packet.h. Without this, the streaming row
                // pump and index/range scans store INT UNSIGNED above
                // INT32_MAX as a negative i64 and BIGINT UNSIGNED above
                // INT64_MAX silently corrupts. The write side already
                // refuses BIGINT UNSIGNED > INT64_MAX up front, so the
                // round-trip via Field::store(..., unsigned=true) is
                // faithful for every value the engine accepts.
                f->store(stoolap_rows_column_int64(scan_.get(),
                                                   static_cast<int32_t>(col)),
                         /*unsigned=*/f->is_unsigned());
                break;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
                f->store(stoolap_rows_column_double(scan_.get(),
                                                    static_cast<int32_t>(col)));
                break;
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_NEWDECIMAL:
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_NEWDATE:
            case MYSQL_TYPE_TIME:
            case MYSQL_TYPE_TIME2:
            case MYSQL_TYPE_ENUM:
            case MYSQL_TYPE_SET:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB: {
                int64_t len = 0;
                const char* s = stoolap_rows_column_text(
                    scan_.get(), static_cast<int32_t>(col), &len);
                if (s) {
                    f->store(s, static_cast<uint>(len), f->charset());
                } else if (f->maybe_null()) {
                    f->set_null();
                }
                break;
            }
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_DATETIME2:
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_TIMESTAMP2: {
                int64_t nanos = stoolap_rows_column_timestamp(
                    scan_.get(), static_cast<int32_t>(col));
                int64_t secs = nanos / 1000000000LL;
                int64_t us = (nanos % 1000000000LL) / 1000LL;
                if (us < 0) {
                    us += 1000000;
                    secs--;
                }
                struct tm tm_;
                time_t t = static_cast<time_t>(secs);
                gmtime_r(&t, &tm_);
                char tbuf[40];
                int n = std::snprintf(tbuf, sizeof(tbuf),
                                      "%04d-%02d-%02d %02d:%02d:%02d.%06lld",
                                      tm_.tm_year + 1900, tm_.tm_mon + 1,
                                      tm_.tm_mday, tm_.tm_hour, tm_.tm_min,
                                      tm_.tm_sec, static_cast<long long>(us));
                f->store(tbuf, static_cast<uint>(n), &my_charset_bin);
                break;
            }
            default:
                err = HA_ERR_UNSUPPORTED;
                goto done;
        }
    }

done:
    move_fields(table, -row_diff);
    return err;
}

int ha_stoolap::rnd_pos(uchar* buf, uchar* pos) {
    // ref_length == table_share->reclength, so this restores the row content
    // captured by position(). Used by UPDATE/DELETE plans that re-read by
    // recorded position rather than by re-scanning.
    //
    // BLOB/TEXT hazard: a BLOB Field stores its in-record bytes as a
    // (length, ptr) trio; ptr references memory owned by the prior
    // rnd_next's stoolap value-store, which the next rnd_next overwrites.
    // Restoring ref bytes verbatim resurrects a freed pointer and the
    // server then reads garbage / crashes. Until rnd_pos is rebuilt to
    // re-fetch the row from stoolap by primary key, refuse the call so
    // affected statements fail loudly instead of corrupting data.
    if (has_blob_field_) {
        my_printf_error(ER_GET_ERRMSG,
                        "stoolap: rnd_pos / re-read by position is not "
                        "supported on tables with BLOB/TEXT columns "
                        "(would resurrect freed payload pointer)",
                        MYF(0));
        return HA_ERR_UNSUPPORTED;
    }
    std::memcpy(buf, pos, ref_length);
    return 0;
}

void ha_stoolap::position(const uchar* record) {
    std::memcpy(ref, record, ref_length);
}

int ha_stoolap::index_init(uint keynr, bool sorted) {
    active_index = keynr;
    index_sorted_ = sorted;
    ci_collation_filter_active_ = false;
    ci_collation_key_info_ = nullptr;
    ci_collation_nparts_ = 0;
    // Drop scan_buf_ alongside scan_: rnd_next prefers the buffer over
    // the streaming cursor, so a non-empty scan_buf_ left over from a
    // prior rnd_init would shadow the index stream we're about to open.
    reset_scan_state();
    return 0;
}

int ha_stoolap::index_end() {
    active_index = MAX_KEY;
    ci_collation_filter_active_ = false;
    ci_collation_key_info_ = nullptr;
    ci_collation_nparts_ = 0;
    reset_scan_state();
    return 0;
}

int ha_stoolap::index_read_map(uchar* buf, const uchar* key,
                               key_part_map keypart_map,
                               enum ha_rkey_function find_flag) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    // Defensive: index_init clears scan_buf_, but a fresh index_read_map
    // call following a prior one within the same index session must also
    // ensure scan_buf_ from any earlier rnd_init can't be sitting around.
    reset_scan_state();

    // Pick the comparison operator for the LAST bound key part and the
    // resulting iteration direction. Earlier key parts are always equality.
    const char* last_op = "=";
    bool desc = false;
    switch (find_flag) {
        case HA_READ_KEY_EXACT:
            last_op = "=";
            break;
        case HA_READ_KEY_OR_NEXT:
            last_op = ">=";
            break;
        case HA_READ_AFTER_KEY:
            last_op = ">";
            break;
        case HA_READ_KEY_OR_PREV:
            last_op = "<=";
            desc = true;
            break;
        case HA_READ_BEFORE_KEY:
            last_op = "<";
            desc = true;
            break;
        case HA_READ_PREFIX:
            last_op = "=";
            break;
        default:
            return HA_ERR_UNSUPPORTED;
    }

    KEY& key_info = table->key_info[active_index];

    uint nparts = 0;
    uint klen = 0;
    for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
        if (!(keypart_map & (key_part_map(1) << i))) break;
        nparts = i + 1;
        klen += key_info.key_part[i].store_length;
    }
    if (nparts == 0) return HA_ERR_KEY_NOT_FOUND;

    std::memset(table->record[1], 0, table->s->reclength);
    key_restore(table->record[1], key, &key_info, klen);

    const my_ptrdiff_t d =
        static_cast<my_ptrdiff_t>(table->record[1] - table->record[0]);
    move_fields(table, d);

    std::string sql = "SELECT " + build_scan_columns() + " FROM ";
    sql += quote_ident(stoolap_table_);
    sql += " WHERE ";

    std::vector<StoolapValue> params;
    std::vector<std::string> text_holders;
    params.reserve(nparts);
    text_holders.reserve(nparts);

    // ci-string ref access: stoolap compares bytes; MariaDB's default
    // ci collations (utf8mb4_general_ci, ...) case-fold AND accent-fold
    // ('é' = 'e', 'ß' = 'ss'). A byte-wise LOWER() rewrite handles
    // ASCII case but not accent equivalents, and we can't pass-through
    // and rely on "Using where" because join ref access drops the
    // residual filter and IN-list ref re-applies the full IN clause
    // per value (multiplying matches).
    //
    // The correct path is to drop the engine-side WHERE for ci ref,
    // pull all rows from stoolap in index order, and ci-filter each
    // row in our overridden index_next using the field's actual
    // CHARSET_INFO comparator (Field::cmp). That gives MariaDB-correct
    // semantics for both single-table and join refs, IN lists (each
    // value's ref returns only its own ci-equivalent rows, no
    // duplication), and accented data ('é' = 'e' is detected via the
    // server's collation library, not approximated). Cost: O(N) per
    // ref, same as a full scan -- acceptable since the optimizer's
    // alternative for ci columns is also full scan.
    //
    // Skipped when stoolap_trust_binary_strings is on (caller has
    // explicitly accepted byte-exact semantics). Also only triggers on
    // the leading key part being ci string for an exact lookup --
    // range/prefix bounds on ci columns are gated to _bin collations
    // by index_flags().
    auto field_needs_ci_fold = [](Field* f) -> bool {
        if (!f) return false;
        switch (f->real_type()) {
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB: {
                CHARSET_INFO* cs = f->charset();
                return cs && !(cs->state & MY_CS_BINSORT);
            }
            default:
                return false;
        }
    };
    const bool trust_bin = stoolap_thd_trust_binary_strings(ha_thd());
    // Trigger the ci fallback whenever ANY bound key part is a ci
    // string column, not just the leading one. KEY(n, s) (INT, ci
    // VARCHAR) is a ref where the leading part is byte-safe but the
    // trailing part still needs ci collation; the bytewise predicate
    // on `s` would miss accent-equivalent rows. index_next already
    // ci-compares every bound key part via Field::cmp, so the filter
    // covers leading-ci AND trailing-ci shapes uniformly.
    bool any_ci_part = false;
    if (!trust_bin && find_flag == HA_READ_KEY_EXACT) {
        for (uint i = 0; i < nparts; ++i) {
            if (field_needs_ci_fold(key_info.key_part[i].field)) {
                any_ci_part = true;
                break;
            }
        }
    }
    const bool ci_collation_lookup = any_ci_part;
    int rc_err = 0;
    int next_param = 1;
    if (ci_collation_lookup) {
        // No engine-side predicate: index_next will Field::cmp every
        // bound key part against record[1]. Strip the trailing " WHERE "
        // we appended above.
        if (sql.size() >= 7 && sql.compare(sql.size() - 7, 7, " WHERE ") == 0) {
            sql.resize(sql.size() - 7);
        }
    } else {
        for (uint i = 0; i < nparts; ++i) {
            Field* f = key_info.key_part[i].field;
            const bool is_last = (i == nparts - 1);
            const char* op = is_last ? last_op : "=";

            if (i) sql += " AND ";
            sql += quote_ident(
                std::string_view(f->field_name.str, f->field_name.length));
            if (f->is_null()) {
                // NULL with a range comparison is always false in SQL — no rows.
                // For equality we use IS NULL.
                if (op[0] == '=' && op[1] == '\0') {
                    sql += " IS NULL";
                    continue;
                }
                move_fields(table, -d);
                return HA_ERR_KEY_NOT_FOUND;
            }
            StoolapValue v{};
            text_holders.emplace_back();
            if (!extract_field(f, v, text_holders.back())) {
                rc_err = HA_ERR_UNSUPPORTED;
                break;
            }
            sql.push_back(' ');
            sql += op;
            sql += " $";
            sql += std::to_string(next_param++);
            params.push_back(v);
        }
    }
    move_fields(table, -d);
    if (rc_err) return rc_err;

    // Index-order iteration: order by every column of the index. DESC when
    // the caller asked for backward iteration; index_next still walks forward
    // through the result set.
    //
    // Skip the ORDER BY for an exact lookup that's bounded by a unique key
    // (PRIMARY or UNIQUE) covering all bound parts -- there's at most one
    // row, so sorting it is pure overhead in stoolap's planner.
    const bool exact_unique_lookup =
        find_flag == HA_READ_KEY_EXACT &&
        nparts == key_info.user_defined_key_parts &&
        (key_info.flags & HA_NOSAME);
    // Drop ORDER BY when:
    //   - the lookup is on a unique key with all parts equality-bound (one
    //     row max, sorting is pointless), OR
    //   - index_init was called with sorted=false AND we're walking
    //     forward (`!desc`). For backward scans we still need ORDER BY
    //     DESC; stoolap returns rows in some natural order but we need
    //     them reversed so subsequent index_next forward-iterates them.
    const bool need_order = !exact_unique_lookup && (index_sorted_ || desc);
    if (need_order) {
        sql += " ORDER BY ";
        for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
            if (i) sql += ", ";
            Field* f = key_info.key_part[i].field;
            sql += quote_ident(
                std::string_view(f->field_name.str, f->field_name.length));
            if (desc) sql += " DESC";
        }
    }

    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    int rc = query_params_via(ctx, db_raw(), sql.c_str(), params.data(),
                              static_cast<int32_t>(params.size()), &rows);
    if (rc != STOOLAP_OK) {
        const char* msg = errmsg_via(ctx, db_raw());
        sql_print_error("stoolap: index_read SELECT failed: %s", msg);
        return report_stoolap_error(msg);
    }
    scan_.reset(rows);

    if (ci_collation_lookup) {
        // Engage the per-row ci-collation filter. The bound key parts'
        // bytes are already in table->record[1] from key_restore above.
        // index_next will compare EVERY bound key part of each fetched
        // row against the search key bytes via each Field's CHARSET_INFO
        // comparator and emit only rows where every part ci-matches.
        // Comparing only the leading part would let composite-key joins
        // like KEY(s, n) ON st.s=so.s AND st.n=so.n through with the
        // wrong `n` value.
        ci_collation_filter_active_ = true;
        ci_collation_key_info_ = &key_info;
        ci_collation_nparts_ = nparts;
    }
    int next_rc = index_next(buf);
    return (next_rc == HA_ERR_END_OF_FILE) ? HA_ERR_KEY_NOT_FOUND : next_rc;
}

int ha_stoolap::index_next(uchar* buf) {
    if (!ci_collation_filter_active_) return rnd_next(buf);
    const KEY* ki = ci_collation_key_info_;
    const uint np = ci_collation_nparts_;
    if (!ki || np == 0) {
        return rnd_next(buf);
    }
    while (true) {
        int r = rnd_next(buf);
        if (r != 0) return r;
        // Every bound key part must ci-equal the search key. Each
        // Field::cmp dispatches to that field's collation: ci VARCHAR
        // uses utf8mb4_general_ci's strnncollsp (case + accent fold),
        // INT/BIGINT/DATETIME/etc. use byte-equality. Bail on the
        // first mismatch so composite indexes like KEY(s, n) reject
        // rows where `n` differs even when `s` ci-matches.
        bool all_match = true;
        for (uint i = 0; i < np; ++i) {
            Field* f = ki->key_part[i].field;
            if (!f) {
                all_match = false;
                break;
            }
            const my_ptrdiff_t off = f->ptr - table->record[0];
            if (f->cmp(table->record[1] + off) != 0) {
                all_match = false;
                break;
            }
        }
        if (all_match) return 0;
    }
}

int ha_stoolap::index_first(uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    reset_scan_state();

    std::string sql = "SELECT " + build_scan_columns() + " FROM ";
    sql += quote_ident(stoolap_table_);
    sql += " ORDER BY ";
    KEY& ki = table->key_info[active_index];
    for (uint i = 0; i < ki.user_defined_key_parts; ++i) {
        if (i) sql += ", ";
        Field* f = ki.key_part[i].field;
        sql += quote_ident(
            std::string_view(f->field_name.str, f->field_name.length));
    }
    // index_first / index_last are one-row probes (MIN/MAX shortcut,
    // existence check, ordered-fallback first row). Without LIMIT 1
    // stoolap would produce an unbounded ordered stream that we'd
    // discard after the first row, paying for the entire sort.
    sql += " LIMIT 1";
    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    int rc = query_via(ctx, db_raw(), sql.c_str(), &rows);
    if (rc != STOOLAP_OK) {
        const char* msg = errmsg_via(ctx, db_raw());
        sql_print_error("stoolap: index_first SELECT failed: %s", msg);
        return report_stoolap_error(msg);
    }
    scan_.reset(rows);
    int next_rc = rnd_next(buf);
    return (next_rc == HA_ERR_END_OF_FILE) ? HA_ERR_KEY_NOT_FOUND : next_rc;
}

int ha_stoolap::index_last(uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    reset_scan_state();

    std::string sql = "SELECT " + build_scan_columns() + " FROM ";
    sql += quote_ident(stoolap_table_);
    sql += " ORDER BY ";
    KEY& ki = table->key_info[active_index];
    for (uint i = 0; i < ki.user_defined_key_parts; ++i) {
        if (i) sql += ", ";
        Field* f = ki.key_part[i].field;
        sql += quote_ident(
            std::string_view(f->field_name.str, f->field_name.length));
        sql += " DESC";
    }
    sql += " LIMIT 1";
    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    int rc = query_via(ctx, db_raw(), sql.c_str(), &rows);
    if (rc != STOOLAP_OK) {
        const char* msg = errmsg_via(ctx, db_raw());
        sql_print_error("stoolap: index_last SELECT failed: %s", msg);
        return report_stoolap_error(msg);
    }
    scan_.reset(rows);
    int next_rc = rnd_next(buf);
    return (next_rc == HA_ERR_END_OF_FILE) ? HA_ERR_KEY_NOT_FOUND : next_rc;
}

int ha_stoolap::read_range_first(const key_range* start_key,
                                 const key_range* end_key, bool eq_range,
                                 bool sorted) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    reset_scan_state();

    KEY& key_info = table->key_info[active_index];

    // Punt collation-sensitive key parts to MariaDB's default range loop.
    // MariaDB rewrites LIKE / BETWEEN over VARCHAR into bounded ranges
    // assuming its own collation rules (case-insensitive `_general_ci`
    // by default), but stoolap compares strings byte-wise. Forwarding
    // those end bounds verbatim would silently drop rows that case-fold
    // into the range. handler::read_range_first does index_read_map(start)
    // + per-row compare_key(end) using MariaDB's collation, so it stays
    // correct -- we just lose the engine-side end-bound shortcut.
    //
    // Binary-collated string columns (`_bin`, VARBINARY, BLOB) compare
    // bytewise on both sides, so the bound IS safe to push for those:
    // we keep them on the engine path. Non-string types (NEWDECIMAL,
    // ENUM, SET) still punt because their bound encoding has surprises
    // that haven't been re-validated.
    auto contains_unsafe_part = [&](const key_range* range) {
        if (!range) return false;
        for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
            if (!(range->keypart_map & (key_part_map(1) << i))) break;
            Field* f = key_info.key_part[i].field;
            switch (f->real_type()) {
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_VAR_STRING:
                case MYSQL_TYPE_STRING:
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB: {
                    // Binary collation -> bytewise -> safe to push the
                    // bound. ci collations -> punt.
                    CHARSET_INFO* cs = f->charset();
                    if (cs && (cs->state & MY_CS_BINSORT)) break;
                    return true;
                }
                case MYSQL_TYPE_NEWDECIMAL:
                case MYSQL_TYPE_ENUM:
                case MYSQL_TYPE_SET:
                    return true;
                default:
                    break;
            }
        }
        return false;
    };
    if (contains_unsafe_part(start_key) || contains_unsafe_part(end_key)) {
        return handler::read_range_first(start_key, end_key, eq_range, sorted);
    }

    // Composite key bounds need lexicographic-tuple semantics:
    // (a, b) >= (1, 5) AND (a, b) <= (2, 3) is *not* the same as
    // a >= 1 AND b >= 5 AND a <= 2 AND b <= 3 -- the latter is empty
    // (`a = 1 AND a = 2`), the former matches (1,5),(1,6),(1,10),
    // (2,2),(2,3). Stoolap's CREATE INDEX is single-column only, so
    // the inner key parts buy nothing as a physical index anyway.
    // For any composite range we expand to an OR-chain of
    // equality-prefixed predicates on the last key part:
    //
    //   (a, b) >= (1, 5)
    //     becomes (a > 1) OR (a = 1 AND b >= 5)
    //   (a, b) <= (2, 3)
    //     becomes (a < 2) OR (a = 2 AND b <= 3)
    //
    // That's literal lex semantics, evaluated correctly by any SQL
    // engine that does column compares -- including stoolap's
    // expression VM.
    auto count_bound_parts = [&](const key_range* range) -> uint {
        if (!range) return 0;
        uint n = 0;
        for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
            if (!(range->keypart_map & (key_part_map(1) << i))) break;
            n = i + 1;
        }
        return n;
    };
    const uint start_parts = count_bound_parts(start_key);
    const uint end_parts = count_bound_parts(end_key);
    const bool composite_range = (start_parts > 1 || end_parts > 1);

    // Helper: translate a key_range::flag into the SQL operator we need on
    // the LAST bound key part. Earlier parts are always equality.
    auto op_for_start = [](ha_rkey_function f) -> const char* {
        switch (f) {
            case HA_READ_KEY_EXACT:
                return "=";
            case HA_READ_KEY_OR_NEXT:
                return ">=";
            case HA_READ_AFTER_KEY:
                return ">";
            case HA_READ_KEY_OR_PREV:
                return "<=";
            case HA_READ_BEFORE_KEY:
                return "<";
            case HA_READ_PREFIX:
                return "=";
            default:
                return nullptr;  // unsupported
        }
    };
    auto op_for_end = [](ha_rkey_function f) -> const char* {
        // MariaDB convention for the upper bound: HA_READ_AFTER_KEY means
        // "up to and including the key" (col <= K), HA_READ_BEFORE_KEY
        // means "up to but not including" (col < K).
        switch (f) {
            case HA_READ_AFTER_KEY:
                return "<=";
            case HA_READ_BEFORE_KEY:
                return "<";
            default:
                return nullptr;
        }
    };

    auto count_parts = [](const KEY& ki, key_part_map map, uint& nparts,
                          uint& klen) {
        nparts = klen = 0;
        for (uint i = 0; i < ki.user_defined_key_parts; ++i) {
            if (!(map & (key_part_map(1) << i))) break;
            nparts = i + 1;
            klen += ki.key_part[i].store_length;
        }
    };

    std::string sql = "SELECT " + build_scan_columns() + " FROM ";
    sql += quote_ident(stoolap_table_);

    std::vector<StoolapValue> params;
    // std::deque (not vector) so push_back never moves earlier strings:
    // append_bound runs twice (start then end key parts) and stashes
    // text_holders.back().data() into StoolapValue.text.ptr after each
    // call. A vector realloc between the two phases would invalidate
    // pointers stored from the start phase. (Same fix bulk_text_holders_
    // got earlier for the same shape of bug.)
    std::deque<std::string> text_holders;
    int next_param = 1;
    bool any_pred = false;

    // Lex-aware bound emitter. For nparts > 1 with a non-equality
    // last_op, emits an OR-chain:
    //   (p0 <strict> v0) OR (p0 = v0 AND p1 <last_op> v1)
    // For "=" or single-part bounds, falls through to the per-part
    // AND form (still correct for those shapes).
    auto append_lex_bound = [&](const char* col, const char* op, int param_no,
                                bool is_first_pred) {
        sql += is_first_pred ? " WHERE " : " AND ";
        sql += col;
        sql += ' ';
        sql += op;
        sql += " $";
        sql += std::to_string(param_no);
    };
    (void)composite_range;
    auto append_bound = [&](const key_range* range,
                            const char* (*op_fn)(ha_rkey_function),
                            bool is_end) -> int {
        if (!range) return 0;
        const char* last_op = op_fn(range->flag);
        if (!last_op) return HA_ERR_UNSUPPORTED;
        const bool is_eq_op = (last_op[0] == '=' && last_op[1] == '\0');
        // Strict variant of last_op for the prefix legs of an OR-chain:
        // ">=" / ">" -> ">"; "<=" / "<" -> "<".
        const char strict = (last_op[0] == '>') ? '>' : '<';

        uint nparts = 0, klen = 0;
        count_parts(key_info, range->keypart_map, nparts, klen);
        if (nparts == 0) return 0;

        // Stage the key bytes into record[1] so Field accessors can read.
        std::memset(table->record[1], 0, table->s->reclength);
        key_restore(table->record[1], range->key, &key_info, klen);
        const my_ptrdiff_t d =
            static_cast<my_ptrdiff_t>(table->record[1] - table->record[0]);
        move_fields(table, d);

        // Lex OR-chain for composite non-equality bounds.
        if (nparts > 1 && !is_eq_op) {
            // Pre-extract every key-part value once (we reference them
            // once per leg of the OR chain).
            std::vector<int> param_nos;
            param_nos.reserve(nparts);
            for (uint i = 0; i < nparts; ++i) {
                Field* f = key_info.key_part[i].field;
                if (f->is_null()) {
                    move_fields(table, -d);
                    return HA_ERR_KEY_NOT_FOUND;  // NULL in lex bound: no rows.
                }
                StoolapValue v{};
                text_holders.emplace_back();
                if (!extract_field(f, v, text_holders.back())) {
                    move_fields(table, -d);
                    return HA_ERR_UNSUPPORTED;
                }
                params.push_back(v);
                param_nos.push_back(next_param++);
            }
            sql += any_pred ? " AND (" : " WHERE (";
            any_pred = true;
            for (uint chain_i = 0; chain_i < nparts; ++chain_i) {
                if (chain_i) sql += " OR ";
                sql += "(";
                for (uint i = 0; i <= chain_i; ++i) {
                    if (i) sql += " AND ";
                    Field* f = key_info.key_part[i].field;
                    sql += quote_ident(std::string_view(f->field_name.str,
                                                        f->field_name.length));
                    char this_op[3] = {0};
                    if (i < chain_i) {
                        this_op[0] = '=';
                    } else if (chain_i < nparts - 1) {
                        this_op[0] = strict;
                    } else {
                        this_op[0] = last_op[0];
                        this_op[1] = last_op[1];
                    }
                    sql.push_back(' ');
                    sql += this_op;
                    sql += " $";
                    sql += std::to_string(param_nos[i]);
                }
                sql += ")";
            }
            sql += ")";
            move_fields(table, -d);
            (void)is_end;
            (void)append_lex_bound;
            return 0;
        }

        for (uint i = 0; i < nparts; ++i) {
            Field* f = key_info.key_part[i].field;
            const bool is_last = (i == nparts - 1);
            const char* op = is_last ? last_op : "=";

            sql += any_pred ? " AND " : " WHERE ";
            any_pred = true;

            sql += quote_ident(
                std::string_view(f->field_name.str, f->field_name.length));
            if (f->is_null()) {
                if (op[0] == '=' && op[1] == '\0') {
                    sql += " IS NULL";
                    continue;
                }
                move_fields(table, -d);
                return HA_ERR_KEY_NOT_FOUND;
            }
            StoolapValue v{};
            text_holders.emplace_back();
            if (!extract_field(f, v, text_holders.back())) {
                move_fields(table, -d);
                return HA_ERR_UNSUPPORTED;
            }
            sql.push_back(' ');
            sql += op;
            sql += " $";
            sql += std::to_string(next_param++);
            params.push_back(v);
        }
        move_fields(table, -d);
        (void)is_end;
        return 0;
    };

    if (int rc = append_bound(start_key, op_for_start, /*is_end=*/false))
        return rc;
    if (int rc = append_bound(end_key, op_for_end, /*is_end=*/true)) return rc;

    // Index-order iteration. Skip the ORDER BY when:
    //   - the start bound is an exact match on a unique index (at most
    //     one row, so order is irrelevant), or
    //   - the caller didn't ask for sorted output (MariaDB sets `sorted`
    //     to indicate whether the range plan needs index-ordered rows).
    // Skipping the sort lets stoolap pick the cheapest scan and avoids a
    // gratuitous order step on the result set.
    bool exact_unique = false;
    if (start_key && start_key->flag == HA_READ_KEY_EXACT &&
        (key_info.flags & HA_NOSAME)) {
        uint sk_parts = 0, sk_klen = 0;
        count_parts(key_info, start_key->keypart_map, sk_parts, sk_klen);
        exact_unique = (sk_parts == key_info.user_defined_key_parts);
    }
    if (!exact_unique && sorted) {
        sql += " ORDER BY ";
        for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
            if (i) sql += ", ";
            Field* f = key_info.key_part[i].field;
            sql += quote_ident(
                std::string_view(f->field_name.str, f->field_name.length));
        }
    }

    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    int rc = query_params_via(ctx, db_raw(), sql.c_str(), params.data(),
                              static_cast<int32_t>(params.size()), &rows);
    if (rc != STOOLAP_OK) {
        const char* msg = errmsg_via(ctx, db_raw());
        sql_print_error("stoolap: read_range query failed: %s", msg);
        return report_stoolap_error(msg);
    }
    scan_.reset(rows);
    int next_rc = rnd_next(table->record[0]);
    return (next_rc == HA_ERR_END_OF_FILE) ? HA_ERR_END_OF_FILE : next_rc;
}

int ha_stoolap::read_range_next() {
    // Route through index_next so the ci-collation filter fires on
    // ranges punted to handler::read_range_first (ci-string keys).
    // For non-ci scans index_next is just rnd_next, so no overhead.
    return index_next(table->record[0]);
}

void ha_stoolap::get_auto_increment(ulonglong offset, ulonglong increment,
                                    ulonglong nb_desired_values,
                                    ulonglong* first_value,
                                    ulonglong* nb_reserved_values) {
    // MariaDB asks for the next auto-increment value before write_row
    // stamps it onto the field. Reserve ids process-wide: per-connection
    // caches are fast but unsafe under concurrent sessions and explicit
    // high-id inserts.
    //
    // `offset` and `increment` come from the session's
    // auto_increment_offset / auto_increment_increment, used by
    // multi-writer replication to give each writer a disjoint id ladder
    // (writer 1 issues 1, 5, 9, ...; writer 2 issues 2, 6, 10, ...).
    // The contract: the returned id and the (nb_reserved_values - 1)
    // ids that follow MUST satisfy (id % increment == offset % increment).
    // MariaDB itself rounds the value we return up to satisfy the
    // modulus, so we could just return ai_next_ verbatim. The trap is
    // that MariaDB then advances by `increment` for each subsequent row
    // in the same batch -- and a *concurrent* session would still see
    // our cursor at the un-stepped position, hand it a colliding id,
    // and produce ER_DUP_ENTRY at write_row time. Reserving with
    // `step=increment` and `offset_mod=offset%increment` makes the
    // engine cursor jump past every id MariaDB will logically issue
    // within the batch, so concurrent sessions skip the whole window.
    *first_value = ~ulonglong(0);
    *nb_reserved_values = nb_desired_values ? nb_desired_values : 1;

    Field* auto_field = table ? table->found_next_number_field : nullptr;
    if (!auto_field) return;

    const uint64_t step = increment ? static_cast<uint64_t>(increment) : 1;
    const uint64_t offset_mod =
        (step > 0) ? static_cast<uint64_t>(offset) % step : 0;

    uint64_t first = 0;
    if (g_engine.ai_reserve(stoolap_table_,
                            static_cast<uint64_t>(*nb_reserved_values), step,
                            offset_mod, &first)) {
        *first_value = static_cast<ulonglong>(first);
        return;
    }

    std::string sql = "SELECT MAX(";
    sql += quote_ident(std::string_view(auto_field->field_name.str,
                                        auto_field->field_name.length));
    sql += ") FROM ";
    sql += quote_ident(stoolap_table_);
    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    if (query_via(ctx, db_raw(), sql.c_str(), &rows) != STOOLAP_OK || !rows) {
        return;
    }
    stoolap_mariadb::RowsPtr scoped(rows);
    uint64_t current_max = 0;
    if (stoolap_rows_next(rows) == STOOLAP_ROW &&
        !stoolap_rows_column_is_null(rows, 0)) {
        // Floor at 0: a row with an explicit negative AI value (e.g.
        // INSERT VALUES (-2, ...)) makes MAX() return a negative int64.
        // Casting that to uint64 wraps to ULLONG_MAX-1, then +1
        // overflows and the next generated insert fails
        // ER_AUTOINC_READ_FAILED. AUTO_INCREMENT only ever issues
        // positive ids, so negatives below the cursor are irrelevant
        // and safe to clamp to 0.
        const int64_t raw = stoolap_rows_column_int64(rows, 0);
        current_max = (raw > 0) ? static_cast<uint64_t>(raw) : 0;
    }
    g_engine.ai_seed_and_reserve(stoolap_table_, current_max + 1,
                                 static_cast<uint64_t>(*nb_reserved_values),
                                 step, offset_mod, &first);
    *first_value = static_cast<ulonglong>(first);
}

void ha_stoolap::invalidate_records_cache() {
    cached_records_valid_ = false;
    if (stoolap_table_.empty()) return;
    if (THD* thd = ha_thd()) {
        auto* ctx = get_thd_ctx(thd);
        if (ctx && ctx->has_tx()) {
            ctx->note_records_dirty(stoolap_table_);
            ctx->records_invalidate(stoolap_table_);
            return;
        }
    }
    g_engine.records_invalidate(stoolap_table_);
}

void ha_stoolap::adjust_records_cache(int64_t delta) {
    if (stoolap_table_.empty()) return;

    if (THD* thd = ha_thd()) {
        auto* ctx = get_thd_ctx(thd);
        if (ctx && ctx->has_tx()) {
            ctx->note_records_dirty(stoolap_table_);
            ctx->records_adjust(stoolap_table_, delta);
            if (cached_records_valid_) {
                uint64_t adjusted = cached_records_;
                if (delta < 0) {
                    const uint64_t dec =
                        static_cast<uint64_t>(-(delta + 1)) + 1;
                    if (dec > adjusted) {
                        cached_records_valid_ = false;
                        ctx->records_invalidate(stoolap_table_);
                        return;
                    }
                    adjusted -= dec;
                } else {
                    const uint64_t inc = static_cast<uint64_t>(delta);
                    if (inc > UINT64_MAX - adjusted) {
                        cached_records_valid_ = false;
                        ctx->records_invalidate(stoolap_table_);
                        return;
                    }
                    adjusted += inc;
                }
                cached_records_ = static_cast<ha_rows>(adjusted);
                stats.records = cached_records_;
            }
            return;
        }
    }

    cached_records_valid_ = false;
    g_engine.records_invalidate(stoolap_table_);
}

void ha_stoolap::apply_count_delta(int64_t delta) {
    adjust_records_cache(delta);
}

void ha_stoolap::set_count_exact(uint64_t value) {
    cached_records_ = static_cast<ha_rows>(value);
    cached_records_valid_ = true;
    stats.records = cached_records_;
    if (!stoolap_table_.empty()) {
        g_engine.records_set(stoolap_table_, value);
    }
}

ha_rows ha_stoolap::cached_records() {
    // Lookup order: handler-local (this handler) -> tx-local for active
    // transactions, or engine-global for autocommit -> live COUNT(*).
    // The tx-local layer is deliberately per-THD so uncommitted/snapshot
    // visible counts never leak into another connection's planning stats.
    if (cached_records_valid_) return cached_records_;
    if (stoolap_table_.empty()) return stats.records;

    THD* thd = ha_thd();
    // Some optimizer/statistics callers reach records() before MariaDB has
    // taken the handler through external_lock(). That means the normal
    // statement registration path may not have opened the session's Stoolap
    // transaction yet. If this is an explicit transaction, register here too
    // so COUNT(*) observes the same snapshot/own-writes view the later row
    // access path will use; otherwise a pre-lock stats probe can silently
    // seed planning with an autocommit-visible count.
    if (thd && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {
        if (register_trx(thd) != 0) return stats.records;
    }
    auto* ctx = thd ? get_thd_ctx(thd) : nullptr;
    // Skip the cross-session cache while a stoolap tx is open: the
    // tx-side COUNT(*) sees this session's uncommitted writes (and not
    // other sessions' post-snapshot writes), which is the right answer
    // for *us* but wrong for any other connection that reads the cache.
    // The reverse is also true: a cached value placed by an autocommit
    // reader in another session can be stale relative to our own
    // in-flight changes. Use only handler-local / tx-local cache here.
    const bool in_tx = ctx && ctx->has_tx();

    if (in_tx) {
        uint64_t cached = 0;
        if (ctx->records_lookup(stoolap_table_, &cached)) {
            cached_records_ = static_cast<ha_rows>(cached);
            cached_records_valid_ = true;
            stats.records = cached_records_;
            return stats.records;
        }
    } else {
        uint64_t cached = 0;
        if (g_engine.records_lookup(stoolap_table_, &cached)) {
            cached_records_ = static_cast<ha_rows>(cached);
            cached_records_valid_ = true;
            stats.records = cached_records_;
            return stats.records;
        }
    }

    // Include a tautological WHERE to force Stoolap through the MVCC-visible
    // scan/aggregate path. Bare COUNT(*) can use Stoolap's table-count
    // metadata, which is not safe around in-flight/rolled-back transactions:
    // it can report rows that ordinary row reads cannot see.
    std::string sql =
        "SELECT COUNT(*) FROM " + quote_ident(stoolap_table_) + " WHERE 1 = 1";
    StoolapRows* rows = nullptr;
    stoolap_mariadb::g_stats.records_live_counts.fetch_add(
        1, std::memory_order_relaxed);
    int rc = query_via(ctx, db_raw(), sql.c_str(), &rows);
    if (rc != STOOLAP_OK || !rows) return stats.records;

    stoolap_mariadb::RowsPtr scoped(rows);
    if (stoolap_rows_next(rows) == STOOLAP_ROW) {
        cached_records_ =
            static_cast<ha_rows>(stoolap_rows_column_int64(rows, 0));
        cached_records_valid_ = true;
        stats.records = cached_records_;
        if (in_tx) {
            ctx->records_set(stoolap_table_,
                             static_cast<uint64_t>(cached_records_));
        }
        // Do not publish live COUNT(*) results into the process-wide cache.
        // Some optimizer/stat callers can run before MariaDB has called
        // external_lock/register_trx for the statement, so detecting an
        // active transaction here is not reliable enough to prevent a
        // transaction-visible count from leaking to other sessions. Keep
        // this as a handler-local cache only; explicit DDL paths such as
        // TRUNCATE may still seed exact global values themselves.
    }
    return stats.records;
}

ha_rows ha_stoolap::records() {
    // Keep this inexpensive for optimizer/stat calls. The handler does not
    // advertise HA_HAS_RECORDS because then MariaDB may use this as the
    // user-visible result for SELECT COUNT(*) FROM t, which requires stronger
    // MVCC guarantees than a shared statistics cache can provide.
    return cached_records();
}

ha_rows ha_stoolap::records_in_range(uint inx, const key_range* min_key,
                                     const key_range* max_key,
                                     page_range* /*res*/) {
    // The default handler::records_in_range returns 10, which is the
    // optimizer's signal of "I have no idea." That number drives join
    // ordering and index-vs-table-scan choices, so giving MariaDB a real
    // estimate matters for plan quality on multi-table queries.
    //
    // We don't have stoolap-side histograms exposed through the C ABI, so
    // approximate: divide the table's row count by the index's distinct-
    // value estimate (rec_per_key) when available, otherwise fall back to
    // a fraction of the table that scales with bound tightness.
    if (inx >= table->s->keys) return 10;
    const KEY& k = table->key_info[inx];
    // Use the same cached COUNT(*) path as records()/info(). Depending on
    // MariaDB's optimizer call order, records_in_range() can be reached
    // before HA_STATUS_VARIABLE has warmed stats.records; treating that as
    // a one-row table causes wildly optimistic range costs and poor joins.
    const ha_rows cached_total = cached_records();
    const ha_rows total = cached_total ? cached_total : 1;

    // ci-leading ref/range access in this handler runs a full table
    // scan in index_next (the only collation-correct option without a
    // stoolap-side ci index). The planner needs to see that real cost
    // so it doesn't sort joins as if ref returns one row -- a big
    // outer would otherwise drive O(outer * inner) probes that each
    // scan the inner table. Reporting `total` here makes the optimizer
    // treat ci ref access as no cheaper than a scan, and it picks a
    // sane join order (or a hash join). Skip the inflation when the
    // user opted into byte semantics: there the engine does a real
    // byte-equal index lookup and ref really is O(1)-ish.
    if (!stoolap_thd_trust_binary_strings(ha_thd())) {
        for (uint i = 0; i < k.user_defined_key_parts; ++i) {
            Field* f = k.key_part[i].field;
            if (!f) continue;
            switch (f->real_type()) {
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_VAR_STRING:
                case MYSQL_TYPE_STRING:
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB: {
                    CHARSET_INFO* cs = f->charset();
                    if (cs && !(cs->state & MY_CS_BINSORT)) {
                        return total;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Equality lookup on a unique key is at most one row.
    if (min_key && max_key && min_key->keypart_map == max_key->keypart_map &&
        min_key->flag == HA_READ_KEY_EXACT &&
        max_key->flag == HA_READ_AFTER_KEY && (k.flags & HA_NOSAME)) {
        return 1;
    }

    // Estimate cardinality via stored rec_per_key if MariaDB has populated
    // it (typically zero for our handler since we don't run ANALYZE-style
    // statistics collection, but cheap to consult).
    if (k.actual_rec_per_key(0) > 0) {
        const double rpk = k.actual_rec_per_key(0);
        return static_cast<ha_rows>(rpk);
    }

    // No rec_per_key. Pick a coarse estimate based on the bound shape.
    // A previous attempt at this pushed MariaDB onto index_merge_intersect
    // plans on composite-key tables and produced wrong results, but we
    // now advertise HA_KEY_SCAN_NOT_ROR everywhere -- which tells the
    // optimizer our index scans aren't rowid-ordered and disables that
    // family of plans -- so it's safe to give a more honest answer.
    //
    //   - exact equality on a non-unique key: ~0.1% (key with many
    //     distinct values; near-unique would have hit the unique branch).
    //   - tight range (both bounds): ~1%.
    //   - half-open range (one bound): ~10%.
    //
    // Floor of 10 so tiny tables don't get pushed onto bad plans.
    const bool exact =
        (min_key && max_key && min_key->flag == HA_READ_KEY_EXACT);
    const bool tight = (min_key && max_key && !exact);
    ha_rows est;
    if (exact)
        est = total / 1000;
    else if (tight)
        est = total / 100;
    else
        est = total / 10;
    if (est < 10) est = 10;
    return est;
}

IO_AND_CPU_COST ha_stoolap::scan_time() {
    // Default handler::scan_time() uses stats.data_file_length, which
    // is 0 for our handler (stoolap is in-memory or behind FFI; we
    // don't have a meaningful file length). Approximate scan cost as
    // `stats.records` cost units split between cpu and io so the ci
    // ref cost gate (keyread_time below) has a non-zero quantity to
    // multiply by, and the optimizer's scan-vs-ref tie-break has real
    // numbers to compare.
    IO_AND_CPU_COST cost;
    const double n = static_cast<double>(stats.records ? stats.records : 1);
    cost.io = n * 0.5;
    cost.cpu = n * 0.5;
    return cost;
}

IO_AND_CPU_COST ha_stoolap::keyread_time(uint index, ulong ranges,
                                         ha_rows /*rows*/, ulonglong blocks) {
    if (index >= table->s->keys || stoolap_thd_trust_binary_strings(ha_thd())) {
        return handler::keyread_time(index, ranges,
                                     /*rows=*/stats.records, blocks);
    }
    const KEY& k = table->key_info[index];
    if (k.user_defined_key_parts == 0) {
        return handler::keyread_time(index, ranges,
                                     /*rows=*/stats.records, blocks);
    }
    bool ci = false;
    for (uint i = 0; i < k.user_defined_key_parts && !ci; ++i) {
        Field* f = k.key_part[i].field;
        if (!f) continue;
        switch (f->real_type()) {
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB: {
                CHARSET_INFO* cs = f->charset();
                ci = cs && !(cs->state & MY_CS_BINSORT);
                break;
            }
            default:
                break;
        }
    }
    if (!ci) {
        return handler::keyread_time(index, ranges,
                                     /*rows=*/stats.records, blocks);
    }
    // Replace the cheap-keylookup estimate with full-scan equivalent.
    // The handler's ci fallback walks every row of the inner table in
    // index_next, so each ref/range probe really is a scan. Override
    // the caller's `rows` estimate too -- MariaDB passes rec_per_key
    // there, which we cannot poison without leaking state across
    // sessions (P1 from a prior round). Fall back on stats.records *
    // ranges for both halves of the cost so the planner can no longer
    // see ci ref as a cheap point lookup.
    IO_AND_CPU_COST scan = scan_time();
    const double mult = static_cast<double>(ranges ? ranges : 1);
    IO_AND_CPU_COST cost;
    cost.io = scan.io * mult;
    cost.cpu = scan.cpu * mult;
    return cost;
}

int ha_stoolap::info(uint flag) {
    stats.deleted = 0;
    stats.data_file_length = 0;
    stats.index_file_length = 0;

    // get_dup_key() clears errkey then calls info(HA_STATUS_ERRKEY) to ask
    // us to re-populate it. We remember the violated key from write_row.
    if (flag & HA_STATUS_ERRKEY) {
        errkey = last_dup_key_;
    }

    // Refresh the row count when MariaDB asks for variable stats. This
    // is an optimizer-planning estimate, not user-facing -- so prefer
    // any cached value (handler-local OR tx-local/process-wide) over running a
    // fresh stoolap COUNT(*). info() fires several times per statement
    // (planning + records_in_range loops + post-statement), and a tight
    // mutation loop invalidates the cache after each row, so calling
    // cached_records() here would re-COUNT(*) the whole table N times
    // per statement. The user's actual SELECT COUNT(*) goes through
    // records() -> cached_records() which still refreshes on demand.
    if (flag & HA_STATUS_VARIABLE) {
        if (!stoolap_table_.empty()) {
            if (cached_records_valid_) {
                stats.records = cached_records_;
            } else {
                uint64_t cached = 0;
                THD* thd = ha_thd();
                auto* ctx = thd ? static_cast<stoolap_mariadb::ThdContext*>(
                                      thd_get_ha_data(thd, stoolap_hton))
                                : nullptr;
                const bool in_tx = ctx && ctx->has_tx();
                if (in_tx && ctx->records_lookup(stoolap_table_, &cached)) {
                    cached_records_ = static_cast<ha_rows>(cached);
                    cached_records_valid_ = true;
                    stats.records = cached_records_;
                } else if (!in_tx &&
                           g_engine.records_lookup(stoolap_table_, &cached)) {
                    cached_records_ = static_cast<ha_rows>(cached);
                    cached_records_valid_ = true;
                    stats.records = cached_records_;
                }
                // Both caches missed: leave stats.records at whatever
                // the previous statement left, falling back to a
                // non-zero placeholder so the optimizer doesn't see
                // "Impossible WHERE" on a freshly-loaded table.
            }
        }
        if (stats.records == 0) stats.records = 1000;
    }

    // Cardinality steering for ci-leading-key indexes (the handler's
    // ci fallback runs a full scan per ref probe in index_next) lives
    // entirely in per-call hooks: keyread_time and records_in_range.
    // Writing KEY::rec_per_key here would persist across the same
    // session's later trust_binary_strings=1 queries and silently feed
    // ci-folded results to a session that asked for byte-exact ones.
    return 0;
}

THR_LOCK_DATA** ha_stoolap::store_lock(THD* /*thd*/, THR_LOCK_DATA** to,
                                       enum thr_lock_type /*lock_type*/) {
    // Skeleton: no engine-level lock yet. Stoolap manages its own concurrency
    // via MVCC, so we return the buffer unchanged (BLACKHOLE-style).
    return to;
}

int ha_stoolap::external_lock(THD* thd, int lock_type) {
    // Lock acquisition runs at statement start; F_UNLCK at end-of-statement.
    if (lock_type == F_UNLCK) return 0;
    return register_trx(thd);
}

// ---------------------------------------------------------------------------
// Plugin declaration
// ---------------------------------------------------------------------------

static MYSQL_SYSVAR_STR(
    dsn, stoolap_dsn_var, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Stoolap DSN (e.g. memory:// or file:///var/lib/stoolap)",
    /*check=*/nullptr, /*update=*/nullptr,
    /*default=*/"memory://");

// Backing variable + update callback for the perf-trace toggle. Kept as
// a plain bool that the system-var update writes; the real consumer is
// the std::atomic<bool> in stoolap_bridge.cc, which the hot paths read
// without taking the THD context. The update callback synchronises the
// two whenever a user runs `SET GLOBAL stoolap_perf_trace = 1`.
static char stoolap_perf_trace_var = 0;
static void stoolap_perf_trace_update(MYSQL_THD, struct st_mysql_sys_var*,
                                      void* var_ptr, const void* save) {
    const char v = *static_cast<const char*>(save);
    *static_cast<char*>(var_ptr) = v;
    stoolap_mariadb::g_perf_trace_enabled.store(v != 0,
                                                std::memory_order_relaxed);
}

static MYSQL_SYSVAR_BOOL(
    perf_trace, stoolap_perf_trace_var, PLUGIN_VAR_OPCMDARG,
    "Enable per-query / per-row pushdown timing in Stoolap_perf_*. "
    "Off by default -- the timing path costs ~50ns per next_row() which "
    "is visible on microsecond-scale point queries. Flip on temporarily "
    "when investigating pushdown latency.",
    /*check=*/nullptr, stoolap_perf_trace_update,
    /*default=*/false);

// Per-session opt-in: bypass the ci-collation guard for SELECT pushdown.
// Stoolap compares strings byte-wise; MariaDB's default VARCHAR collation
// is utf8mb3_general_ci. With the guard ON (default behaviour), queries
// that touch a non-binary string column in WHERE/HAVING/ORDER/GROUP fall
// to the row pump so MariaDB's collation rules apply. With this flag
// flipped, pushdown proceeds anyway -- the user takes responsibility for
// the semantic difference (LIKE 'a%' won't match 'A...', ORDER BY name
// is byte-order, etc.). Safe when the data is ASCII/already-cased or the
// app doesn't depend on case folding.
static MYSQL_THDVAR_BOOL(
    trust_binary_strings, PLUGIN_VAR_OPCMDARG,
    "Push string predicates / sorts / groups to stoolap even on "
    "non-binary VARCHAR/TEXT columns. Stoolap compares bytes; MariaDB "
    "compares with collation. Caller takes responsibility for the "
    "semantic difference. Off by default.",
    /*check=*/nullptr, /*update=*/nullptr,
    /*default=*/false);

// When ON, every successful pushdown runs `EXPLAIN <pushed-sql>` through
// stoolap before the real query and dumps each plan line to the server
// error log. Lets users see what stoolap actually planned (MariaDB's
// `EXPLAIN` only reports the engine accepted the SELECT as one plan).
// Off by default; use only for diagnosis -- adds one extra stoolap call
// per pushed query.
static MYSQL_THDVAR_BOOL(
    explain_pushdown, PLUGIN_VAR_OPCMDARG,
    "Log stoolap's EXPLAIN of every pushed SELECT to the server error "
    "log. Diagnostic only; off by default.",
    /*check=*/nullptr, /*update=*/nullptr,
    /*default=*/false);

static struct st_mysql_sys_var* stoolap_system_variables[] = {
    MYSQL_SYSVAR(dsn), MYSQL_SYSVAR(perf_trace),
    MYSQL_SYSVAR(trust_binary_strings), MYSQL_SYSVAR(explain_pushdown),
    nullptr};

extern "C" int stoolap_thd_trust_binary_strings(MYSQL_THD thd) {
    return THDVAR(thd, trust_binary_strings) ? 1 : 0;
}

extern "C" int stoolap_thd_explain_pushdown(MYSQL_THD thd) {
    return THDVAR(thd, explain_pushdown) ? 1 : 0;
}

struct st_mysql_storage_engine stoolap_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

// SHOW STATUS LIKE 'Stoolap_%' surface. Counters live in stoolap_bridge.cc;
// SHOW_LONGLONG against std::atomic<uint64_t> is safe because both are 8
// bytes and a relaxed atomic load on aarch64 is just a plain load -- the
// status reader doesn't synchronise with writers, and approximate counts
// are fine for SHOW STATUS.
#define STATUS_LONGLONG(name, member)                                 \
    {name, reinterpret_cast<char*>(&stoolap_mariadb::g_stats.member), \
     SHOW_LONGLONG}

static struct st_mysql_show_var stoolap_status_vars[] = {
    STATUS_LONGLONG("Stoolap_pushdown_hits", pushdown_hits),
    STATUS_LONGLONG("Stoolap_pushdown_misses", pushdown_misses),
    STATUS_LONGLONG("Stoolap_direct_modify_hits", direct_modify_hits),
    STATUS_LONGLONG("Stoolap_records_live_counts", records_live_counts),
    STATUS_LONGLONG("Stoolap_buffered_scans", buffered_scans),
    STATUS_LONGLONG("Stoolap_buffered_rows", buffered_rows),
    // Drift detector for the error mapping table. Bumped every time
    // map_stoolap_error degrades a non-empty stoolap message to
    // HA_ERR_GENERIC -- which means stoolap reworded an error and our
    // pattern table missed it. The user still gets the raw stoolap
    // text via ER_GET_ERRMSG (1296), but the SQLSTATE class is wrong.
    // case_18_error_mapping.py asserts this stays at 0 across every
    // known error class.
    STATUS_LONGLONG("Stoolap_unmapped_errors", unmapped_errors),
    // PERF DEBUG: aggregate nanoseconds per phase across all pushed
    // SELECTs. Divide by Stoolap_perf_query_count (or _next_row_count
    // for next_row_ns) to read off per-query / per-row averages. Always
    // on; cost is two clock_gettime calls per phase, ~20ns total.
    STATUS_LONGLONG("Stoolap_perf_factory_setup_ns", perf_factory_setup_ns),
    STATUS_LONGLONG("Stoolap_perf_eager_query_ns", perf_eager_query_ns),
    STATUS_LONGLONG("Stoolap_perf_init_scan_ns", perf_init_scan_ns),
    STATUS_LONGLONG("Stoolap_perf_next_row_ns", perf_next_row_ns),
    STATUS_LONGLONG("Stoolap_perf_end_scan_ns", perf_end_scan_ns),
    STATUS_LONGLONG("Stoolap_perf_query_count", perf_query_count),
    STATUS_LONGLONG("Stoolap_perf_next_row_count", perf_next_row_count),
    {nullptr, nullptr, SHOW_UNDEF},
};

#undef STATUS_LONGLONG

maria_declare_plugin(stoolap){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &stoolap_storage_engine,
    "STOOLAP",
    "Stoolap Contributors",
    "Stoolap embedded SQL engine bridged as a MariaDB storage engine",
    PLUGIN_LICENSE_BSD, /* see README: revisit Apache vs MariaDB plugin license */
    stoolap_init_func,
    stoolap_done_func,
    0x0001,              /* version 0.1 */
    stoolap_status_vars, /* status variables */
    stoolap_system_variables,
    "0.1.0",
    MariaDB_PLUGIN_MATURITY_ALPHA} maria_declare_plugin_end;
