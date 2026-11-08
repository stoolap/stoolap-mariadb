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
    // EXISTS/IN/scalar subqueries disqualifies the push.
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

bool item_uses_ci_string(Item* item);
bool field_not_as_written(const Field* f);
bool item_uses_decimal_field(Item* item);

// Forward decl: defined later. Walks every TABLE_LIST::on_expr (and
// the prep / sj variants) in a FROM chain plus every nested-join
// branch, returning true on the first predicate hit.
using ItemPred = bool (*)(Item*);
bool any_on_expr_matches_chain(TABLE_LIST* head, ItemPred pred);

}  // namespace

namespace stoolap_pushdown {

// Whether the row pump writes `sel`'s table through SQL (a table a
// FOREIGN KEY names), where it names a row by its primary key
bool rows_through_sql(THD* thd, SELECT_LEX* sel) {
    const TABLE_LIST* t = sel->get_table_list();
    if (!t || !t->table || !t->table->s) return false;
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    return db && stoolap_writes_through_sql(
                     db, stoolap_mariadb::stoolap_table_from_path(
                             t->table->s->normalized_path.str)
                             .c_str()) == 1;
}

bool can_direct_modify(THD* thd, const Item* where) {
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

    // True if `root` references a ci-string or DECIMAL field. Both
    // would miscompare against stoolap's bytewise/TEXT storage.
    auto items_unsafe_for_pushdown = [&](Item* root) {
        if (!root) return false;
        const bool ci_unsafe =
            !stoolap_thd_trust_binary_strings(thd) && item_uses_ci_string(root);
        // DECIMAL bail is unconditional (type mismatch, not collation).
        const bool decimal_unsafe = item_uses_decimal_field(root);
        return ci_unsafe || decimal_unsafe;
    };

    if (items_unsafe_for_pushdown(sel->where)) return false;
    // Single-table UPDATE may still carry a semi-join-lifted on_expr.
    if (any_on_expr_matches_chain(sel->get_table_list(), item_uses_ci_string) &&
        !stoolap_thd_trust_binary_strings(thd))
        return false;
    if (any_on_expr_matches_chain(sel->get_table_list(),
                                  item_uses_decimal_field))
        return false;

    if (cmd == SQLCOM_UPDATE) {
        // SET RHS expressions live in lex->value_list. A scalar subquery
        // like SET n = (SELECT COUNT(*) FROM o WHERE status='completed')
        // would route to stoolap and bytewise-compare against 'COMPLETED'.
        List_iterator<Item> it(thd->lex->value_list);
        Item* expr;
        while ((expr = it++)) {
            if (items_unsafe_for_pushdown(expr)) return false;
        }
        // DECIMAL targets: stoolap stores DECIMAL as TEXT. Assignments
        // pass a numeric param that rounds through float before write,
        // dropping scale. Encoded targets would store the value as
        // written. The row-pump path is faithful: it writes through
        // Field::store and extract_field.
        List_iterator<Item> tit(sel->item_list);
        Item* tgt;
        while ((tgt = tit++)) {
            if (!tgt) continue;
            if (tgt->type() == Item::FIELD_ITEM) {
                auto* fi = static_cast<Item_field*>(tgt);
                if (fi->field && field_not_as_written(fi->field)) {
                    return false;
                }
            }
        }
    }

    bool any = false;
    if (!every_table_is_stoolap(sel, any)) return false;
    if (!any) return false;

    // IGNORE skips a row's error; stoolap's DELETE would fail the whole
    // statement (MariaDB never pushes an UPDATE IGNORE).
    if (cmd == SQLCOM_DELETE && thd->lex->ignore) return false;

    // The statement from its Item tree, the condition MariaDB pushed.
    std::string sql;
    return stoolap_sqlgen::gen_modify(thd, sel, const_cast<Item*>(where), &sql,
                                      nullptr, nullptr, nullptr,
                                      rows_through_sql(thd, sel));
}

// The one INTEGER `sql` reads (a COUNT), or false
bool count_rows(StoolapDB* db, StoolapTx* tx, const std::string& sql,
                uint64_t* n) {
    const uint8_t* buf = nullptr;
    int64_t len = 0;
    if (stoolap_query_params_all(db, tx, sql.c_str(), nullptr, 0, &buf, &len) !=
            STOOLAP_OK ||
        !buf)
        return false;
    size_t pos = 0;
    uint32_t cols = 0;
    if (!stoolap_mariadb::pkt_parse_header(buf, static_cast<size_t>(len), &pos,
                                           &cols) ||
        cols != 1 || pos + 4 + 1 + 8 > static_cast<size_t>(len) ||
        stoolap_mariadb::pkt_le32(buf + pos) != 1 ||
        buf[pos + 4] != STOOLAP_TYPE_INTEGER)
        return false;
    const int64_t v = stoolap_mariadb::pkt_i64(buf + pos + 5);
    *n = v > 0 ? static_cast<uint64_t>(v) : 0;
    return true;
}

int try_direct_modify(THD* thd, const Item* where, ha_rows* affected,
                      unsigned* errkey_out, ha_rows* found) {
    if (!can_direct_modify(thd, where)) return HA_ERR_WRONG_COMMAND;

    std::string sql;
    stoolap_sqlgen::ModifyGuard guard;
    stoolap_sqlgen::ModifyCount count;
    SELECT_LEX* sel = thd->lex->first_select_lex();
    if (!stoolap_sqlgen::gen_modify(thd, sel, const_cast<Item*>(where), &sql,
                                    nullptr, &guard, &count,
                                    rows_through_sql(thd, sel)))
        return HA_ERR_WRONG_COMMAND;

    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db) return HA_ERR_INTERNAL_ERROR;

