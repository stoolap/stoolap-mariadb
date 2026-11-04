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

#include "ha_stoolap_select.h"

#include "my_global.h"
#include "field.h"
#include "key.h"
#include "table.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "item_sum.h"
#include "item_subselect.h"
#include "select_handler.h"
#include "derived_handler.h"
#include "log.h"
#include "mysqld_error.h"  // ER_GET_ERRMSG

#include "stoolap_bridge.h"
#include "stoolap_packet.h"
#include "stoolap_sqlgen.h"
#include "stoolap_thd_context.h"
#include "stoolap_thd_inspect.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace stoolap_mariadb {
class Engine;
}
extern stoolap_mariadb::Engine g_engine;

// Defined in ha_stoolap.cc. Pushdown factories call register_trx
// defensively because create_select can fire before any handler's
// external_lock; without it a pushed SELECT inside START TRANSACTION
// would miss the session's snapshot.
extern int register_trx(THD* thd);

// Defined in ha_stoolap.cc. `message` is valid only until the next FFI
// call on the originating handle.
struct StoolapErrorView {
    const char* message;
};

extern StoolapErrorView fetch_db_error(StoolapDB* db);
extern StoolapErrorView fetch_tx_error(StoolapTx* tx);
extern int report_stoolap_error(const StoolapErrorView& v);
extern unsigned errkey_from_view(const StoolapErrorView& v, TABLE_SHARE* share);

// brewed mariadbd doesn't ship my_print_error_service; bind directly.
#undef my_error
#undef my_printf_error
extern "C" void my_error(unsigned int nr, unsigned long MyFlags, ...);
extern "C" void my_printf_error(unsigned int nr, const char* fmt,
                                unsigned long MyFlags, ...);

namespace {

/** Walk every base table referenced by a SELECT_LEX and confirm each one
 *  belongs to stoolap_hton; we have no cross-engine fallback for direct
 *  DML, so a single non-stoolap table disqualifies the push and the
 *  caller falls back to MariaDB's per-row callback path.
 *
 *  Iterates this SELECT_LEX's local FROM list (`next_local`) and recurses
 *  explicitly into derived tables' inner SELECT_LEX(s). The global
 *  TABLE_LIST chain (`next_global`) mixes in entries from other parts
 *  of the LEX (subqueries in WHERE, etc.); explicit recursion is both
 *  narrower and easier to reason about.
 */
bool every_table_is_stoolap_impl(SELECT_LEX* sel_lex, bool& has_any_table);

bool every_table_is_stoolap_impl(SELECT_LEX* sel_lex, bool& has_any_table) {
    if (!sel_lex) return true;
    for (TABLE_LIST* tl = sel_lex->get_table_list(); tl; tl = tl->next_local) {
        if (tl->derived) {
            // Recursively check every leg of the derived's unit. The
            // derived TABLE_LIST itself is not a stoolap leaf, but the
            // tables it references must be stoolap-only for the outer
            // pushdown to handle the whole query as one stoolap plan.
            for (SELECT_LEX* inner = tl->derived->first_select(); inner;
                 inner = inner->next_select()) {
                if (!every_table_is_stoolap_impl(inner, has_any_table))
                    return false;
            }
            continue;
        }
        if (!tl->table) continue;
        has_any_table = true;
        if (!tl->table->file || tl->table->file->ht != stoolap_hton)
            return false;
    }
    // Subqueries (Item_subselect) live in inner units off SELECT_LEX::slave,
    // not on the FROM list. Walk those too so a non-stoolap table inside
    // EXISTS/IN/scalar subqueries disqualifies the push -- and so the
    // identifier rewriter (collect_leaf_names) sees their tables.
    for (SELECT_LEX_UNIT* u = sel_lex->first_inner_unit(); u;
         u = u->next_unit()) {
        for (SELECT_LEX* sub = u->first_select(); sub;
             sub = sub->next_select()) {
            if (!every_table_is_stoolap_impl(sub, has_any_table)) return false;
        }
    }
    return true;
}

bool every_table_is_stoolap(SELECT_LEX* sel_lex, bool& has_any_table) {
    has_any_table = false;
    return every_table_is_stoolap_impl(sel_lex, has_any_table);
}

}  // namespace

