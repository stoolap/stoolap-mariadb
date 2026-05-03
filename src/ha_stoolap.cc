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

// Declared locally to avoid pulling in sql_class.h (transitively requires
// wsrep headers brew doesn't ship).
extern "C" LEX_STRING* thd_query_string(MYSQL_THD thd);

handlerton* stoolap_hton = nullptr;
stoolap_mariadb::Engine g_engine;

// External linkage: ha_stoolap_select.cc's pushdown factories call
// register_trx() defensively before the eager query, since create_select
// can fire before any handler's external_lock.
stoolap_mariadb::ThdContext* get_thd_ctx(THD* thd) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    return ctx;
}

// REPEATABLE-READ / SERIALIZABLE map to STOOLAP_ISOLATION_SNAPSHOT;
// without this, MariaDB's default RR ran as read-committed inside stoolap
// and saw post-BEGIN commits from other sessions.
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

// Pointers inside `details` remain valid only until the next FFI call on
// the originating handle, so consume the view inline before issuing any
// further FFI on that handle. `details.message` is normalised to "".
struct StoolapErrorView {
    int32_t code;
    StoolapErrorDetails details;
};

StoolapErrorView fetch_db_error(StoolapDB* db);
StoolapErrorView fetch_tx_error(StoolapTx* tx);
StoolapErrorView fetch_stmt_error(StoolapStmt* stmt);
StoolapErrorView fetch_rows_error(StoolapRows* rows);
int report_stoolap_error(const StoolapErrorView& v);

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
        // stoolap_tx_commit consumes the tx handle; querying its error
        // message after the call is impossible. Dominant failure mode
        // is write conflict, so map to LOCK_DEADLOCK / 40001 for retry.
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

// Anchor the snapshot at BEGIN time. Without this, the tx wouldn't
// open until the first statement's external_lock fires and a concurrent
// commit between BEGIN and the first SELECT would slide into the snapshot.
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

// Engine-private chunk MariaDB allocates per SAVEPOINT (sized via
// hton->savepoint_offset). MariaDB doesn't pass the user's name into
// our callback, so we synthesise "sp<id>" from a per-connection counter.
struct StoolapSavepointSlot {
    uint64_t id;
    uint8_t name_len;
    char name[24];  // "sp" + 20-digit uint64 max
};

void format_savepoint_slot(StoolapSavepointSlot* slot, uint64_t id) {
    slot->id = id;
    int n = std::snprintf(slot->name, sizeof(slot->name), "sp%llu",
                          static_cast<unsigned long long>(id));
    if (n < 0) n = 0;
    if (n >= static_cast<int>(sizeof(slot->name))) {
        n = static_cast<int>(sizeof(slot->name)) - 1;
    }
    slot->name_len = static_cast<uint8_t>(n);
}

int stoolap_savepoint_set_cb(handlerton* /*hton*/, THD* thd, void* sv) {
    auto* ctx = get_thd_ctx(thd);
    if (!ctx || !ctx->has_tx()) {
        sql_print_error("stoolap: SAVEPOINT without an active tx");
        return HA_ERR_GENERIC;
    }
    auto* slot = static_cast<StoolapSavepointSlot*>(sv);
    format_savepoint_slot(slot, ctx->next_savepoint_id());
    int rc = stoolap_tx_savepoint(ctx->tx(), slot->name,
                                  static_cast<int32_t>(slot->name_len));
    if (rc != STOOLAP_OK) {
        auto verr = fetch_tx_error(ctx->tx());
        sql_print_error("stoolap: SAVEPOINT failed: %s", verr.details.message);
        return report_stoolap_error(verr);
    }
    return 0;
}