    // The rows found, when the UPDATE counts only the rows it changes: read
    // before it, since it may change what the condition takes. Reported
    // only ("Rows matched"), so a failed count leaves the changed count.
    // In autocommit it is a statement of its own, so a commit between it
    // and the UPDATE can make it off; stoolap#936 would count both in the
    // UPDATE.
    StoolapTx* tx = ctx->has_tx() ? ctx->tx() : nullptr;
    uint64_t found_n = 0;
    bool have_found = false;
    if (!count.sql.empty())
        have_found = count_rows(db, tx, count.sql, &found_n);

    // A prepared statement's parameters are literals in the generated
    // SQL (their bound values), as in a pushed SELECT.
    int64_t rows_aff = 0;
    const int rc = ctx->has_tx()
                       ? stoolap_tx_exec(ctx->tx(), sql.c_str(), &rows_aff)
                       : stoolap_exec(db, sql.c_str(), &rows_aff);
    if (rc != STOOLAP_OK) {
        StoolapErrorView verr =
            ctx->has_tx() ? fetch_tx_error(ctx->tx()) : fetch_db_error(db);
        const char* msg = *verr.message ? verr.message : "unknown error";
        // A row's SET value that does not fit its column (ModifyGuard):
        // MariaDB's strict-mode error. The row is not known: row 1.
        if (const auto* col = guard.overflowed(msg)) {
            my_error(col->error, MYF(0), col->name.c_str(), 1L);
            return HA_ERR_GENERIC;
        }
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
    if (found)
        *found = have_found ? static_cast<ha_rows>(found_n)
                            : static_cast<ha_rows>(rows_aff);
    stoolap_mariadb::g_stats.direct_modify_hits.fetch_add(
        1, std::memory_order_relaxed);
    return 0;
}

}  // namespace stoolap_pushdown

// ===========================================================================
// Whole-SELECT pushdown
// ===========================================================================