namespace stoolap_pushdown {

// Statement shapes the direct path can take: a single-table UPDATE or
// DELETE without ORDER BY / LIMIT, outside triggers and stored routines,
// over stoolap tables only. Whether its expressions translate is
// plan_direct_modify's call (stoolap_sqlgen).
bool can_direct_modify(THD* thd) {
    if (!thd) return false;
    if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare()) return false;
    if (thd->spcont) return false;
    if (thd->in_sub_stmt) return false;

    const int cmd = thd->lex->sql_command;
    if (cmd != SQLCOM_UPDATE && cmd != SQLCOM_DELETE) return false;

    SELECT_LEX* sel = thd->lex->first_select_lex();
    if (!sel) return false;

    if (sel->table_list.elements != 1)
        return false;  // multi-table UPDATE/DELETE
    if (sel->order_list.elements || sel->limit_params.select_limit)
        return false;  // ORDER BY / LIMIT
    if (sel->uncacheable & UNCACHEABLE_SIDEEFFECT) return false;

    bool any = false;
    if (!every_table_is_stoolap(sel, any)) return false;
    return any;
}

bool plan_direct_modify(THD* thd, TABLE* table, Item* cond, List<Item>* fields,
                        std::string* sql) {
    if (!can_direct_modify(thd) || !table) return false;
    std::string guard, why;
    bool ok;
    if (thd->lex->sql_command == SQLCOM_UPDATE) {
        if (!fields) return false;
        // Without CLIENT_FOUND_ROWS the client is told the rows changed,
        // so only those are updated (and counted).
        const bool changed_only =
            !(thd->client_capabilities & CLIENT_FOUND_ROWS);
        ok = stoolap_sqlgen::gen_update(thd, table, cond, *fields,
                                        thd->lex->value_list, changed_only, sql,
                                        &guard, &why);
    } else {
        ok = stoolap_sqlgen::gen_delete(thd, table, cond, sql, &why);
    }
    if (!ok) {
        if (stoolap_thd_explain_pushdown(thd))
            sql_print_information("stoolap[explain]: no direct DML: %s",
                                  why.c_str());
        return false;
    }
    if (guard.empty()) return true;

    // Some SET value is not a constant: a row it would not fit (out of
    // range, too long, NULL into NOT NULL) needs MariaDB's conversion,
    // with its error or warning, so the row pump runs the statement.
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db || register_trx(thd) != 0) return false;
    StoolapRows* rows = nullptr;
    const int rc = ctx->has_tx()
                       ? stoolap_tx_query(ctx->tx(), guard.c_str(), &rows)
                       : stoolap_query(db, guard.c_str(), &rows);
    if (rc != STOOLAP_OK || !rows) return false;
    const bool misfit = stoolap_rows_next(rows) == STOOLAP_ROW;
    stoolap_rows_close(rows);
    if (misfit && stoolap_thd_explain_pushdown(thd))
        sql_print_information(
            "stoolap[explain]: no direct DML: a row "
            "needs MariaDB's conversion: %s",
            guard.c_str());
    return !misfit;
}