int stoolap_savepoint_release_cb(handlerton* /*hton*/, THD* thd, void* sv) {
    auto* ctx = get_thd_ctx(thd);
    // Tx already gone (commit/rollback ran first): savepoint dissolved with it.
    if (!ctx || !ctx->has_tx()) return 0;
    auto* slot = static_cast<StoolapSavepointSlot*>(sv);
    int rc = stoolap_tx_release_savepoint(ctx->tx(), slot->name,
                                          static_cast<int32_t>(slot->name_len));
    if (rc != STOOLAP_OK) {
        auto verr = fetch_tx_error(ctx->tx());
        sql_print_error("stoolap: RELEASE SAVEPOINT failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
    }
    return 0;
}

int stoolap_savepoint_rollback_cb(handlerton* /*hton*/, THD* thd, void* sv) {
    auto* ctx = get_thd_ctx(thd);
    if (!ctx || !ctx->has_tx()) return 0;
    auto* slot = static_cast<StoolapSavepointSlot*>(sv);
    int rc = stoolap_tx_rollback_to_savepoint(
        ctx->tx(), slot->name, static_cast<int32_t>(slot->name_len));
    if (rc != STOOLAP_OK) {
        auto verr = fetch_tx_error(ctx->tx());
        sql_print_error("stoolap: ROLLBACK TO SAVEPOINT failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
    }
    // We track count deltas at tx granularity, not per-savepoint, so any
    // adjusted count is now wrong; re-fetch on next read.
    ctx->invalidate_dirty_records();
    return 0;
}

/* ---------- Tx-aware execution helpers ---------- */

// THD-context db() is cloned per-connection so its parse cache stays
// warm across statements; ha_stoolap::db_ is reset per-handler-close.
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

// O(1) MVCC-safe table count. tx-side sees this session's uncommitted
// INSERT/DELETE; db-side returns the autocommit-visible count.
int count_via(stoolap_mariadb::ThdContext* ctx, StoolapDB* fallback,
              const char* table, uint64_t* out_count) {
    if (ctx && ctx->has_tx()) {
        return stoolap_tx_table_count(ctx->tx(), table, out_count);
    }
    return stoolap_table_count(warm_db(ctx, fallback), table, out_count);
}

}  // namespace (helpers below have external linkage so
// ha_stoolap_select.cc can call them.)

namespace {

// Defensive: callers fetch only AFTER a non-OK FFI rc, so STOOLAP_ERR_OK
// here means the error fetch itself is broken (NULL handle, wrong handle).
// Promote to GENERIC so map_stoolap_errcode can't silently return 0.
StoolapErrorView finish_view(StoolapErrorView v) {
    if (!v.details.message) v.details.message = "";
    v.code = v.details.code;
    if (v.code == STOOLAP_ERR_OK) {
        v.code = STOOLAP_ERR_GENERIC;
        v.details.code = STOOLAP_ERR_GENERIC;
    }
    return v;
}

}  // namespace

StoolapErrorView fetch_db_error(StoolapDB* db) {
    StoolapErrorView v{};
    if (db) (void)stoolap_errdetails(db, &v.details);
    return finish_view(v);
}

StoolapErrorView fetch_tx_error(StoolapTx* tx) {
    StoolapErrorView v{};
    if (tx) (void)stoolap_tx_errdetails(tx, &v.details);
    return finish_view(v);
}

StoolapErrorView fetch_stmt_error(StoolapStmt* stmt) {
    StoolapErrorView v{};
    if (stmt) (void)stoolap_stmt_errdetails(stmt, &v.details);
    return finish_view(v);
}

StoolapErrorView fetch_rows_error(StoolapRows* rows) {
    StoolapErrorView v{};
    if (rows) (void)stoolap_rows_errdetails(rows, &v.details);
    return finish_view(v);
}

// Pick the handle that owns the live error: tx if open, else warm db.
StoolapErrorView fetch_error_via(stoolap_mariadb::ThdContext* ctx,
                                 StoolapDB* fallback) {
    if (ctx && ctx->has_tx()) return fetch_tx_error(ctx->tx());
    StoolapDB* d = (ctx && ctx->db()) ? ctx->db() : fallback;
    return fetch_db_error(d);
}

// Composite UNIQUE is refused at CREATE so every KEY has exactly one
// user-defined part; if composite UNIQUE ever lands, walk every key part.
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

// Stoolap errors mix single-quoted ("column 'pid'") and bareword
// ("on column u with value") tokens. Strip surrounding quotes if present.
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

uint find_key_by_name(TABLE_SHARE* share, std::string_view name) {
    if (!share || name.empty()) return MAX_KEY;
    for (uint i = 0; i < share->keys; ++i) {
        const KEY& k = share->key_info[i];
        if (k.name.length == name.size() &&
            ::strncasecmp(k.name.str, name.data(), name.size()) == 0) {
            return i;
        }
    }
    return MAX_KEY;
}

// Prefer details.constraint (typed index name); fall back to column lookup,
// then to message-grep for stoolap codes that still come back GENERIC.
uint errkey_from_view(const StoolapErrorView& v, TABLE_SHARE* share) {
    if (!share) return 0;
    if (v.code == STOOLAP_ERR_PRIMARY_KEY) {
        if (share->primary_key < share->keys) return share->primary_key;
    } else if (v.code == STOOLAP_ERR_UNIQUE) {
        if (v.details.constraint && *v.details.constraint) {
            uint k = find_key_by_name(share, v.details.constraint);
            if (k != MAX_KEY) return k;
        }
        if (v.details.column && *v.details.column) {
            uint k = find_key_for_column(share, v.details.column);
            if (k != MAX_KEY) return k;
        }
    }
    // Prose fallback (stoolap returned GENERIC for what looks like a dup).
    if (v.details.message && *v.details.message) {
        std::string_view m(v.details.message);
        auto starts_with = [&](std::string_view p) {
            return m.size() >= p.size() && m.compare(0, p.size(), p) == 0;
        };
        if (starts_with("primary key constraint failed")) {
            if (share->primary_key < share->keys) return share->primary_key;
        } else if (starts_with("unique constraint failed")) {
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
    if (share->primary_key < share->keys) return share->primary_key;
    return 0;
}

// `known` is true iff the code matched an explicit case below. False
// means the value fell through `default` (future stoolap code we don't
// know about yet). Used by report_stoolap_error to gate the prose-grep
// fallback to GENERIC/INTERNAL/unknown only -- codes deliberately mapped
// to HA_ERR_GENERIC (NOT_NULL, CHECK, ...) stay typed-only.
struct ErrcodeMap {
    int ha_err;
    bool known;
};

ErrcodeMap map_stoolap_errcode(int32_t code) {
    switch (code) {
        case STOOLAP_ERR_OK:
            return {0, true};
        case STOOLAP_ERR_PRIMARY_KEY:
        case STOOLAP_ERR_UNIQUE:
            return {HA_ERR_FOUND_DUPP_KEY, true};
        case STOOLAP_ERR_FOREIGN_KEY:
            return {HA_ERR_NO_REFERENCED_ROW, true};
        case STOOLAP_ERR_TABLE_NOT_FOUND:
        case STOOLAP_ERR_VIEW_NOT_FOUND:
            return {HA_ERR_NO_SUCH_TABLE, true};
        case STOOLAP_ERR_TABLE_EXISTS:
        case STOOLAP_ERR_VIEW_EXISTS:
        case STOOLAP_ERR_INDEX_EXISTS:
            return {HA_ERR_TABLE_EXIST, true};
        case STOOLAP_ERR_TX_ABORTED:
        case STOOLAP_ERR_DB_LOCKED:
            // No deadlock vs lock-wait distinction in stoolap; both
            // become 40001 so clients retry.
            return {HA_ERR_LOCK_DEADLOCK, true};
        case STOOLAP_ERR_NOT_SUPPORTED:
            return {HA_ERR_UNSUPPORTED, true};
        case STOOLAP_ERR_READ_ONLY:
            return {HA_ERR_TABLE_READONLY, true};
        case STOOLAP_ERR_NOT_NULL:
        case STOOLAP_ERR_CHECK:
        case STOOLAP_ERR_TYPE_MISMATCH:
        case STOOLAP_ERR_VALUE_TOO_LONG:
        case STOOLAP_ERR_INVALID_ARGUMENT:
        case STOOLAP_ERR_PARSE:
        case STOOLAP_ERR_DIVISION_BY_ZERO:
        case STOOLAP_ERR_COLUMN_NOT_FOUND:
        case STOOLAP_ERR_INDEX_NOT_FOUND:
        case STOOLAP_ERR_TX_CLOSED:
        case STOOLAP_ERR_IO:
        case STOOLAP_ERR_QUERY_CANCELLED:
        case STOOLAP_ERR_REOPEN_REQUIRED:
            // No specific MariaDB code; surface as 1296 with stoolap text.
            return {HA_ERR_GENERIC, true};
        case STOOLAP_ERR_GENERIC:
        case STOOLAP_ERR_INTERNAL:
            // Stoolap explicitly said "no classification"; let prose try.
            return {HA_ERR_GENERIC, true};
        default:
            // Unknown future code; let prose try and bump unmapped_errors.
            return {HA_ERR_GENERIC, false};
    }
}

// Legacy prose-grep fallback for typed codes that come back GENERIC.
// Source of truth for the strings: ../stoolap/src/core/error.rs.
int map_stoolap_error(const char* msg) {
    if (!msg) return HA_ERR_GENERIC;
    std::string_view m(msg);

    // PREFIX anchors at pos 0; CONTAINS finds the needle anywhere
    // (used for tail markers like the truncate-blocked variant of
    // "uncommitted changes" that appears as the second clause).
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

    // Order: most-specific first. PK/UNIQUE/FK use anchored prefixes
    // since the FK message contains "does not exist" which would
    // otherwise route to NO_SUCH_TABLE.
    static constexpr Pattern kTable[] = {
        {"primary key constraint failed", HA_ERR_FOUND_DUPP_KEY, PREFIX},
        {"unique constraint failed", HA_ERR_FOUND_DUPP_KEY, PREFIX},
        {"foreign key constraint violation", HA_ERR_NO_REFERENCED_ROW, PREFIX},
        // Anchored so future similar wordings don't match by accident.
        {"not null constraint failed", HA_ERR_GENERIC, PREFIX},
        {"CHECK constraint failed", HA_ERR_GENERIC, PREFIX},
        {"' already exists", HA_ERR_TABLE_EXIST, CONTAINS},
        {"' not found", HA_ERR_NO_SUCH_TABLE, CONTAINS},
        // "row N has uncommitted changes ..." + truncate-blocked variant.
        {"uncommitted changes", HA_ERR_LOCK_DEADLOCK, CONTAINS},
        {"write conflict", HA_ERR_LOCK_DEADLOCK, CONTAINS},
        {"failed to acquire lock", HA_ERR_LOCK_DEADLOCK, PREFIX},
        {"not supported", HA_ERR_UNSUPPORTED, CONTAINS},
        {"unsupported", HA_ERR_UNSUPPORTED, CONTAINS},
    };
    for (const Pattern& p : kTable) {
        if (matches(p)) return p.ha_err;
    }
    return HA_ERR_GENERIC;
}

// Brewed mariadbd exports my_error / my_printf_error directly, but the
// plugin's mysql/plugin.h rewrites them through a service-pointer struct
// that isn't shipped. Undef + extern declarations bind to the real symbols.
#undef my_error
#undef my_printf_error
extern "C" void my_error(unsigned int nr, unsigned long MyFlags, ...);
extern "C" void my_printf_error(unsigned int nr, const char* fmt,
                                unsigned long MyFlags, ...);

int report_stoolap_error(const StoolapErrorView& v) {
    const char* msg = v.details.message;
    const bool have_text = (msg && *msg);

    const ErrcodeMap m = map_stoolap_errcode(v.code);
    int rc = m.ha_err;

    // Run prose-grep only when the typed surface had no specific opinion:
    // unknown future code OR stoolap explicitly said GENERIC/INTERNAL.
    const bool typed_unhelpful = !m.known || v.code == STOOLAP_ERR_GENERIC ||
                                 v.code == STOOLAP_ERR_INTERNAL;
    if (rc == HA_ERR_GENERIC && have_text && typed_unhelpful) {
        const int prose_rc = map_stoolap_error(msg);
        if (prose_rc != HA_ERR_GENERIC) {
            // Typed surface lost an error class the prose can still
            // classify -- record the gap and use the prose result so
            // the user gets the right SQLSTATE. Log the (code, msg)
            // pair so an operator can file an upstream typed-error
            // request -- this is the signal that turns the counter
            // from "something is off" into "here is the wording".
            stoolap_mariadb::g_stats.typed_fallback_hits.fetch_add(
                1, std::memory_order_relaxed);
            sql_print_information(
                "stoolap typed-error gap: code=%d msg='%s' "
                "(prose mapped to ha_err=%d) -- file upstream so the "
                "STOOLAP_ERR_* layer covers it directly",
                v.code, msg, prose_rc);
            rc = prose_rc;
        } else {
            // Unrecognised typed code AND wording; signal drift.
            stoolap_mariadb::g_stats.unmapped_errors.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    const bool needs_text = (rc == HA_ERR_GENERIC || rc == HA_ERR_UNSUPPORTED);
    if (needs_text && have_text) {
        my_printf_error(ER_GET_ERRMSG, "stoolap: %s", MYF(0), msg);
    }
    return rc;
}

namespace {

// MariaDB hands us "./db/tbl"; both db and tbl can contain '_' and '/'.
// Inject `_0` for `_` and `_1` for `/` so distinct (db, tbl) pairs never
// collide (naive `/` -> `__` collides on db=`p`,tbl=`q__r` vs db=`p__q`,tbl=`r`).
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

size_t fk_skip_ws(std::string_view s, size_t p) {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p])))
        ++p;
    return p;
}

// Case-insensitive keyword match at p with word boundaries.
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

size_t count_commas(std::string_view s) {
    size_t n = 0;
    for (char c : s)
        if (c == ',') ++n;
    return n;
}

// Rewrites the parent table from `tbl` (or `db.tbl`) to `db__tbl`.
// Skips self-referencing FK (parent not yet registered) and multi-column
// FK (stoolap parser is single-column only); npos on parse failure.
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
    // Silently dropping them lets CREATE TABLE succeed and then accept
    // FK-violating child rows with no client-visible warning.
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

// Append every FOREIGN KEY clause from the THD's CREATE SQL to `extra`,
// prefixed with ", " for splicing into the table body. False when the user's
// CREATE has an FK shape stoolap can't enforce (my_printf_error already raised).
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

// Single-column PK is emitted inline (stoolap only builds PkIndex from
// the column-level form). UNIQUE goes table-level. Multi-column PK/UNIQUE
// and ci-collated PK/UNIQUE are refused before we get here.

// Returns the offending field name when any PK/UNIQUE part is a ci string
// (stoolap's TEXT equality is byte-wise; ci_general would accept 'a'/'A').
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
    // Refuse ci PK/UNIQUE up front: silent acceptance of case-different
    // duplicates is worse than failing at CREATE.
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

    // Single-column PK gets emitted inline. Multi-column PK is refused
    // because stoolap only enforces the column-level PRIMARY KEY form;
    // table-level (a, b) parses but doesn't enforce uniqueness.
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
    // Same story for table-level UNIQUE: composite UNIQUE parses but isn't
    // enforced by stoolap.
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
    // HTON_CAN_RECREATE would route TRUNCATE through DROP+CREATE; we expose
    // a real delete_all_rows that runs stoolap's native TRUNCATE instead.
    stoolap_hton->flags = 0;
    stoolap_hton->commit = stoolap_commit_cb;
    stoolap_hton->rollback = stoolap_rollback_cb;
    stoolap_hton->close_connection = stoolap_close_connection_cb;
    stoolap_hton->start_consistent_snapshot =
        stoolap_start_consistent_snapshot_cb;
    // Pushdown factories return NULL for ineligible plans (cross-engine
    // join, SP context, prepare phase, ...) and MariaDB falls through to
    // row-pump. See ha_stoolap_select.cc.
    stoolap_hton->create_select =
        stoolap_pushdown::create_stoolap_select_handler;
    stoolap_hton->create_unit = stoolap_pushdown::create_stoolap_unit_handler;
    stoolap_hton->create_derived =
        stoolap_pushdown::create_stoolap_derived_handler;
    stoolap_hton->savepoint_offset = sizeof(StoolapSavepointSlot);
    stoolap_hton->savepoint_set = stoolap_savepoint_set_cb;
    stoolap_hton->savepoint_release = stoolap_savepoint_release_cb;
    stoolap_hton->savepoint_rollback = stoolap_savepoint_rollback_cb;

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
    // Schema-only instances (mysql_create_frm_image) have a NULL share.
    // No native rowid: position()/rnd_pos() memcpy the full record via `ref`.
    if (table_arg) {
        ref_length = table_arg->reclength;
    }
}

ulonglong ha_stoolap::table_flags() const {
    // No HA_HAS_RECORDS on purpose: MariaDB's optimized-away COUNT(*) would
    // turn records() into a user-visible result, and Stoolap's bare
    // table-count metadata isn't MVCC-safe enough for that contract.
    // HA_PRIMARY_KEY_REQUIRED_FOR_{DELETE,POSITION}: our SQL builder
    // identifies rows by PK; without these flags the optimizer may strip
    // PK columns from read_set and UPDATE/DELETE would target the wrong row.
    return HA_REC_NOT_IN_SEQ | HA_NULL_IN_KEY | HA_BINLOG_ROW_CAPABLE |
           HA_PRIMARY_KEY_REQUIRED_FOR_DELETE |
           HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
           HA_CAN_DIRECT_UPDATE_AND_DELETE;
}

ulong ha_stoolap::index_flags(uint inx, uint part, bool /*all_parts*/) const {
    // Numeric/timestamp/boolean parts: full range-scan + ordered iter; their
    // byte-compare matches MariaDB's expected order.
    // ci string parts: drop everything (return 0) so the optimizer falls
    // back to a full scan where MariaDB applies its own collation per row.
    // _bin string parts: full flags (byte compare agrees).
    // DECIMAL/ENUM/SET: drop everything; ref access would build numeric-vs-
    // text predicates against stoolap's TEXT storage.
    // HA_KEY_SCAN_NOT_ROR disables index_merge_intersect (our scans aren't
    // in rowid order; we have no rowid concept).
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

    // Reconcile: MariaDB may have a .frm while stoolap is missing the table
    // (fresh datadir, wipe, DSN switch). CREATE TABLE IF NOT EXISTS no-ops
    // when present. The reconciled-set cache short-circuits the next open()
    // (called per-statement in tight loops); invalidates on DROP/TRUNCATE/RENAME.
    if (!g_engine.is_reconciled(stoolap_table_)) {
        auto* ctx = ha_thd() ? get_thd_ctx(ha_thd()) : nullptr;
        StoolapDB* d = (ctx && ctx->db()) ? ctx->db() : db_ensure();
        if (!d) return HA_ERR_OUT_OF_MEM;

        std::string sql = build_create_sql(stoolap_table_, table,
                                           /*if_not_exists=*/true);
        if (sql.empty()) return HA_ERR_UNSUPPORTED;
        if (stoolap_exec(d, sql.c_str(), nullptr) != STOOLAP_OK) {
            auto verr = fetch_db_error(d);
            sql_print_error("stoolap: schema reconcile failed for '%s': %s",
                            stoolap_table_.c_str(), verr.details.message);
            return report_stoolap_error(verr);
        }
        // Re-emit secondary indexes too: a wiped stoolap-side table comes
        // back without them, leaving "index-served" queries on a full scan.
        create_secondary_indexes(d, stoolap_table_, table);
        g_engine.mark_reconciled(stoolap_table_);
    }
    return 0;
}

int ha_stoolap::close() {
    // Release scan state now -- a large fetch_all buffer would otherwise
    // live until the handler is destroyed.
    reset_scan_state();
    bulk_insert_stmt_.reset();
    insert_sql_.clear();
    db_.reset();
    stoolap_table_.clear();
    return 0;
}

// Stoolap's CREATE INDEX is single-column only; composite KEY (a, b) gets
// an index on the leading column (b filters per row). Without it, MariaDB
// advertises an index with no physical backing and degrades to full scan.
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

// A single StoolapDB* is unsafe for concurrent use: concurrent DDL
// through g_engine.raw() would race both the executor and the per-handle
// error buffer, leaking error messages across threads. Use the THD clone.
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
        auto verr = fetch_db_error(db);
        sql_print_error("stoolap: CREATE TABLE failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
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
        auto verr = fetch_db_error(db);
        sql_print_error("stoolap: DROP TABLE failed: %s", verr.details.message);
        return report_stoolap_error(verr);
    }
    g_engine.drop_reconciled(flat);
    g_engine.records_drop(flat);
    // Re-CREATE TABLE of the same name must start AI fresh, not from the
    // dropped table's leftover counter.
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
        auto verr = fetch_db_error(db);
        sql_print_error("stoolap: RENAME TABLE failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
    }
    g_engine.drop_reconciled(flat_from);
    g_engine.mark_reconciled(flat_to);
    // Move records-cache entry so the renamed table doesn't pay a fresh COUNT.
    uint64_t cnt = 0;
    if (g_engine.records_lookup(flat_from, &cnt)) {
        g_engine.records_set(flat_to, cnt);
    }
    g_engine.records_drop(flat_from);
    g_engine.ai_invalidate();
    return 0;
}