namespace {

// True if `item` references a ci-string field in a compare-sensitive
// position. Recurses into Item_func/sum/cond/ref/subselect; subquery
// recursion is required so EXISTS(...WHERE s='completed'...) doesn't
// byte-match 'COMPLETED' under MariaDB's ci semantics.
bool item_uses_ci_string(Item* item);
bool order_list_uses_ci_string(SQL_I_List<ORDER>& lst);
bool select_lex_pushdown_uses_ci(SELECT_LEX* sel, bool include_projection);

// Walk every ON-expression (on_expr, prep_on_expr, sj_on_expr) reachable
// from `sel`'s FROM list, recursing into nested joins. Used by both ci
// and DECIMAL guards: a JOIN ON predicate over those types miscompares.
bool any_on_expr_matches(List<TABLE_LIST>* join_list, ItemPred pred);

bool any_on_expr_matches_chain(TABLE_LIST* head, ItemPred pred) {
    for (TABLE_LIST* tl = head; tl; tl = tl->next_local) {
        if (tl->on_expr && pred(tl->on_expr)) return true;
        if (tl->prep_on_expr && pred(tl->prep_on_expr)) return true;
        if (tl->sj_on_expr && pred(tl->sj_on_expr)) return true;
        if (tl->nested_join &&
            any_on_expr_matches(&tl->nested_join->join_list, pred)) {
            return true;
        }
    }
    return false;
}

bool any_on_expr_matches(List<TABLE_LIST>* join_list, ItemPred pred) {
    if (!join_list) return false;
    List_iterator<TABLE_LIST> it(*join_list);
    TABLE_LIST* tl;
    while ((tl = it++)) {
        if (tl->on_expr && pred(tl->on_expr)) return true;
        if (tl->prep_on_expr && pred(tl->prep_on_expr)) return true;
        if (tl->sj_on_expr && pred(tl->sj_on_expr)) return true;
        if (tl->nested_join &&
            any_on_expr_matches(&tl->nested_join->join_list, pred)) {
            return true;
        }
    }
    return false;
}

// True when a multi-leg unit would compare ci-string projections
// byte-wise on stoolap: UNION DISTINCT / INTERSECT / EXCEPT dedup,
// tail-level ORDER/GROUP, or any per-leg compare-sensitive clause.
bool unit_pushdown_uses_ci(SELECT_LEX_UNIT* unit) {
    if (!unit) return false;
    SELECT_LEX* first = unit->first_select();
    if (!first) return false;
    const bool multi_leg = (first->next_select() != nullptr);

    for (SELECT_LEX* sel = first; sel; sel = sel->next_select()) {
        if (select_lex_pushdown_uses_ci(sel, /*include_projection=*/true))
            return true;
    }
    if (!multi_leg) return false;

    const bool dedups_at_unit = (unit->union_distinct != nullptr) || [&]() {
        for (SELECT_LEX* sel = first; sel; sel = sel->next_select()) {
            const enum sub_select_type lk = sel->get_linkage();
            if (lk == INTERSECT_TYPE || lk == EXCEPT_TYPE) return true;
        }
        return false;
    }();

    SELECT_LEX* gp = unit->global_parameters();
    const bool tail_uses_ci =
        gp && (order_list_uses_ci_string(gp->order_list) ||
               order_list_uses_ci_string(gp->group_list));

    if (!dedups_at_unit && !tail_uses_ci) return false;

    // Some leg projects something that the dedup / tail compare will see.
    // Bare FIELD_ITEM is the case the per-leg projection walker
    // intentionally skipped; check it here.
    for (SELECT_LEX* sel = first; sel; sel = sel->next_select()) {
        List_iterator<Item> it(sel->item_list);
        Item* pi;
        while ((pi = it++)) {
            if (pi && item_uses_ci_string(pi)) return true;
        }
    }
    return false;
}

// ci-string check for whole-query pushdown. Recurses into derived
// TABLE_LISTs so their inner WHERE/HAVING/etc. are inspected too.
// `include_projection` is false for EXISTS bodies (their projection
// is semantically ignored).
bool select_lex_pushdown_uses_ci(SELECT_LEX* sel,
                                 bool include_projection = true) {
    if (!sel) return false;
    // Compare-sensitive clauses of THIS select.
    if (sel->where && item_uses_ci_string(sel->where)) return true;
    if (sel->having && item_uses_ci_string(sel->having)) return true;
    if (order_list_uses_ci_string(sel->order_list)) return true;
    if (order_list_uses_ci_string(sel->group_list)) return true;
    // JOIN ON compares like WHERE does in the engine.
    if (any_on_expr_matches_chain(sel->get_table_list(), item_uses_ci_string)) {
        return true;
    }
    if (include_projection) {
        // Bare field refs and Item_ref-to-bare-field are byte-safe
        // (bytes flow to the client unchanged). EXCEPT under SELECT
        // DISTINCT, which dedups projected bytes positionally and
        // would byte-split 'COMPLETED' from 'completed'.
        const bool distinct_dedups = (sel->options & SELECT_DISTINCT) != 0;
        auto strip_to_bare_field = [](Item* it) -> Item* {
            for (int hops = 0; it && hops < 8; ++hops) {
                if (it->type() == Item::FIELD_ITEM) return it;
                if (it->type() != Item::REF_ITEM) return nullptr;
                auto* r = static_cast<Item_ref*>(it);
                if (!r->ref || !*r->ref) return nullptr;
                it = *r->ref;
            }
            return nullptr;
        };
        List_iterator<Item> it(sel->item_list);
        Item* pi;
        while ((pi = it++)) {
            if (!pi) continue;
            if (Item* bare = strip_to_bare_field(pi)) {
                if (distinct_dedups && item_uses_ci_string(bare)) return true;
                continue;
            }
            if (item_uses_ci_string(pi)) return true;
        }
    }
    // Derived clauses run inside the pushed plan; same protection.
    for (TABLE_LIST* tl = sel->get_table_list(); tl; tl = tl->next_local) {
        if (!tl->derived) continue;
        // Multi-leg derived units inherit set-op semantics (dedup +
        // tail ORDER compare projection bytes); use unit-level check.
        if (tl->derived->first_select() &&
            tl->derived->first_select()->next_select()) {
            if (unit_pushdown_uses_ci(tl->derived)) return true;
        } else {
            for (SELECT_LEX* inner = tl->derived->first_select(); inner;
                 inner = inner->next_select()) {
                if (select_lex_pushdown_uses_ci(inner, include_projection))
                    return true;
            }
        }
    }
    return false;
}

bool item_uses_ci_string(Item* item) {
    if (!item) return false;
    switch (item->type()) {
        case Item::FIELD_ITEM: {
            auto* fi = static_cast<Item_field*>(item);
            Field* f = fi->field;
            if (!f || !f->has_charset()) return false;
            CHARSET_INFO* cs = f->charset();
            return cs && !(cs->state & MY_CS_BINSORT);
        }
        case Item::FUNC_ITEM:
        case Item::SUM_FUNC_ITEM: {
            auto* fs = static_cast<Item_func_or_sum*>(item);
            const uint n = fs->argument_count();
            for (uint i = 0; i < n; ++i) {
                if (item_uses_ci_string(fs->arguments()[i])) return true;
            }
            return false;
        }
        case Item::COND_ITEM: {
            auto* cond = static_cast<Item_cond*>(item);
            List_iterator<Item> it(*cond->argument_list());
            Item* sub;
            while ((sub = it++)) {
                if (item_uses_ci_string(sub)) return true;
            }
            return false;
        }
        case Item::REF_ITEM: {
            auto* ref = static_cast<Item_ref*>(item);
            if (ref->ref && *ref->ref) {
                return item_uses_ci_string(*ref->ref);
            }
            return false;
        }
        case Item::SUBSELECT_ITEM: {
            auto* ss = static_cast<Item_subselect*>(item);
            SELECT_LEX_UNIT* u = ss->unit;
            if (!u) return false;
            // EXISTS ignores its projection; IN/scalar/ALL/ANY consume it
            // and need full inspection including bare ci fields.
            const bool exists_only =
                ss->substype() == Item_subselect::EXISTS_SUBS;
            for (SELECT_LEX* sel = u->first_select(); sel;
                 sel = sel->next_select()) {
                if (select_lex_pushdown_uses_ci(
                        sel, /*include_projection=*/!exists_only))
                    return true;
                if (exists_only) continue;
                // Stricter than outer projection: an outer bytewise
                // compare on the consumed result needs us to bail even
                // on bare ci fields here.
                List_iterator<Item> it(sel->item_list);
                Item* proj;
                while ((proj = it++)) {
                    if (proj && proj->type() == Item::FIELD_ITEM &&
                        item_uses_ci_string(proj)) {
                        return true;
                    }
                }
            }
            return false;
        }
        default:
            return false;
    }
}

// True if `item` references a column stoolap does not hold as MariaDB's
// own value: DECIMAL (TEXT, so '1.50' misses 1.50) and every encoded
// type (stoolap_types.h: dates and times, ENUM / SET, binary and
// non-utf8 strings), which statement text would compare as written.
// Bare projection is safe; not gated by trust_binary_strings (type
// mismatch, not collation).
bool item_uses_decimal_field(Item* item);
bool select_lex_uses_decimal_field(SELECT_LEX* sel, bool include_projection);

bool field_not_as_written(const Field* f) {
    return f->real_type() == MYSQL_TYPE_NEWDECIMAL ||
           stoolap_mariadb::field_enc(f) != stoolap_mariadb::Enc::PLAIN;
}

bool item_uses_decimal_field(Item* item) {
    if (!item) return false;
    switch (item->type()) {
        case Item::FIELD_ITEM: {
            auto* fi = static_cast<Item_field*>(item);
            Field* f = fi->field;
            return f && field_not_as_written(f);
        }
        case Item::FUNC_ITEM:
        case Item::SUM_FUNC_ITEM: {
            auto* fs = static_cast<Item_func_or_sum*>(item);
            const uint n = fs->argument_count();
            for (uint i = 0; i < n; ++i) {
                if (item_uses_decimal_field(fs->arguments()[i])) return true;
            }
            return false;
        }
        case Item::COND_ITEM: {
            auto* cond = static_cast<Item_cond*>(item);
            List_iterator<Item> it(*cond->argument_list());
            Item* sub;
            while ((sub = it++)) {
                if (item_uses_decimal_field(sub)) return true;
            }
            return false;
        }
        case Item::REF_ITEM: {
            auto* ref = static_cast<Item_ref*>(item);
            if (ref->ref && *ref->ref) {
                return item_uses_decimal_field(*ref->ref);
            }
            return false;
        }
        case Item::SUBSELECT_ITEM: {
            auto* ss = static_cast<Item_subselect*>(item);
            SELECT_LEX_UNIT* u = ss->unit;
            if (!u) return false;
            const bool exists_only =
                ss->substype() == Item_subselect::EXISTS_SUBS;
            for (SELECT_LEX* sel = u->first_select(); sel;
                 sel = sel->next_select()) {
                if (select_lex_uses_decimal_field(
                        sel, /*include_projection=*/!exists_only))
                    return true;
                if (exists_only) continue;
                // Stricter than outer pushdown: scalar / IN / ALL / ANY
                // subqueries return rows that the outer compares against.
                // A bare DECIMAL field projected from those would be
                // compared bytewise (stoolap-side) against a numeric
                // literal at the outer level.
                List_iterator<Item> it(sel->item_list);
                Item* proj;
                while ((proj = it++)) {
                    if (proj && proj->type() == Item::FIELD_ITEM &&
                        item_uses_decimal_field(proj)) {
                        return true;
                    }
                }
            }
            return false;
        }
        default:
            return false;
    }
}

bool order_list_uses_decimal(SQL_I_List<ORDER>& lst) {
    for (ORDER* o = lst.first; o; o = o->next) {
        if (o->item && *o->item && item_uses_decimal_field(*o->item))
            return true;
    }
    return false;
}

bool select_lex_uses_decimal_field(SELECT_LEX* sel,
                                   bool include_projection = true) {
    if (!sel) return false;
    if (sel->where && item_uses_decimal_field(sel->where)) return true;
    if (sel->having && item_uses_decimal_field(sel->having)) return true;
    if (order_list_uses_decimal(sel->order_list)) return true;
    if (order_list_uses_decimal(sel->group_list)) return true;
    // JOIN ON predicates: stoolap stores DECIMAL as TEXT, so a JOIN
    // ON DECIMAL compare is the same hazard as WHERE.
    if (any_on_expr_matches_chain(sel->get_table_list(),
                                  item_uses_decimal_field)) {
        return true;
    }
    if (include_projection) {
        List_iterator<Item> it(sel->item_list);
        Item* pi;
        while ((pi = it++)) {
            if (!pi) continue;
            if (pi->type() == Item::FIELD_ITEM) continue;
            if (item_uses_decimal_field(pi)) return true;
        }
    }
    for (TABLE_LIST* tl = sel->get_table_list(); tl; tl = tl->next_local) {
        if (!tl->derived) continue;
        for (SELECT_LEX* inner = tl->derived->first_select(); inner;
             inner = inner->next_select()) {
            if (select_lex_uses_decimal_field(inner, include_projection))
                return true;
        }
    }
    return false;
}

bool order_list_uses_ci_string(SQL_I_List<ORDER>& lst) {
    for (ORDER* o = lst.first; o; o = o->next) {
        if (o->item && *o->item && item_uses_ci_string(*o->item)) return true;
    }
    return false;
}

// false -> factory returns NULL -> MariaDB falls back to row pump.
// semantic_guards: the ci / DECIMAL / placeholder checks the text-based
// unit and derived paths rely on. The single-SELECT path passes false:
// stoolap_sqlgen makes those decisions per expression.
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
 * The rows of a pushed query. The factory takes them all at once
 * (stoolap_rows_into_cursor), so a runtime error still falls back to
 * MariaDB; next_row then has the cursor write each into the handler's
 * table record, result column c into table->field[c].
 */
struct PushdownRows {
    stoolap_mariadb::CursorPtr cursor;
    bool described = false;