int run_direct_modify(THD* thd, const std::string& sql, ha_rows* affected,
                      unsigned* errkey_out) {
    if (sql.empty()) return HA_ERR_WRONG_COMMAND;
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db) return HA_ERR_INTERNAL_ERROR;
    if (register_trx(thd) != 0) return HA_ERR_INTERNAL_ERROR;

    int64_t rows_aff = 0;
    const int rc = ctx->has_tx()
                       ? stoolap_tx_exec(ctx->tx(), sql.c_str(), &rows_aff)
                       : stoolap_exec(db, sql.c_str(), &rows_aff);
    if (rc != STOOLAP_OK) {
        StoolapErrorView verr =
            ctx->has_tx() ? fetch_tx_error(ctx->tx()) : fetch_db_error(db);
        const char* msg = *verr.message ? verr.message : "unknown error";
        sql_print_error("stoolap: direct DML failed: %s", msg);
        // MariaDB's direct-DML path does not always call print_error
        // after the engine returns, so a mapped FK / dup-key would
        // otherwise rollback silently. Emit the concrete error number
        // for the mapped classes; report_stoolap_error already raised
        // 1296 for GENERIC/UNSUPPORTED, so skip those here.
        const int mapped = report_stoolap_error(verr);
        if (mapped == HA_ERR_FOUND_DUPP_KEY && errkey_out) {
            TABLE_LIST* tl = thd->lex->query_tables;
            TABLE_SHARE* share = (tl && tl->table) ? tl->table->s : nullptr;
            *errkey_out = errkey_from_view(verr, share);
        }
        switch (mapped) {
            case HA_ERR_FOUND_DUPP_KEY:
                my_printf_error(ER_DUP_ENTRY, "stoolap: %s", MYF(0), msg);
                break;
            case HA_ERR_NO_REFERENCED_ROW:
                my_printf_error(ER_NO_REFERENCED_ROW_2, "stoolap: %s", MYF(0),
                                msg);
                break;
            case HA_ERR_ROW_IS_REFERENCED:
                my_printf_error(ER_ROW_IS_REFERENCED_2, "stoolap: %s", MYF(0),
                                msg);
                break;
            case HA_ERR_LOCK_DEADLOCK:
                my_printf_error(ER_LOCK_DEADLOCK, "stoolap: %s", MYF(0), msg);
                break;
            case HA_ERR_TABLE_EXIST:
                my_printf_error(ER_TABLE_EXISTS_ERROR, "stoolap: %s", MYF(0),
                                msg);
                break;
            case HA_ERR_NO_SUCH_TABLE:
                my_printf_error(ER_NO_SUCH_TABLE, "stoolap: %s", MYF(0), msg);
                break;
            default:
                break;
        }
        return mapped;
    }
    if (affected) *affected = static_cast<ha_rows>(rows_aff);
    stoolap_mariadb::g_stats.direct_modify_hits.fetch_add(
        1, std::memory_order_relaxed);
    return 0;
}

}  // namespace stoolap_pushdown

// ===========================================================================
// Whole-SELECT pushdown
// ===========================================================================

namespace {

// Statement-level conditions for running a SELECT (or a set-operation
// leg, or a derived table's SELECT) in stoolap. Whether its expressions
// mean the same there is stoolap_sqlgen's call.
bool can_pushdown_select(THD* thd, SELECT_LEX* sel_lex,
                         SELECT_LEX_UNIT* sel_unit) {
    if (!thd || !sel_lex) return false;
    // Single-SELECT only. Unit / partial pushdown handled elsewhere.
    if (sel_unit) return false;
    // PREPARE: still has `?` placeholders; stoolap would reject. EXECUTE
    // lands here with bound params via LEX::param_list.
    if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare()) return false;
    // SP body text uses local names, not bound values; needs an Item-
    // walker we can't hook from the plugin. Triggers/sub_stmt: row pump
    // preserves MariaDB semantics for side-effects.
    if (thd->spcont) return false;
    if (thd->in_sub_stmt) return false;
    if (thd->lex->sql_command != SQLCOM_SELECT) return false;
    // SELECT INTO @var/OUTFILE/DUMPFILE subclass select_result_interceptor;
    // plain select_send (incl. binary protocol) is non-interceptor.
    if (thd->lex->result && thd->lex->result->result_interceptor() != nullptr)
        return false;
    if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT) return false;
    if (sel_lex->uncacheable & UNCACHEABLE_RAND) return false;
    // FOR UPDATE / LOCK IN SHARE MODE: lock-mode mapping non-trivial.
    if (sel_lex->select_lock != SELECT_LEX::select_lock_type::NONE)
        return false;

    // Every leaf table must be a stoolap table; cross-engine joins go to
    // the row pump (and Phase 3 will catch them via derived_handler).
    bool any = false;
    if (!every_table_is_stoolap(sel_lex, any) || !any) return false;

    // Bare COUNT(*) over a single base table: punt to row-pump aggregate.
    // Stoolap's bare COUNT can use non-MVCC-safe table-count metadata.
    // Derived FROMs still push (no records() shortcut applies).
    if (sel_lex->table_list.elements == 1 && sel_lex->item_list.elements == 1 &&
        sel_lex->where == nullptr && sel_lex->having == nullptr &&
        sel_lex->group_list.elements == 0 &&
        sel_lex->order_list.elements == 0 &&
        !sel_lex->limit_params.explicit_limit) {
        TABLE_LIST* sole = sel_lex->get_table_list();
        const bool sole_is_base = sole && !sole->derived;
        if (sole_is_base) {
            Item* it = sel_lex->item_list.head();
            if (it && it->type() == Item::SUM_FUNC_ITEM) {
                auto* sum = static_cast<Item_sum*>(it);
                if (sum->sum_func() == Item_sum::COUNT_FUNC &&
                    sum->get_arg_count() == 1) {
                    Item* a = sum->get_arg(0);
                    // COUNT(*) parses as COUNT(literal); COUNT(col) is
                    // Item_field. Bail only on the literal-arg shape so
                    // COUNT(col), which excludes NULLs, keeps pushing.
                    if (a && a->const_item()) return false;
                }
            }
        }
    }

    return true;
}