namespace {

// Point Fields at a row buffer that isn't table->record[0].
void move_fields(TABLE* t, my_ptrdiff_t d) {
    if (!d) return;
    for (uint i = 0; i < t->s->fields; ++i)
        t->field[i]->move_field_offset(d);
}

// For TEXT columns the caller owns a std::string (`text_holder`) that
// keeps StoolapValue.text.ptr valid for the duration of the FFI call.
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
            // BIGINT UNSIGNED > INT64_MAX is the only narrower-than-i64
            // case we can't represent: val_int() hands us a negative
            // longlong that would land as negative i64 in stoolap and
            // compare wrong on range/order. Reject rather than corrupt.
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

    // No PK + byte-identical duplicates + filesort+LIMIT path: each per-row
    // UPDATE/DELETE callback would over-mutate every dup. Stoolap has no
    // rowid for tiebreaking and its UPDATE/DELETE grammar has no LIMIT,
    // so no byte-side fix exists. Direct DML (cond_push) still works.
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

// Bounds memory on huge INSERT...VALUES / SELECT / LOAD DATA (~5MB at
// 8 columns) while keeping per-flush fixed cost a small fraction of
// throughput. Stays well under int32_t row_count for the batch FFI.
static constexpr size_t kBulkFlushRows = 50000;

void ha_stoolap::start_bulk_insert(ha_rows rows, uint /*flags*/) {
    bulk_active_ = false;
    bulk_owns_tx_ = false;
    bulk_params_.clear();
    bulk_text_holders_.clear();

    // Auto-commit path uses stoolap_stmt_exec_batch (one tx, one fsync).
    // Inside an explicit user tx, we still skip per-row parse by preparing
    // once and looping tx_stmt_exec at flush. end_bulk_insert dispatches.

    // INSERT IGNORE / REPLACE / ODKU need per-row HA_ERR_FOUND_DUPP_KEY
    // callbacks for recovery. stmt_exec_batch is all-or-nothing -- one dup
    // would silently drop every non-conflicting row.
    if (stoolap_thd_needs_per_row_dup_handling(ha_thd())) {
        return;
    }

    bulk_active_ = true;

    // Cap the up-front reserve at the flush threshold so multi-million-row
    // INSERT...VALUES doesn't allocate the whole batch before flushing.
    // INSERT...SELECT often passes 0 (unknown source size); use the cap.
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
            auto verr = fetch_db_error(db_for_prepare);
            sql_print_error(
                "stoolap: prepare bulk INSERT failed: %s",
                *verr.details.message ? verr.details.message : "(no detail)");
            bulk_params_.clear();
            bulk_text_holders_.clear();
            return report_stoolap_error(verr);
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
        StoolapErrorView verr = (ctx && ctx->has_tx())
                                    ? fetch_tx_error(ctx->tx())
                                    : fetch_stmt_error(stmt_raw);
        if (verr.code == STOOLAP_ERR_OK || !*verr.details.message) {
            verr = fetch_db_error(db_for_prepare);
        }
        sql_print_error(
            "stoolap: batch INSERT failed: %s",
            *verr.details.message ? verr.details.message : "(no detail)");
        invalidate_records_cache();
        int err = report_stoolap_error(verr);
        if (err == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = errkey_from_view(verr, table->s);
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

    // Keep AI allocator in sync with explicit-id INSERTs (cross-connection).
    // Skip non-positive: signed -2 casts to ULLONG_MAX-1 and the next
    // generated insert fails ER_AUTOINC_READ_FAILED.
    if (table->found_next_number_field) {
        Field* aifield = table->found_next_number_field;
        const longlong sv = aifield->val_int();
        const bool unsigned_col = (aifield->flags & UNSIGNED_FLAG) != 0;
        if (unsigned_col || sv > 0) {
            const ulonglong v = static_cast<ulonglong>(sv);
            g_engine.ai_note_explicit(stoolap_table_, static_cast<uint64_t>(v));
        }
    }

    // Stoolap's semantic-cache reuses parsed plans by SQL string hash;
    // no plugin-side StoolapStmt cache needed.
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

    // Buffer for end_bulk_insert / kBulkFlushRows-triggered flush.
    if (bulk_active_) {
        const size_t holders_base = bulk_text_holders_.size();
        const size_t params_base = bulk_params_.size();
        for (uint i = 0; i < table->s->fields; ++i) {
            bulk_text_holders_.emplace_back();
            Field* f = table->field[i];
            StoolapValue v{};
            if (!extract_field(f, v, bulk_text_holders_.back())) {
                err = HA_ERR_UNSUPPORTED;
                // Roll back this row's partial entries: leaving cols
                // 0..k-1 in bulk_params_ would misalign every following
                // row's column indices in the batch.
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
        // Eager flush when over threshold. In autocommit mode, wrap the
        // chunked flushes in one outer tx so the whole bulk statement is
        // atomic (stmt_exec_batch otherwise commits per-chunk).
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
                // Some bulk INSERT paths skip external_lock; register here
                // so commit/rollback callbacks fire at statement end.
                trans_register_ha(ha_thd(), /*all=*/false, stoolap_hton,
                                  /*flags=*/0);
            }
            int frc = flush_bulk_buffer();
            if (frc) {
                if (bulk_owns_tx_ && ctx && ctx->has_tx()) {
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
                auto verr = fetch_error_via(ctx, db_raw());
                sql_print_error("stoolap: INSERT failed: %s",
                                verr.details.message);
                err = report_stoolap_error(verr);
                if (err == HA_ERR_FOUND_DUPP_KEY) {
                    last_dup_key_ = errkey_from_view(verr, table_share);
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

    // SET from new_data, WHERE from old_data: UPDATE t SET ... WHERE ... LIMIT 1.
    std::string sql = "UPDATE ";
    sql += quote_ident(stoolap_table_);
    sql += " SET ";

    std::vector<StoolapValue> params;
    std::vector<std::string> text_holders;
    params.reserve(table->s->fields * 2);
    text_holders.reserve(table->s->fields * 2);

    int err = 0;
    int next_param = 1;

    // SET filters: write_set hint AND bytewise old-vs-new compare. The
    // compare matters for ODKU/REPLACE which re-supply every column of
    // the existing row including the PK; stoolap rejects re-assigning a
    // PK column even with the same value.
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
            auto verr = fetch_error_via(ctx, db_raw());
            sql_print_error("stoolap: UPDATE failed: %s", verr.details.message);
            err = report_stoolap_error(verr);
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
            auto verr = fetch_error_via(ctx, db_raw());
            sql_print_error("stoolap: DELETE failed: %s", verr.details.message);
            err = report_stoolap_error(verr);
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
        auto verr = fetch_db_error(db_raw());
        sql_print_error("stoolap: ANALYZE failed: %s", verr.details.message);
        return report_stoolap_error(verr);
    }
    return 0;
}

const COND* ha_stoolap::cond_push(const COND* cond) {
    if (!cond) return nullptr;
    THD* thd = ha_thd();
    if (!thd) return cond;

    // Accept (return null) only when the engine will actually filter:
    // the direct-DML path forwards thd->query() with WHERE intact and
    // stoolap re-evaluates. Returning null on a shape that ends up
    // per-row would silently mass-mutate the table.
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
    // Non-zero falls back to per-row update_row(); never error here.
    return stoolap_pushdown::can_direct_modify(ha_thd()) ? 0
                                                         : HA_ERR_WRONG_COMMAND;
}

int ha_stoolap::direct_update_rows(ha_rows* update_rows, ha_rows* found_rows) {
    // pre_direct_update_rows may have run already and stashed the count.
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
    // No found-but-unchanged tracking; report found == affected.
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

// MariaDB 11.4 gates direct DML on these; the base impl returns
// HA_ERR_WRONG_COMMAND. The actual modify runs in pre_direct_*_rows
// (some paths use it as the work site; the short-circuit handles
// follow-up direct_*_rows calls).
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
    // Route via autocommit db (stoolap blocks TRUNCATE inside a tx).
    // Cache invalidate AFTER success: a pre-emptive set(0) would leave
    // the optimizer seeing 0 while a rejected TRUNCATE keeps the rows.
    std::string sql = "TRUNCATE TABLE ";
    sql += quote_ident(stoolap_table_);

    if (stoolap_exec(db_ensure(), sql.c_str(), nullptr) != STOOLAP_OK) {
        auto verr = fetch_db_error(db_raw());
        sql_print_error("stoolap: TRUNCATE failed: %s", verr.details.message);
        return report_stoolap_error(verr);
    }
    set_count_exact(0);
    // Process-wide allocator may still reflect pre-truncate counter.
    g_engine.ai_invalidate();
    return 0;
}

using stoolap_mariadb::pkt_le32;
using stoolap_mariadb::pkt_parse_header;
using stoolap_mariadb::pkt_skip_value;
using stoolap_mariadb::pkt_store_value;

std::string ha_stoolap::build_scan_columns() {
    // Project read_set | write_set: write_set covers UPDATE's old/new
    // compare; ORDER BY columns are already in read_set (filesort would
    // read garbage otherwise). Only-read_set broke ORDER BY paths.
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
        // No bits set -- COUNT(*)-shape needs one row per stoolap row.
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
        auto verr = fetch_error_via(ctx, db_raw());
        sql_print_error("stoolap: SELECT failed: %s", verr.details.message);
        return report_stoolap_error(verr);
    }

    // Buffered (fetch_all) wins on full scans (~5x less FFI traffic) but
    // loses on early-exit plans. Stay streaming when LIMIT is explicit.
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
    // Take ownership of stoolap's allocation; std::vector::assign would memcpy.
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
    // Inverse of scan_proj_: cell index -> field index, for O(1) lookup
    // per cell in rnd_next.
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
    reset_scan_state();
    return 0;
}

int ha_stoolap::rnd_next(uchar* buf) {
    // Buffered path: parse next row from the packed fetch_all buffer.
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
        // On parse error, clamp rows_left to 0 so subsequent rnd_next
        // returns EOF (the buffer offset is unrecoverable).
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
    // We tried bitmap-gated projection (read_set | write_set) but MariaDB's
    // filesort / temp-table / GROUP BY paths sometimes consult columns
    // outside the bitmap for sort keys, producing empty results on
    // ORDER BY ... LIMIT N. Populating every projected column is the only
    // correct choice without diving into the optimizer hooks.
    for (uint i = 0; i < table->s->fields; ++i) {
        const int col = (i < scan_proj_.size()) ? scan_proj_[i] : -1;
        if (col < 0) continue;
        Field* f = table->field[i];
        // Skip column_is_null FFI for NOT NULL columns (schema guarantees);
        // for nullable, set_notnull() clears stale 1s in the null bitmap.
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
                // Preserve UNSIGNED_FLAG: without it, INT UNSIGNED > INT32_MAX
                // stores as negative i64 and BIGINT UNSIGNED > INT64_MAX
                // silently corrupts. write_row already refuses BIGINT
                // UNSIGNED > INT64_MAX, so the round-trip is faithful.
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
    // BLOB hazard: a BLOB Field's in-record bytes are (length, ptr) where
    // ptr is owned by the prior rnd_next's value-store and overwritten by
    // the next rnd_next. Restoring ref bytes verbatim resurrects a freed
    // pointer; refuse until rnd_pos is rebuilt to re-fetch by PK.
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
    // rnd_next prefers scan_buf_ over scan_; clear so a leftover buffer
    // from a prior rnd_init doesn't shadow this index stream.
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

    // ci-string ref: MariaDB's ci collations case+accent fold; stoolap
    // compares bytes. Drop the engine-side WHERE, pull all rows in index
    // order, and ci-filter each row in index_next via Field::cmp (the
    // real CHARSET_INFO comparator). Join ref drops the residual filter
    // and IN-list ref re-applies the IN per value, so we can't rely on
    // "Using where". stoolap_trust_binary_strings opts out.
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
    // Trigger ci fallback for ANY bound ci part: KEY(n, s) on (INT, ci
    // VARCHAR) ref-binds both, and the bytewise predicate on `s` would
    // miss accent-equivalent rows. index_next ci-compares every bound part.
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
        // index_next does the work; strip the trailing " WHERE ".
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
                // NULL with range op = no rows; equality uses IS NULL.
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

    // Skip ORDER BY when an exact lookup is bound by a unique key (one
    // row max). For unsorted forward scans we also drop it; backward
    // scans still need ORDER BY DESC so index_next can forward-iterate.
    const bool exact_unique_lookup =
        find_flag == HA_READ_KEY_EXACT &&
        nparts == key_info.user_defined_key_parts &&
        (key_info.flags & HA_NOSAME);
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
        auto verr = fetch_error_via(ctx, db_raw());
        sql_print_error("stoolap: index_read SELECT failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
    }
    scan_.reset(rows);

    if (ci_collation_lookup) {
        // Search-key bytes are in table->record[1] from key_restore.
        // index_next will Field::cmp every bound part per row.
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
    // One-row probe (MIN/MAX, existence). Without LIMIT 1 we'd pay for
    // sorting the whole stream and discard everything after the first row.
    sql += " LIMIT 1";
    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    int rc = query_via(ctx, db_raw(), sql.c_str(), &rows);
    if (rc != STOOLAP_OK) {
        auto verr = fetch_error_via(ctx, db_raw());
        sql_print_error("stoolap: index_first SELECT failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
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
        auto verr = fetch_error_via(ctx, db_raw());
        sql_print_error("stoolap: index_last SELECT failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
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

    // Punt ci string ranges to handler::read_range_first (it does
    // index_read_map(start) + per-row compare_key(end) using MariaDB's
    // collation). _bin / BINARY / BLOB are byte-safe; NEWDECIMAL/ENUM/SET
    // still punt (encoded bound semantics not re-validated).
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

    // Lexicographic-tuple semantics: (a,b) >= (1,5) is NOT a >= 1 AND b >= 5
    // (which would be empty when paired with the upper bound). Expand to
    //   (a > 1) OR (a = 1 AND b >= 5)   -- lower bound
    //   (a < 2) OR (a = 2 AND b <= 3)   -- upper bound
    // evaluated correctly by stoolap's expression VM.
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

    // SQL op for the last bound part; earlier parts are always equality.
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
        // Upper bound convention: HA_READ_AFTER_KEY = "<= K" (inclusive),
        // HA_READ_BEFORE_KEY = "< K" (exclusive).
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
    // deque, not vector: append_bound runs twice and stashes
    // text_holders.back().data() into StoolapValue.text.ptr; a vector
    // realloc between phases would invalidate pointers from phase one.
    std::deque<std::string> text_holders;
    int next_param = 1;
    bool any_pred = false;

    // For nparts > 1 with non-eq last_op emits the OR-chain:
    //   (p0 <strict> v0) OR (p0 = v0 AND p1 <last_op> v1)
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
        // Strict variant for OR-chain prefix legs: ">=" / ">" -> ">".
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

        // OR-chain for composite non-eq bounds.
        if (nparts > 1 && !is_eq_op) {
            // Bind every key-part value once; each is referenced per leg.
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

    // Skip ORDER BY when an exact unique-index lookup (one row) or when
    // MariaDB's `sorted` flag says the range plan doesn't need order.
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
        auto verr = fetch_error_via(ctx, db_raw());
        sql_print_error("stoolap: read_range query failed: %s",
                        verr.details.message);
        return report_stoolap_error(verr);
    }
    scan_.reset(rows);
    int next_rc = rnd_next(table->record[0]);
    return (next_rc == HA_ERR_END_OF_FILE) ? HA_ERR_END_OF_FILE : next_rc;
}

int ha_stoolap::read_range_next() {
    // Route through index_next so the ci-collation filter fires on punted
    // ranges. Non-ci scans just hit rnd_next via index_next; no overhead.
    return index_next(table->record[0]);
}

void ha_stoolap::get_auto_increment(ulonglong offset, ulonglong increment,
                                    ulonglong nb_desired_values,
                                    ulonglong* first_value,
                                    ulonglong* nb_reserved_values) {
    // Reserve ids process-wide: per-connection caches collide under
    // concurrent sessions + explicit high-id inserts. Reserving with
    // step=increment makes our cursor jump past every id MariaDB
    // logically issues in this batch (offset/increment is the multi-
    // writer replication ladder).
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
        // Floor at 0: an explicit negative AI value (INSERT VALUES (-2)
        // ...) gives a negative MAX(); uint64 wrap + 1 overflows and the
        // next generated insert fails ER_AUTOINC_READ_FAILED.
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
    // Lookup order: handler-local -> tx-local (in tx) or engine-global
    // (autocommit) -> live count via FFI. The tx-local layer is
    // per-THD so snapshot-visible counts never leak between connections.
    if (cached_records_valid_) return cached_records_;
    if (stoolap_table_.empty()) return stats.records;

    THD* thd = ha_thd();
    // Optimizer/stat callers can reach here before external_lock has
    // registered the tx. Register now so COUNT observes the same view
    // the later row access path will use; otherwise a pre-lock probe
    // seeds planning with an autocommit-visible count.
    if (thd && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {
        if (register_trx(thd) != 0) return stats.records;
    }
    auto* ctx = thd ? get_thd_ctx(thd) : nullptr;
    // Skip cross-session cache during an open tx: tx-side counts see this
    // session's uncommitted writes -- right for us, wrong to publish.
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

    // O(1) MVCC-safe count: tx-side sees own uncommitted writes,
    // db-side returns autocommit-visible count via SegmentedTable.
    stoolap_mariadb::g_stats.records_live_counts.fetch_add(
        1, std::memory_order_relaxed);
    uint64_t count = 0;
    int rc = count_via(ctx, db_raw(), stoolap_table_.c_str(), &count);
    if (rc != STOOLAP_OK) return stats.records;

    cached_records_ = static_cast<ha_rows>(count);
    cached_records_valid_ = true;
    stats.records = cached_records_;
    if (in_tx) {
        ctx->records_set(stoolap_table_, count);
    }
    // Never publish here to the process-wide cache: pre-external_lock
    // probes can't reliably distinguish autocommit from in-tx, so a
    // tx-visible count could leak across sessions. DDL (TRUNCATE) seeds
    // exact global values via set_count_exact instead.
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
    // ordering and index-vs-scan choices. No stoolap-side histograms,
    // so divide row count by rec_per_key when populated, else estimate
    // by bound shape.
    if (inx >= table->s->keys) return 10;
    const KEY& k = table->key_info[inx];
    // records_in_range() can fire before HA_STATUS_VARIABLE has warmed
    // stats.records, so use cached_records directly.
    const ha_rows cached_total = cached_records();
    const ha_rows total = cached_total ? cached_total : 1;

    // ci-leading ref runs a full scan in index_next (no stoolap-side ci
    // index). Report `total` so the planner treats ci ref as no cheaper
    // than a scan, picking sane join order / hash join. trust_binary_strings
    // opts into byte-equal lookup where ref really is O(1)-ish.
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

    // Coarse estimate by bound shape (HA_KEY_SCAN_NOT_ROR keeps us out
    // of index_merge_intersect plans):
    //   exact eq on non-unique key: ~0.1%
    //   tight range (both bounds):  ~1%
    //   half-open range:            ~10%
    // Floor at 10 so tiny tables don't pick bad plans.
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
    // We have no file length (stoolap is in-memory / behind FFI); the
    // default scan_time() returns 0. Approximate as stats.records so the
    // ci ref cost gate below has a non-zero quantity to multiply by.
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
    // ci fallback walks every row in index_next, so each probe is a
    // full scan. Override caller's `rows` (which is rec_per_key and
    // would leak ci-folded counts across sessions if persisted).
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

    if (flag & HA_STATUS_ERRKEY) {
        // get_dup_key() calls us to re-publish the violated key.
        errkey = last_dup_key_;
    }

    // Cache-only refresh on purpose: info() fires several times per
    // statement and a mutation loop invalidates after each row, so
    // calling cached_records() here would re-COUNT N times per stmt.
    // User-visible SELECT COUNT(*) still routes through records().
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
                // Both caches missed: keep prior stats.records; the 1000
                // placeholder below avoids "Impossible WHERE" on a fresh
                // load.
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

// MYSQL_SYSVAR writes here; the update callback mirrors into the atomic
// in stoolap_bridge.cc that the hot paths read without taking THD ctx.
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

static MYSQL_THDVAR_BOOL(
    trust_binary_strings, PLUGIN_VAR_OPCMDARG,
    "Push string predicates / sorts / groups to stoolap even on "
    "non-binary VARCHAR/TEXT columns. Stoolap compares bytes; MariaDB "
    "compares with collation. Caller takes responsibility for the "
    "semantic difference. Off by default.",
    /*check=*/nullptr, /*update=*/nullptr,
    /*default=*/false);

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

// SHOW_LONGLONG over std::atomic<uint64_t>: same 8 bytes; relaxed load
// is a plain load on aarch64; approximate counts are fine for SHOW STATUS.
// Single-line _SS_PTR macro to avoid LLVM 18 vs 22 backslash-continuation
// disagreement that would break the format gate.
#define _SS_PTR(member) \
    reinterpret_cast<char*>(&stoolap_mariadb::g_stats.member)

static struct st_mysql_show_var stoolap_status_vars[] = {
    {"Stoolap_pushdown_hits", _SS_PTR(pushdown_hits), SHOW_LONGLONG},
    {"Stoolap_pushdown_misses", _SS_PTR(pushdown_misses), SHOW_LONGLONG},
    {"Stoolap_direct_modify_hits", _SS_PTR(direct_modify_hits), SHOW_LONGLONG},
    {"Stoolap_records_live_counts", _SS_PTR(records_live_counts),
     SHOW_LONGLONG},
    {"Stoolap_buffered_scans", _SS_PTR(buffered_scans), SHOW_LONGLONG},
    {"Stoolap_buffered_rows", _SS_PTR(buffered_rows), SHOW_LONGLONG},
    // Strict drift counter (case_18 pins at 0); see PushdownStats.
    {"Stoolap_unmapped_errors", _SS_PTR(unmapped_errors), SHOW_LONGLONG},
    // Stoolap-side typed-error gap signal; allowlisted in runner.
    {"Stoolap_typed_fallback_hits", _SS_PTR(typed_fallback_hits),
     SHOW_LONGLONG},
    // Per-phase ns; divide by Stoolap_perf_query_count (or
    // _next_row_count) for averages. Two clock_gettime per phase (~20ns).
    {"Stoolap_perf_factory_setup_ns", _SS_PTR(perf_factory_setup_ns),
     SHOW_LONGLONG},
    {"Stoolap_perf_eager_query_ns", _SS_PTR(perf_eager_query_ns),
     SHOW_LONGLONG},
    {"Stoolap_perf_init_scan_ns", _SS_PTR(perf_init_scan_ns), SHOW_LONGLONG},
    {"Stoolap_perf_next_row_ns", _SS_PTR(perf_next_row_ns), SHOW_LONGLONG},
    {"Stoolap_perf_end_scan_ns", _SS_PTR(perf_end_scan_ns), SHOW_LONGLONG},
    {"Stoolap_perf_query_count", _SS_PTR(perf_query_count), SHOW_LONGLONG},
    {"Stoolap_perf_next_row_count", _SS_PTR(perf_next_row_count),
     SHOW_LONGLONG},
    {nullptr, nullptr, SHOW_UNDEF},
};

#undef _SS_PTR

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