    bool take(stoolap_mariadb::RowsPtr rows) {
        if (!rows) return false;
        cursor.reset(stoolap_cursor_new());
        if (!cursor ||
            stoolap_rows_into_cursor(rows.get(), cursor.get()) != STOOLAP_OK) {
            cursor.reset();
            return false;
        }
        return true;
    }

    // The next row into table->record[0].
    int next(TABLE* table, const char* what) {
        if (!cursor) return HA_ERR_END_OF_FILE;
        if (!described) {
            std::vector<StoolapField> fields;
            fields.reserve(table->s->fields);
            for (uint c = 0; c < table->s->fields; ++c)
                fields.push_back(stoolap_mariadb::record_field(
                    table, c, static_cast<int32_t>(c)));
            if (stoolap_cursor_set_fields(
                    cursor.get(), fields.data(),
                    static_cast<int32_t>(fields.size())) != STOOLAP_OK)
                return fail(what);
            described = true;
        }
        stoolap_mariadb::RecordTarget target{table, 0};
        int64_t id = 0;
        int32_t last = 0;
        const int rc = stoolap_cursor_next(cursor.get(), table->record[0],
                                           &stoolap_mariadb::store_cell,
                                           &target, &id, &last);
        if (rc == STOOLAP_ROW) return 0;
        if (rc == STOOLAP_DONE) return HA_ERR_END_OF_FILE;
        return target.err ? target.err : fail(what);
    }