inline uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Single relaxed-load + branch when off; the timing path is dead weight
// for users who just want to run the plugin. Enable with
// `SET GLOBAL stoolap_perf_trace = 1` to populate Stoolap_perf_*.
inline bool perf_trace_on() {
    return stoolap_mariadb::g_perf_trace_enabled.load(
        std::memory_order_relaxed);
}

/**
 * Wraps a `StoolapRows*` opened by the eager stoolap_query in the
 * pushdown factory, immediately calls stoolap_rows_fetch_all to turn it
 * into a packed buffer, and parses the header. The handler then
 * delivers rows by walking the buffer in-process -- no per-cell FFI.
 */
struct PushdownPacked {
    stoolap_mariadb::StoolapBuffer buf;
    size_t pos = 0;          // current parse offset (past header)
    uint32_t cols = 0;       // declared column count
    uint32_t rows_left = 0;  // rows still to deliver
    bool ok = false;

    void take(stoolap_mariadb::RowsPtr rows) {
        if (!rows) return;
        uint8_t* raw = nullptr;
        int64_t blen = 0;
        int frc = stoolap_rows_fetch_all(rows.get(), &raw, &blen);
        // RowsPtr destructor closes the iterator after fetch_all consumed it.
        rows.reset();
        if (frc != STOOLAP_OK) {
            if (raw) stoolap_buffer_free(raw, blen);
            return;
        }
        buf.take(raw, blen);
        if (!stoolap_mariadb::pkt_parse_header(buf.data(), buf.size(), &pos,
                                               &cols)) {
            buf.reset();
            return;
        }
        if (pos + 4 > buf.size()) {
            buf.reset();
            return;
        }
        rows_left = stoolap_mariadb::pkt_le32(buf.data() + pos);
        pos += 4;
        ok = true;
    }
};

class ha_stoolap_select_handler : public select_handler {
public:
    ha_stoolap_select_handler(THD* thd_arg, SELECT_LEX* sel, std::string sql,
                              PushdownPacked packed)
        : select_handler(thd_arg, stoolap_hton, sel),
          sql_(std::move(sql)),
          packed_(std::move(packed)) {}

    // Unit overload: whole UNION/EXCEPT/INTERSECT pushdown.
    ha_stoolap_select_handler(THD* thd_arg, SELECT_LEX_UNIT* unit,
                              std::string sql, PushdownPacked packed)
        : select_handler(thd_arg, stoolap_hton, unit),
          sql_(std::move(sql)),
          packed_(std::move(packed)) {}

    ~ha_stoolap_select_handler() override = default;

protected:
    int init_scan() override {
        const bool trace = perf_trace_on();
        const uint64_t t0 = trace ? now_ns() : 0;
        // Factory ran stoolap_query then stoolap_rows_fetch_all. If the
        // packed buffer didn't materialise we have no data to deliver.
        const int rc = packed_.ok ? 0 : HA_ERR_INTERNAL_ERROR;
        if (trace) {
            stoolap_mariadb::g_stats.perf_init_scan_ns.fetch_add(
                now_ns() - t0, std::memory_order_relaxed);
        }
        return rc;
    }

    int next_row() override {
        const bool trace = perf_trace_on();
        const uint64_t t0 = trace ? now_ns() : 0;
        if (!packed_.ok || packed_.rows_left == 0) return HA_ERR_END_OF_FILE;

        const uint fields = table->s->fields;
        if (static_cast<uint>(packed_.cols) < fields) {
            my_printf_error(ER_GET_ERRMSG,
                            "stoolap pushdown: column count mismatch "
                            "(got %u, want %u)",
                            MYF(0), packed_.cols, fields);
            packed_.rows_left = 0;
            return HA_ERR_GENERIC;
        }

        const uint8_t* p = packed_.buf.data();
        const size_t len = packed_.buf.size();
        // The pushed SQL was built from the SELECT_LEX's item list, so
        // result cell c maps 1:1 to table->field[c]. Trailing extra
        // cells (schemas drifting under us) get skipped.
        int err = 0;
        for (uint c = 0; c < fields && !err; ++c) {
            err = stoolap_mariadb::pkt_store_value(p, len, &packed_.pos,
                                                   table->field[c]);
        }
        for (uint c = fields; c < packed_.cols && !err; ++c) {
            if (!stoolap_mariadb::pkt_skip_value(p, len, &packed_.pos))
                err = HA_ERR_GENERIC;
        }
        if (err) {
            packed_.rows_left = 0;
        } else {
            packed_.rows_left--;
        }
        if (trace) {
            stoolap_mariadb::g_stats.perf_next_row_ns.fetch_add(
                now_ns() - t0, std::memory_order_relaxed);
            stoolap_mariadb::g_stats.perf_next_row_count.fetch_add(
                1, std::memory_order_relaxed);
        }
        return err;
    }

    int end_scan() override {
        const bool trace = perf_trace_on();
        const uint64_t t0 = trace ? now_ns() : 0;
        packed_.buf.reset();
        if (trace) {
            stoolap_mariadb::g_stats.perf_end_scan_ns.fetch_add(
                now_ns() - t0, std::memory_order_relaxed);
        }
        return 0;
    }

private:
    std::string sql_;
    PushdownPacked packed_;
};

}  // namespace

namespace {

void count_miss() {
    stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
        1, std::memory_order_relaxed);
}