    int fail(const char* what) {
        my_printf_error(ER_GET_ERRMSG, "stoolap %s: %s", MYF(0), what,
                        stoolap_cursor_errmsg(cursor.get()));
        cursor.reset();
        return HA_ERR_GENERIC;
    }
};

class ha_stoolap_select_handler : public select_handler {
public:
    ha_stoolap_select_handler(THD* thd_arg, SELECT_LEX* sel, std::string sql,
                              PushdownRows rows)
        : select_handler(thd_arg, stoolap_hton, sel),
          sql_(std::move(sql)),
          rows_(std::move(rows)) {}

    // Unit overload: whole UNION/EXCEPT/INTERSECT pushdown.
    ha_stoolap_select_handler(THD* thd_arg, SELECT_LEX_UNIT* unit,
                              std::string sql, PushdownRows rows)
        : select_handler(thd_arg, stoolap_hton, unit),
          sql_(std::move(sql)),
          rows_(std::move(rows)) {}

    ~ha_stoolap_select_handler() override = default;

protected:
    int init_scan() override {
        const bool trace = perf_trace_on();
        const uint64_t t0 = trace ? now_ns() : 0;
        // The factory ran the query and took its rows.
        const int rc = rows_.cursor ? 0 : HA_ERR_INTERNAL_ERROR;
        if (trace) {
            stoolap_mariadb::g_stats.perf_init_scan_ns.fetch_add(
                now_ns() - t0, std::memory_order_relaxed);
        }
        return rc;
    }