// Runs generated pushdown SQL eagerly and packs its rows. Eager, so any
// stoolap-side error (unknown function, type mismatch, ...) still falls
// back to MariaDB: once a factory returns a handler, MariaDB is committed
// to it. stoolap_prepare catches only syntax; semantic errors surface at
// execute time only.
bool run_pushdown(THD* thd, const std::string& sql, PushdownPacked* packed,
                  uint64_t* eager_ns) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db) return false;

    // Open the stoolap tx if the THD is inside an explicit BEGIN: the
    // factories run before external_lock would, and the query below must
    // see the session's snapshot. No-op outside an explicit transaction.
    if (register_trx(thd) != 0) return false;

    // Diagnostic: stoolap's own EXPLAIN of the pushed SQL (MariaDB's
    // EXPLAIN only says "PUSHED ..."). Costs an extra query, so opt-in.
    const bool explain = stoolap_thd_explain_pushdown(thd);
    if (explain) {
        sql_print_information("stoolap[explain]: %s", sql.c_str());
        std::string esql = "EXPLAIN " + sql;
        StoolapRows* erows = nullptr;
        const int erc = ctx->has_tx()
                            ? stoolap_tx_query(ctx->tx(), esql.c_str(), &erows)
                            : stoolap_query(db, esql.c_str(), &erows);
        if (erc == STOOLAP_OK && erows) {
            while (stoolap_rows_next(erows) == STOOLAP_ROW) {
                int64_t len = 0;
                const char* line = stoolap_rows_column_text(erows, 0, &len);
                if (line) {
                    sql_print_information("stoolap[explain]:   %.*s",
                                          static_cast<int>(len), line);
                }
            }
            stoolap_rows_close(erows);
        }
    }

    StoolapRows* raw = nullptr;
    const uint64_t t0 = eager_ns ? now_ns() : 0;
    const int rc = ctx->has_tx()
                       ? stoolap_tx_query(ctx->tx(), sql.c_str(), &raw)
                       : stoolap_query(db, sql.c_str(), &raw);
    if (eager_ns) *eager_ns = now_ns() - t0;
    if (rc != STOOLAP_OK) {
        if (explain) {
            const char* msg = ctx->has_tx() ? stoolap_tx_errmsg(ctx->tx())
                                            : stoolap_errmsg(db);
            sql_print_information("stoolap[explain]: not pushed: %s: %s",
                                  msg ? msg : "", sql.c_str());
        }
        return false;
    }
    packed->take(stoolap_mariadb::RowsPtr(raw));
    return packed->ok;
}

void explain_refusal(THD* thd, const std::string& why) {
    if (stoolap_thd_explain_pushdown(thd))
        sql_print_information("stoolap[explain]: not pushed: %s", why.c_str());
}

}  // namespace