    int next_row() override {
        const bool trace = perf_trace_on();
        const uint64_t t0 = trace ? now_ns() : 0;
        const int err = rows_.next(table, "pushdown");
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
        rows_.cursor.reset();
        if (trace) {
            stoolap_mariadb::g_stats.perf_end_scan_ns.fetch_add(
                now_ns() - t0, std::memory_order_relaxed);
        }
        return 0;
    }

private:
    std::string sql_;
    PushdownRows rows_;
};

}  // namespace

namespace {

void count_miss() {
    stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
        1, std::memory_order_relaxed);
}

// Runs generated pushdown SQL eagerly and takes its rows. Eager, so any
// stoolap-side error (unknown function, type mismatch, ...) still falls
// back to MariaDB: once a factory returns a handler, MariaDB is committed
// to it. stoolap_prepare catches only syntax; semantic errors surface at
// execute time only.
bool run_pushdown(THD* thd, const std::string& sql, PushdownRows* rows,
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
    return rows->take(stoolap_mariadb::RowsPtr(raw));
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
    PushdownRows rows;
    uint64_t eager_ns = 0;
    if (!run_pushdown(thd, sql, &rows, trace ? &eager_ns : nullptr)) {
        count_miss();
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    auto* h = new ha_stoolap_select_handler(thd, sel_lex, std::move(sql),
                                            std::move(rows));
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
    PushdownRows rows;
    if (!run_pushdown(thd, sql, &rows, nullptr)) {
        count_miss();
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    return new ha_stoolap_select_handler(thd, unit, std::move(sql),
                                         std::move(rows));
}

}  // namespace stoolap_pushdown

// ===========================================================================
// Derived-table pushdown
// ===========================================================================

namespace {

class ha_stoolap_derived_handler : public derived_handler {
public:
    ha_stoolap_derived_handler(THD* thd_arg, std::string sql, PushdownRows rows)
        : derived_handler(thd_arg, stoolap_hton),
          sql_(std::move(sql)),
          rows_(std::move(rows)) {}

    ~ha_stoolap_derived_handler() override = default;

    int init_scan() override {
        return rows_.cursor ? 0 : HA_ERR_INTERNAL_ERROR;
    }

    int next_row() override { return rows_.next(table, "derived"); }

    int end_scan() override {
        rows_.cursor.reset();
        return 0;
    }

private:
    std::string sql_;
    PushdownRows rows_;
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
    PushdownRows rows;
    if (!run_pushdown(thd, sql, &rows, nullptr)) {
        count_miss();
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    return new ha_stoolap_derived_handler(thd, std::move(sql), std::move(rows));
}

bool item_safe_for_byte_comparison(Item* item) {
    return !item_uses_ci_string(item);
}

}  // namespace stoolap_pushdown