namespace stoolap_pushdown {

// SQL comes from the prepared Item tree (stoolap_sqlgen). Placeholders of
// an EXECUTE are bound Item_params there and become literals, so no
// parameters are passed.
select_handler* create_stoolap_select_handler(THD* thd, SELECT_LEX* sel_lex,
                                              SELECT_LEX_UNIT* sel_unit) {
    const bool trace = perf_trace_on();
    const uint64_t t_factory_start = trace ? now_ns() : 0;
    std::string sql, why;
    if (!can_pushdown_select(thd, sel_lex, sel_unit)) {
        count_miss();
        return nullptr;
    }
    if (!stoolap_sqlgen::gen_select(thd, sel_lex, &sql, &why)) {
        explain_refusal(thd, why);
        count_miss();
        return nullptr;
    }
    PushdownPacked packed;
    uint64_t eager_ns = 0;
    if (!run_pushdown(thd, sql, &packed, trace ? &eager_ns : nullptr)) {
        count_miss();
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    auto* h = new ha_stoolap_select_handler(thd, sel_lex, std::move(sql),
                                            std::move(packed));
    if (trace) {
        stoolap_mariadb::g_stats.perf_eager_query_ns.fetch_add(
            eager_ns, std::memory_order_relaxed);
        const uint64_t total = now_ns() - t_factory_start;
        stoolap_mariadb::g_stats.perf_factory_setup_ns.fetch_add(
            total > eager_ns ? total - eager_ns : 0, std::memory_order_relaxed);
        stoolap_mariadb::g_stats.perf_query_count.fetch_add(
            1, std::memory_order_relaxed);
    }
    return h;
}

// A whole UNION / INTERSECT / EXCEPT statement.
select_handler* create_stoolap_unit_handler(THD* thd, SELECT_LEX_UNIT* unit) {
    if (!thd || !unit) return nullptr;
    SELECT_LEX* first = unit->first_select();
    // A single SELECT is the SELECT factory's.
    if (!first || !first->next_select()) return nullptr;
    for (SELECT_LEX* sel = first; sel; sel = sel->next_select()) {
        if (!can_pushdown_select(thd, sel, /*sel_unit=*/nullptr)) {
            count_miss();
            return nullptr;
        }
    }
    std::string sql, why;
    if (!stoolap_sqlgen::gen_unit(thd, unit, &sql, &why)) {
        explain_refusal(thd, why);
        count_miss();
        return nullptr;
    }
    PushdownPacked packed;
    if (!run_pushdown(thd, sql, &packed, nullptr)) {
        count_miss();
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    return new ha_stoolap_select_handler(thd, unit, std::move(sql),
                                         std::move(packed));
}

}  // namespace stoolap_pushdown

// ===========================================================================
// Derived-table pushdown
// ===========================================================================

namespace {

class ha_stoolap_derived_handler : public derived_handler {
public:
    ha_stoolap_derived_handler(THD* thd_arg, std::string sql,
                               PushdownPacked packed)
        : derived_handler(thd_arg, stoolap_hton),
          sql_(std::move(sql)),
          packed_(std::move(packed)) {}

    ~ha_stoolap_derived_handler() override = default;

    int init_scan() override { return packed_.ok ? 0 : HA_ERR_INTERNAL_ERROR; }

    int next_row() override {
        if (!packed_.ok || packed_.rows_left == 0) return HA_ERR_END_OF_FILE;

        const uint fields = table->s->fields;
        if (static_cast<uint>(packed_.cols) < fields) {
            my_printf_error(ER_GET_ERRMSG,
                            "stoolap derived: column count mismatch "
                            "(got %u, want %u)",
                            MYF(0), packed_.cols, fields);
            packed_.rows_left = 0;
            return HA_ERR_GENERIC;
        }
        const uint8_t* p = packed_.buf.data();
        const size_t len = packed_.buf.size();
        int err = 0;
        for (uint c = 0; c < fields && !err; ++c) {
            err = stoolap_mariadb::pkt_store_value(p, len, &packed_.pos,
                                                   table->field[c]);
        }
        for (uint c = fields; c < packed_.cols && !err; ++c) {
            if (!stoolap_mariadb::pkt_skip_value(p, len, &packed_.pos))
                err = HA_ERR_GENERIC;
        }
        if (err) {
            packed_.rows_left = 0;
        } else {
            packed_.rows_left--;
        }
        return err;
    }

    int end_scan() override {
        packed_.buf.reset();
        return 0;
    }

private:
    std::string sql_;
    PushdownPacked packed_;
};

}  // namespace

namespace stoolap_pushdown {

// A derived table (or its UNION) in a statement that also reads other
// engines' tables: stoolap computes it, MariaDB joins the rest.
derived_handler* create_stoolap_derived_handler(THD* thd, TABLE_LIST* derived) {
    if (!thd || !derived || !derived->derived) return nullptr;
    SELECT_LEX_UNIT* unit = derived->derived;
    if (!unit->first_select()) return nullptr;

    // This factory runs its query at once, during prepare. In a SELECT
    // over stoolap tables only, the select or unit handler takes the
    // whole statement, derived table included, and this result would be
    // thrown away; if they cannot, MariaDB materializes the derived table.
    if (thd->lex->sql_command == SQLCOM_SELECT) {
        SELECT_LEX* top = derived->select_lex;
        while (top && top->outer_select())
            top = top->outer_select();
        bool all_stoolap = top != nullptr, any = false;
        for (SELECT_LEX* sl = top ? top->master_unit()->first_select()
                                  : nullptr;
             sl && all_stoolap; sl = sl->next_select()) {
            bool leg_any = false;
            all_stoolap = every_table_is_stoolap(sl, leg_any);
            any |= leg_any;
        }
        if (all_stoolap && any) return nullptr;
    }
    for (SELECT_LEX* sel = unit->first_select(); sel;
         sel = sel->next_select()) {
        if (!can_pushdown_select(thd, sel, /*sel_unit=*/nullptr)) {
            count_miss();
            return nullptr;
        }
    }
    std::string sql, why;
    if (!stoolap_sqlgen::gen_unit(thd, unit, &sql, &why)) {
        explain_refusal(thd, why);
        count_miss();
        return nullptr;
    }
    PushdownPacked packed;
    if (!run_pushdown(thd, sql, &packed, nullptr)) {
        count_miss();
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    return new ha_stoolap_derived_handler(thd, std::move(sql),
                                          std::move(packed));
}

}  // namespace stoolap_pushdown
