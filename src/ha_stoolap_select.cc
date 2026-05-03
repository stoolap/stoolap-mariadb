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

// Defined in ha_stoolap.cc. Pointers in `details` are valid only until
// the next FFI call on the originating handle.
struct StoolapErrorView {
    int32_t code;
    StoolapErrorDetails details;
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

/** Stoolap-side flat name for a (db, tbl) pair. Mirrors
 *  ha_stoolap.cc::stoolap_table_from_path: escapes `_` -> `_0` and
 *  emits `_1` between db and tbl, so distinct (db, tbl) pairs never
 *  flatten to the same backing name. CREATE-time names produced by
 *  the path mapper and DML-time names produced here must agree
 *  byte-for-byte; tests verify both end up addressing the same
 *  stoolap table. */
std::string stoolap_name_for(const char* db, const char* tbl) {
    auto append_escaped = [](std::string& out, const char* s) {
        if (!s) return;
        for (const char* p = s; *p; ++p) {
            if (*p == '_')
                out.append("_0");
            else if (*p == '/')
                out.append("_1");
            else
                out.push_back(*p);
        }
    };
    std::string out;
    if (db && *db) {
        append_escaped(out, db);
        out.append("_1");
    }
    append_escaped(out, tbl);
    return out;
}

struct LeafName {
    std::string db;
    std::string tbl;
    std::string stoolap;
};

void collect_leaf_names(SELECT_LEX* sel_lex, std::vector<LeafName>& out) {
    if (!sel_lex) return;
    for (TABLE_LIST* tl = sel_lex->get_table_list(); tl; tl = tl->next_local) {
        if (tl->derived) {
            // Recurse into the derived's inner SELECTs so the rewriter
            // sees every base stoolap table that appears in the SQL
            // text -- including the ones nested inside derived tables
            // when the whole query pushes as a single plan.
            for (SELECT_LEX* inner = tl->derived->first_select(); inner;
                 inner = inner->next_select()) {
                collect_leaf_names(inner, out);
            }
            continue;
        }
        if (!tl->table) continue;
        if (!tl->table->file || tl->table->file->ht != stoolap_hton) continue;
        const char* db = tl->db.str ? tl->db.str : "";
        const char* tbl = tl->table_name.str ? tl->table_name.str : "";
        out.push_back(
            {std::string(db), std::string(tbl), stoolap_name_for(db, tbl)});
    }
    // Subqueries: walk inner units. Without this the rewriter misses
    // tables referenced inside EXISTS/IN/scalar subqueries, and the
    // SQL we hand stoolap leaves those names un-rewritten -- which
    // surfaces as "table or view 'X' not found" from the engine.
    for (SELECT_LEX_UNIT* u = sel_lex->first_inner_unit(); u;
         u = u->next_unit()) {
        for (SELECT_LEX* sub = u->first_select(); sub;
             sub = sub->next_select()) {
            collect_leaf_names(sub, out);
        }
    }
}

bool is_ident_char(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$';
}

/** Skip past a SQL string / quoted-identifier literal starting at `start`,
 *  returning the index one past the closing delimiter. Handles doubled-
 *  delimiter escapes and C-style backslash escapes. */
size_t skip_string_literal(const std::string& s, size_t start) {
    const char delim = s[start];
    size_t i = start + 1;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            i += 2;
            continue;
        }
        if (c == delim) {
            if (i + 1 < s.size() && s[i + 1] == delim) {
                i += 2;
                continue;
            }
            return i + 1;
        }
        ++i;
    }
    return s.size();
}

/** Word-boundary identifier replacement that skips literal regions. The
 *  `.` exclusion on the left side prevents mangling the table half of a
 *  qualified `db.tbl` reference when iterating a different leaf. */
void replace_ident_word(std::string& s, std::string_view from,
                        std::string_view to) {
    if (from.empty()) return;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\'' || c == '"' || c == '`') {
            i = skip_string_literal(s, i);
            continue;
        }
        if (c != from[0]) {
            ++i;
            continue;
        }
        if (s.compare(i, from.size(), from) != 0) {
            ++i;
            continue;
        }

        const bool ok_left =
            (i == 0) || (!is_ident_char(static_cast<unsigned char>(s[i - 1])) &&
                         s[i - 1] != '.');
        const size_t end = i + from.size();
        const bool ok_right =
            (end >= s.size()) ||
            !is_ident_char(static_cast<unsigned char>(s[end]));
        if (ok_left && ok_right) {
            s.replace(i, from.size(), to);
            i += to.size();
        } else {
            i += from.size();
        }
    }
}

/** Replace every occurrence of `from` with `to`, skipping single-quoted,
 *  double-quoted and backticked string / identifier literals so a
 *  backticked table-name pattern (e.g. "`t`") doesn't mangle a SQL
 *  literal that happens to contain the same byte sequence (e.g.
 *  `UPDATE t SET note='use `t` here'`).
 *
 *  Note: backticked literals (`...`) are MySQL-style quoted identifiers
 *  rather than string literals, but treating them as opaque regions is
 *  fine here. A backticked table reference like `t` IS itself such a
 *  region, so any nested occurrence we skip past was already part of
 *  an identifier we'd not want to rewrite. */
void replace_all_literal_aware(std::string& s, std::string_view from,
                               std::string_view to) {
    if (from.empty()) return;
    const char first = from[0];
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\'' || c == '"') {
            // Inside a string literal -- skip whole literal, then keep
            // scanning. Without this guard, replace_all would mangle
            // SQL string contents (silent data corruption on direct DML).
            i = skip_string_literal(s, i);
            continue;
        }
        // A backticked identifier we encounter mid-scan that happens NOT
        // to match `from` (e.g. a different table's backticked name) is
        // an opaque region too: skip it so we don't half-match into its
        // interior. The caller has already issued targeted replacements
        // for known backticked forms before invoking this helper.
        if (c == '`' && first != '`') {
            i = skip_string_literal(s, i);
            continue;
        }
        if (c != first || s.compare(i, from.size(), from) != 0) {
            ++i;
            continue;
        }
        s.replace(i, from.size(), to);
        i += to.size();
    }
}

/**
 * Rewrite a MariaDB-flavoured UPDATE/DELETE so every reference to a stoolap
 * base table picks up the stoolap-flat name. Covers every MariaDB qualified
 * identifier syntax:
 *
 *   `db`.`tbl`   ->   "db__tbl"
 *   db.tbl       ->   db__tbl
 *   `db`.tbl     ->   "db__tbl"   (mixed: quoted db, bare tbl)
 *   db.`tbl`     ->   "db__tbl"   (mixed: bare db, quoted tbl)
 *   `tbl`        ->   "db__tbl"   (only when the bare name is unambiguous)
 *   tbl          ->   db__tbl     (same condition; word-boundary aware)
 *
 * Phase order matters: every qualified form runs first so a later bare pass
 * can't consume the table half of a qualified ref. The two mixed-quote
 * variants are common enough -- MariaDB users sometimes quote one half but
 * not the other -- that omitting them dropped direct-DML to a stoolap parse
 * error like "expected SET after <db>, got '.'".
 */
std::string rewrite_table_names(const std::string& sql,
                                const std::vector<LeafName>& leaves,
                                const char* current_db) {
    if (leaves.empty()) return sql;
    std::string out = sql;
    const std::string cur_db = current_db ? current_db : "";

    for (const LeafName& l : leaves) {
        const std::string quoted = std::string("\"") + l.stoolap + "\"";
        const std::string bt_qual = "`" + l.db + "`.`" + l.tbl + "`";
        const std::string bt_db_qual = "`" + l.db + "`." + l.tbl;
        const std::string bt_tb_qual = l.db + ".`" + l.tbl + "`";
        // Backticked-both first; the mixed forms are stricter substrings
        // of all-bare so they need their own pass before bare-bare runs.
        replace_all_literal_aware(out, bt_qual, quoted);
        replace_all_literal_aware(out, bt_db_qual, quoted);
        replace_all_literal_aware(out, bt_tb_qual, quoted);
        const std::string bare_qual = l.db + "." + l.tbl;
        replace_ident_word(out, bare_qual, l.stoolap);
    }

    // Bare-form rewrites only fire when the bare name is unambiguous
    // (single distinct database among all leaves with that table name)
    // and the leaf is in the current database.
    std::unordered_map<std::string, int> tbl_distinct_dbs;
    {
        std::unordered_map<std::string, std::unordered_map<std::string, int>>
            seen;
        for (const LeafName& l : leaves)
            seen[l.tbl][l.db]++;
        for (const auto& kv : seen)
            tbl_distinct_dbs[kv.first] = kv.second.size();
    }
    std::unordered_map<std::string, int> done;
    for (const LeafName& l : leaves) {
        if (tbl_distinct_dbs[l.tbl] != 1) continue;
        if (l.db != cur_db) continue;
        if (done[l.stoolap]++) continue;

        const std::string quoted = std::string("\"") + l.stoolap + "\"";
        const std::string bt_bare = "`" + l.tbl + "`";
        replace_all_literal_aware(out, bt_bare, quoted);
        replace_ident_word(out, l.tbl, l.stoolap);
    }
    return out;
}

// Forward decl: collect_bound_params is defined in the next anonymous
// namespace block alongside the SELECT pushdown helpers, but
// try_direct_modify (in stoolap_pushdown below) needs to call it. Multiple
// anonymous namespace blocks in one TU collapse to the same namespace,
// so a forward decl here resolves cleanly.
bool collect_bound_params(THD* thd, std::vector<StoolapValue>& out,
                          std::vector<std::string>& storage);
bool item_uses_ci_string(Item* item);
bool item_uses_decimal_field(Item* item);
bool item_contains_param(Item* item);

// Forward decl: defined later. Walks every TABLE_LIST::on_expr (and
// the prep / sj variants) in a FROM chain plus every nested-join
// branch, returning true on the first predicate hit.
using ItemPred = bool (*)(Item*);
bool any_on_expr_matches_chain(TABLE_LIST* head, ItemPred pred);

}  // namespace

namespace stoolap_pushdown {

// True if any leaf the rewriter touches has a column sharing its bare
// table name. The text-based rewriter's word-boundary replace would
// mangle that column token. Mirrors collect_leaf_names's walk.
bool any_leaf_has_shadow_column(SELECT_LEX* sel_lex);

void collect_leaf_table_names(SELECT_LEX* sel_lex,
                              std::vector<std::string>& names) {
    if (!sel_lex) return;
    for (TABLE_LIST* tl = sel_lex->get_table_list(); tl; tl = tl->next_local) {
        if (tl->derived) {
            for (SELECT_LEX* inner = tl->derived->first_select(); inner;
                 inner = inner->next_select()) {
                collect_leaf_table_names(inner, names);
            }
            continue;
        }
        if (tl->table_name.str && tl->table_name.length > 0) {
            names.emplace_back(tl->table_name.str, tl->table_name.length);
        }
    }
    for (SELECT_LEX_UNIT* u = sel_lex->first_inner_unit(); u;
         u = u->next_unit()) {
        for (SELECT_LEX* sub = u->first_select(); sub;
             sub = sub->next_select()) {
            collect_leaf_table_names(sub, names);
        }
    }
}

bool any_leaf_table_name_collides_with_any_column(
    SELECT_LEX* sel_lex, const std::vector<std::string>& all_names);

bool any_leaf_table_name_collides_with_any_column_one(
    TABLE_LIST* tl, const std::vector<std::string>& all_names) {
    if (!tl || !tl->table || !tl->table->s) return false;
    TABLE* t = tl->table;
    for (uint i = 0; i < t->s->fields; ++i) {
        Field* f = t->field[i];
        if (!f) continue;
        const size_t flen = f->field_name.length;
        const char* fstr = f->field_name.str;
        for (const std::string& tname : all_names) {
            if (tname.size() == flen &&
                strncasecmp(tname.c_str(), fstr, flen) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool any_leaf_table_name_collides_with_any_column(
    SELECT_LEX* sel_lex, const std::vector<std::string>& all_names) {
    if (!sel_lex) return false;
    for (TABLE_LIST* tl = sel_lex->get_table_list(); tl; tl = tl->next_local) {
        if (tl->derived) {
            for (SELECT_LEX* inner = tl->derived->first_select(); inner;
                 inner = inner->next_select()) {
                if (any_leaf_table_name_collides_with_any_column(inner,
                                                                 all_names))
                    return true;
            }
            continue;
        }
        if (any_leaf_table_name_collides_with_any_column_one(tl, all_names))
            return true;
    }
    for (SELECT_LEX_UNIT* u = sel_lex->first_inner_unit(); u;
         u = u->next_unit()) {
        for (SELECT_LEX* sub = u->first_select(); sub;
             sub = sub->next_select()) {
            if (any_leaf_table_name_collides_with_any_column(sub, all_names))
                return true;
        }
    }
    return false;
}

// Cross-leaf shadow: another leaf's table name can collide with this
// leaf's column. UPDATE u SET o=o+1 WHERE EXISTS(SELECT 1 FROM o ...)
// has outer leaf `u` with column `o` and subquery leaf `o` -- bare `o`
// in SET gets rewritten to table `o`'s flat form, mangling the column.
bool any_leaf_has_shadow_column(SELECT_LEX* sel_lex) {
    std::vector<std::string> all_names;
    collect_leaf_table_names(sel_lex, all_names);
    if (all_names.empty()) return false;
    return any_leaf_table_name_collides_with_any_column(sel_lex, all_names);
}

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
        // dropping scale. The row-pump path is faithful because
        // Field::store on the DECIMAL receives the value at full precision.
        List_iterator<Item> tit(sel->item_list);
        Item* tgt;
        while ((tgt = tit++)) {
            if (!tgt) continue;
            if (tgt->type() == Item::FIELD_ITEM) {
                auto* fi = static_cast<Item_field*>(tgt);
                if (fi->field &&
                    fi->field->real_type() == MYSQL_TYPE_NEWDECIMAL) {
                    return false;
                }
            }
        }
    }

    bool any = false;
    if (!every_table_is_stoolap(sel, any)) return false;
    if (!any) return false;

    // Decline if any leaf has a shadow column; the text-based rewriter
    // would mangle SET/WHERE tokens that share a table name. Row-pump
    // handles those via per-row callbacks.
    if (any_leaf_has_shadow_column(sel)) return false;
    return true;
}

int try_direct_modify(THD* thd, ha_rows* affected, unsigned* errkey_out) {
    if (!can_direct_modify(thd)) return HA_ERR_WRONG_COMMAND;

    SELECT_LEX* sel = thd->lex->first_select_lex();
    std::string raw(thd->query(), thd->query_length());
    if (raw.empty()) return HA_ERR_WRONG_COMMAND;

    std::vector<LeafName> leaves;
    collect_leaf_names(sel, leaves);
    std::string sql = rewrite_table_names(raw, leaves, thd->db.str);

    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db) return HA_ERR_INTERNAL_ERROR;

    // Bind any prepared-statement parameters: server-side EXECUTE of
    // UPDATE/DELETE keeps `?` placeholders in thd->query(), with the
    // bound values in LEX::param_list. Without this binding step
    // stoolap would see the placeholders as literal `?` tokens and
    // reject the statement (or worse, target the wrong rows on a
    // looser parser). Plain (non-prepared) DML has param_list empty
    // so this collapses to the no-params path.
    std::vector<StoolapValue> params;
    std::vector<std::string> param_storage;
    if (!collect_bound_params(thd, params, param_storage)) {
        return HA_ERR_WRONG_COMMAND;  // unsupported param type -> per-row fallback
    }

    int64_t rows_aff = 0;
    int rc;
    if (params.empty()) {
        rc = ctx->has_tx() ? stoolap_tx_exec(ctx->tx(), sql.c_str(), &rows_aff)
                           : stoolap_exec(db, sql.c_str(), &rows_aff);
    } else {
        const StoolapValue* pv = params.data();
        const int32_t pn = static_cast<int32_t>(params.size());
        rc = ctx->has_tx()
                 ? stoolap_tx_exec_params(ctx->tx(), sql.c_str(), pv, pn,
                                          &rows_aff)
                 : stoolap_exec_params(db, sql.c_str(), pv, pn, &rows_aff);
    }
    if (rc != STOOLAP_OK) {
        StoolapErrorView verr =
            ctx->has_tx() ? fetch_tx_error(ctx->tx()) : fetch_db_error(db);
        const char* msg =
            *verr.details.message ? verr.details.message : "unknown error";
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
            case HA_ERR_ROW_IS_REFERENCED:
                my_printf_error(ER_NO_REFERENCED_ROW_2, "stoolap: %s", MYF(0),
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

// True if `item` references a DECIMAL column. Stoolap stores DECIMAL
// as TEXT, so a pushed predicate against a numeric literal silently
// misses ('1.50' vs 1.50). Bare projection is safe; not gated by
// trust_binary_strings (type mismatch, not collation).
bool item_uses_decimal_field(Item* item);
bool select_lex_uses_decimal_field(SELECT_LEX* sel, bool include_projection);

bool item_uses_decimal_field(Item* item) {
    if (!item) return false;
    switch (item->type()) {
        case Item::FIELD_ITEM: {
            auto* fi = static_cast<Item_field*>(item);
            Field* f = fi->field;
            return f && f->real_type() == MYSQL_TYPE_NEWDECIMAL;
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

bool unit_uses_decimal_field(SELECT_LEX_UNIT* unit) {
    if (!unit) return false;
    for (SELECT_LEX* sel = unit->first_select(); sel;
         sel = sel->next_select()) {
        if (select_lex_uses_decimal_field(sel, /*include_projection=*/true))
            return true;
    }
    return false;
}

// dynamic_cast required: at EXECUTE time Item_param::type() returns
// CONST_ITEM (or NULL_ITEM), not PARAM_ITEM, so the tag check misses.
bool item_contains_param(Item* item) {
    if (!item) return false;
    if (dynamic_cast<Item_param*>(item)) return true;
    switch (item->type()) {
        case Item::FUNC_ITEM:
        case Item::SUM_FUNC_ITEM: {
            auto* fs = static_cast<Item_func_or_sum*>(item);
            const uint n = fs->argument_count();
            for (uint i = 0; i < n; ++i) {
                if (item_contains_param(fs->arguments()[i])) return true;
            }
            return false;
        }
        case Item::COND_ITEM: {
            auto* cond = static_cast<Item_cond*>(item);
            List_iterator<Item> it(*cond->argument_list());
            Item* sub;
            while ((sub = it++)) {
                if (item_contains_param(sub)) return true;
            }
            return false;
        }
        case Item::REF_ITEM: {
            auto* ref = static_cast<Item_ref*>(item);
            if (ref->ref && *ref->ref) return item_contains_param(*ref->ref);
            return false;
        }
        case Item::SUBSELECT_ITEM: {
            auto* ss = static_cast<Item_subselect*>(item);
            SELECT_LEX_UNIT* u = ss->unit;
            if (!u) return false;
            for (SELECT_LEX* sel = u->first_select(); sel;
                 sel = sel->next_select()) {
                if (sel->where && item_contains_param(sel->where)) return true;
                if (sel->having && item_contains_param(sel->having))
                    return true;
                List_iterator<Item> it(sel->item_list);
                Item* pi;
                while ((pi = it++)) {
                    if (item_contains_param(pi)) return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

// Counts Item_param nodes; mirrors item_contains_param's recursion.
size_t item_count_params(Item* item) {
    if (!item) return 0;
    if (dynamic_cast<Item_param*>(item)) return 1;
    switch (item->type()) {
        case Item::FUNC_ITEM:
        case Item::SUM_FUNC_ITEM: {
            auto* fs = static_cast<Item_func_or_sum*>(item);
            const uint n = fs->argument_count();
            size_t total = 0;
            for (uint i = 0; i < n; ++i) {
                total += item_count_params(fs->arguments()[i]);
            }
            return total;
        }
        case Item::COND_ITEM: {
            auto* cond = static_cast<Item_cond*>(item);
            List_iterator<Item> it(*cond->argument_list());
            Item* sub;
            size_t total = 0;
            while ((sub = it++))
                total += item_count_params(sub);
            return total;
        }
        case Item::REF_ITEM: {
            auto* ref = static_cast<Item_ref*>(item);
            if (ref->ref && *ref->ref) return item_count_params(*ref->ref);
            return 0;
        }
        case Item::SUBSELECT_ITEM: {
            auto* ss = static_cast<Item_subselect*>(item);
            SELECT_LEX_UNIT* u = ss->unit;
            if (!u) return 0;
            size_t total = 0;
            for (SELECT_LEX* sel = u->first_select(); sel;
                 sel = sel->next_select()) {
                if (sel->where) total += item_count_params(sel->where);
                if (sel->having) total += item_count_params(sel->having);
                List_iterator<Item> it(sel->item_list);
                Item* pi;
                while ((pi = it++))
                    total += item_count_params(pi);
            }
            return total;
        }
        default:
            return 0;
    }
}

// Count `?` in WHERE/HAVING (stoolap binds these). Recurses into
// derived tables and inner units.
size_t select_count_predicate_params(SELECT_LEX* sel) {
    if (!sel) return 0;
    size_t total = 0;
    if (sel->where) total += item_count_params(sel->where);
    if (sel->having) total += item_count_params(sel->having);
    for (TABLE_LIST* tl = sel->get_table_list(); tl; tl = tl->next_local) {
        if (!tl->derived) continue;
        for (SELECT_LEX* inner = tl->derived->first_select(); inner;
             inner = inner->next_select()) {
            total += select_count_predicate_params(inner);
        }
    }
    for (SELECT_LEX_UNIT* u = sel->first_inner_unit(); u; u = u->next_unit()) {
        for (SELECT_LEX* sub = u->first_select(); sub;
             sub = sub->next_select()) {
            total += select_count_predicate_params(sub);
        }
    }
    return total;
}

// True if this SELECT (or any nested derived/subquery) has `?` in a
// projection / ORDER BY / GROUP BY surface (stoolap returns NULL there
// instead of the bound value). WHERE/HAVING are NOT checked: stoolap
// binds those, and we depend on it.
//
// Per-leg only. The statement-wide count backstop is applied by the
// factory wrappers at the top scope; doing it per-leg would falsely
// bail on a prepared UNION whose params split across legs.
bool select_param_in_unsafe_surface(SELECT_LEX* sel) {
    if (!sel) return false;
    {
        List_iterator<Item> it(sel->item_list);
        Item* pi;
        while ((pi = it++)) {
            if (pi && item_contains_param(pi)) return true;
        }
    }
    for (ORDER* o = sel->order_list.first; o; o = o->next) {
        if (o->item && *o->item && item_contains_param(*o->item)) return true;
    }
    for (ORDER* o = sel->group_list.first; o; o = o->next) {
        if (o->item && *o->item && item_contains_param(*o->item)) return true;
    }
    // Derived projections become values in the outer plan; subqueries
    // live in inner units.
    for (TABLE_LIST* tl = sel->get_table_list(); tl; tl = tl->next_local) {
        if (!tl->derived) continue;
        for (SELECT_LEX* inner = tl->derived->first_select(); inner;
             inner = inner->next_select()) {
            if (select_param_in_unsafe_surface(inner)) return true;
        }
    }
    for (SELECT_LEX_UNIT* u = sel->first_inner_unit(); u; u = u->next_unit()) {
        for (SELECT_LEX* sub = u->first_select(); sub;
             sub = sub->next_select()) {
            if (select_param_in_unsafe_surface(sub)) return true;
        }
    }
    return false;
}

// Backstop: bail when accounted predicate `?` count < lex param_list
// total. The gap means a placeholder lives in an Item subclass our
// walker doesn't reach.
bool statement_param_backstop_unsafe(THD* thd, size_t predicate_param_count) {
    if (!thd || !thd->lex) return false;
    return thd->lex->param_list.elements > predicate_param_count;
}

bool unit_param_in_unsafe_surface(SELECT_LEX_UNIT* unit) {
    if (!unit) return false;
    for (SELECT_LEX* sel = unit->first_select(); sel;
         sel = sel->next_select()) {
        if (select_param_in_unsafe_surface(sel)) return true;
    }
    // global_parameters() carries unit-level ORDER/GROUP/LIMIT.
    SELECT_LEX* gp = unit->global_parameters();
    if (gp) {
        for (ORDER* o = gp->order_list.first; o; o = o->next) {
            if (o->item && *o->item && item_contains_param(*o->item))
                return true;
        }
        for (ORDER* o = gp->group_list.first; o; o = o->next) {
            if (o->item && *o->item && item_contains_param(*o->item))
                return true;
        }
    }
    return false;
}

// Predicate `?` total across every leg; pairs with the backstop.
size_t unit_count_predicate_params(SELECT_LEX_UNIT* unit) {
    if (!unit) return 0;
    size_t total = 0;
    for (SELECT_LEX* sel = unit->first_select(); sel;
         sel = sel->next_select()) {
        total += select_count_predicate_params(sel);
    }
    return total;
}

bool order_list_uses_ci_string(SQL_I_List<ORDER>& lst) {
    for (ORDER* o = lst.first; o; o = o->next) {
        if (o->item && *o->item && item_uses_ci_string(*o->item)) return true;
    }
    return false;
}

// Reuses the direct-DML identifier rewriter.
bool build_pushdown_select_sql(THD* thd, SELECT_LEX* sel_lex,
                               std::string& out_sql) {
    if (!thd || !sel_lex) return false;
    if (!thd->query() || thd->query_length() == 0) return false;

    std::string raw(thd->query(), thd->query_length());
    std::vector<LeafName> leaves;
    collect_leaf_names(sel_lex, leaves);
    if (leaves.empty()) return false;

    out_sql = rewrite_table_names(raw, leaves, thd->db.str);
    return !out_sql.empty();
}

// `storage` owns string copies so StoolapValue.text.ptr stays valid
// for the duration of the stoolap call. PREPARE phase is filtered out
// upstream (factory bails on is_stmt_prepare).
bool collect_bound_params(THD* thd, std::vector<StoolapValue>& out,
                          std::vector<std::string>& storage) {
    if (!thd || !thd->lex) return true;
    List_iterator<Item_param> it(thd->lex->param_list);
    Item_param* p;
    storage.reserve(thd->lex->param_list.elements);
    while ((p = it++)) {
        StoolapValue v{};
        if (p->null_value) {
            v.value_type = STOOLAP_TYPE_NULL;
            out.push_back(v);
            continue;
        }
        switch (p->result_type()) {
            case INT_RESULT:
                v.value_type = STOOLAP_TYPE_INTEGER;
                v.v.integer = p->val_int();
                break;
            case REAL_RESULT:
                v.value_type = STOOLAP_TYPE_FLOAT;
                v.v.float64 = p->val_real();
                break;
            case DECIMAL_RESULT: {
                // Bind DECIMAL as TEXT, never FLOAT: val_real() rounds
                // through double, losing low-order digits above 2^53.
                String tmp;
                String* s = p->val_str(&tmp);
                if (!s || p->null_value) {
                    v.value_type = STOOLAP_TYPE_NULL;
                    break;
                }
                storage.emplace_back(s->ptr(), s->length());
                const std::string& held = storage.back();
                v.value_type = STOOLAP_TYPE_TEXT;
                v.v.text.ptr = held.data();
                v.v.text.len = static_cast<int64_t>(held.size());
                break;
            }
            case STRING_RESULT: {
                String tmp;
                String* s = p->val_str(&tmp);
                if (!s || p->null_value) {
                    v.value_type = STOOLAP_TYPE_NULL;
                    break;
                }
                storage.emplace_back(s->ptr(), s->length());
                const std::string& held = storage.back();
                v.value_type = STOOLAP_TYPE_TEXT;
                v.v.text.ptr = held.data();
                v.v.text.len = static_cast<int64_t>(held.size());
                break;
            }
            case ROW_RESULT:
            case TIME_RESULT:
            default:
                return false;
        }
        out.push_back(v);
    }
    return true;
}

// false -> factory returns NULL -> MariaDB falls back to row pump.
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

    // Shadow-column hazard: rewrite_table_names is text-based, so any
    // leaf table whose schema has a column with the same name as the
    // table itself ends up with `SELECT u FROM u` becoming
    // `SELECT db__u FROM db__u` -- a column rename that stoolap may
    // accept silently with wrong rows. Direct DML already gates on
    // this; SELECT / unit / derived pushdown share the same rewriter
    // and need the same gate.
    if (stoolap_pushdown::any_leaf_has_shadow_column(sel_lex)) return false;

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

    // ci guard: bail when ci columns appear in WHERE/HAVING/ORDER/GROUP
    // or in non-bare projections (LOWER(s), scalar subqueries, ...).
    // trust_binary_strings opts out.
    if (!stoolap_thd_trust_binary_strings(thd)) {
        if (select_lex_pushdown_uses_ci(sel_lex)) return false;
    }
    // DECIMAL is unconditional (type mismatch, not collation): stored as
    // TEXT, pushed numeric predicates miss.
    if (select_lex_uses_decimal_field(sel_lex)) return false;
    // Stoolap binds `?` only in WHERE/HAVING; placeholders elsewhere
    // come back as NULL. Statement-wide count backstop runs at factory
    // level, not here, to handle prepared UNIONs.
    if (thd->lex && thd->lex->param_list.elements > 0 &&
        select_param_in_unsafe_surface(sel_lex)) {
        return false;
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

namespace stoolap_pushdown {

select_handler* create_stoolap_select_handler(THD* thd, SELECT_LEX* sel_lex,
                                              SELECT_LEX_UNIT* sel_unit) {
    const bool trace = perf_trace_on();
    const uint64_t t_factory_start = trace ? now_ns() : 0;
    if (!can_pushdown_select(thd, sel_lex, sel_unit)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    // Statement-scope param backstop: bail when our per-SELECT walker
    // accounted for fewer placeholders in WHERE/HAVING than
    // `LEX::param_list` reports for the whole statement. The shortfall
    // means a placeholder lives in an Item subclass we don't reach;
    // pushing would risk a NULL projection. (The unit factory has its
    // own backstop scoped across all legs.)
    if (thd->lex && thd->lex->param_list.elements > 0 &&
        statement_param_backstop_unsafe(
            thd, select_count_predicate_params(sel_lex))) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    std::string sql;
    if (!build_pushdown_select_sql(thd, sel_lex, sql)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db) return nullptr;

    // Make sure the stoolap tx is open if the THD is inside an explicit
    // BEGIN. external_lock on a per-table handler normally does this,
    // but create_select can fire first when we install the pushed plan
    // as the whole-statement executor; without this call the eager
    // query below would route through the autocommit handle and ignore
    // the session's snapshot. No-op outside an explicit transaction.
    if (register_trx(thd) != 0) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    // Diagnostic: dump stoolap's own EXPLAIN of the pushed SQL to the
    // server error log when the session has opted in. Runs an extra
    // stoolap_query per pushed SELECT, so it's off by default. Surfaces
    // the plan stoolap actually picked, which MariaDB's EXPLAIN hides
    // (it just says "PUSHED SELECT" with everything else NULL).
    if (stoolap_thd_explain_pushdown(thd)) {
        std::string esql = "EXPLAIN " + sql;
        StoolapRows* erows = nullptr;
        const int erc = ctx->has_tx()
                            ? stoolap_tx_query(ctx->tx(), esql.c_str(), &erows)
                            : stoolap_query(db, esql.c_str(), &erows);
        if (erc == STOOLAP_OK && erows) {
            sql_print_information("stoolap[explain]: %s", sql.c_str());
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

    // Bind any prepared-statement parameters. In PREPARE phase the
    // factory has already bailed; here we're in EXECUTE phase or plain
    // (non-prepared) mode. param_list is empty for plain SELECTs.
    std::vector<StoolapValue> params;
    std::vector<std::string> param_storage;
    if (!collect_bound_params(thd, params, param_storage)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;  // unsupported param type -> row-pump fallback
    }

    // Eagerly run so we can fall back to row-pump on any stoolap-side
    // error (unknown function, type mismatch, ...). Once we return
    // non-NULL, MariaDB has committed and a later init_scan/next_row
    // failure surfaces to the user instead of a clean fallback.
    // stoolap_prepare catches only syntax; semantic errors like
    // "Function not found: MICROSECOND" surface at execute time only.
    StoolapRows* raw = nullptr;
    int rc;
    const uint64_t t_eager_start = trace ? now_ns() : 0;
    if (params.empty()) {
        rc = ctx->has_tx() ? stoolap_tx_query(ctx->tx(), sql.c_str(), &raw)
                           : stoolap_query(db, sql.c_str(), &raw);
    } else {
        const StoolapValue* pv = params.data();
        const int32_t pn = static_cast<int32_t>(params.size());
        rc = ctx->has_tx()
                 ? stoolap_tx_query_params(ctx->tx(), sql.c_str(), pv, pn, &raw)
                 : stoolap_query_params(db, sql.c_str(), pv, pn, &raw);
    }
    const uint64_t eager_ns = trace ? (now_ns() - t_eager_start) : 0;
    if (trace) {
        stoolap_mariadb::g_stats.perf_eager_query_ns.fetch_add(
            eager_ns, std::memory_order_relaxed);
    }
    if (rc != STOOLAP_OK) {
        // No error surfacing: row-pump fallback may still complete.
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    PushdownPacked packed;
    packed.take(stoolap_mariadb::RowsPtr(raw));
    if (!packed.ok) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    auto* h = new ha_stoolap_select_handler(thd, sel_lex, std::move(sql),
                                            std::move(packed));
    if (trace) {
        const uint64_t total = now_ns() - t_factory_start;
        stoolap_mariadb::g_stats.perf_factory_setup_ns.fetch_add(
            total > eager_ns ? total - eager_ns : 0, std::memory_order_relaxed);
        stoolap_mariadb::g_stats.perf_query_count.fetch_add(
            1, std::memory_order_relaxed);
    }
    return h;
}

select_handler* create_stoolap_unit_handler(THD* thd, SELECT_LEX_UNIT* unit) {
    if (!thd || !unit) return nullptr;
    SELECT_LEX* first = unit->first_select();
    if (!first) return nullptr;

    // Single-leg "unit" is handled by the SELECT factory; avoid claiming twice.
    if (!first->next_select()) return nullptr;

    // Each leg must push on its own. The bare-COUNT carve-out fires
    // per leg too -- rare in practice for UNIONs.
    for (SELECT_LEX* sel = first; sel; sel = sel->next_select()) {
        if (!can_pushdown_select(thd, sel, /*sel_unit=*/nullptr)) {
            stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
                1, std::memory_order_relaxed);
            return nullptr;
        }
    }

    // Set-op-level ci guard: per-leg lets bare ci projections through
    // (byte-safe in single SELECT) but UNION DISTINCT / INTERSECT /
    // EXCEPT dedup compares projection bytes; a ci VARCHAR diverges.
    if (!stoolap_thd_trust_binary_strings(thd) && unit_pushdown_uses_ci(unit)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }
    // DECIMAL bail is unconditional (type mismatch, not collation).
    if (unit_uses_decimal_field(unit)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }
    // Unit-wide param backstop (per-leg comparison would mis-bail on
    // UNIONs whose placeholders split across legs).
    if (thd->lex && thd->lex->param_list.elements > 0) {
        if (unit_param_in_unsafe_surface(unit)) {
            stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
                1, std::memory_order_relaxed);
            return nullptr;
        }
        if (statement_param_backstop_unsafe(
                thd, unit_count_predicate_params(unit))) {
            stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
                1, std::memory_order_relaxed);
            return nullptr;
        }
    }

    // SELECT_LEX_UNIT::print() emits canonical UNION form with all legs
    // + tail clauses; QT_PARSABLE keeps it round-trippable.
    String tmp;
    unit->print(
        &tmp, static_cast<enum_query_type>(QT_PARSABLE | QT_TO_SYSTEM_CHARSET));
    if (!tmp.length()) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    // Leaves from every leg -- rewriter normalises across the union.
    std::vector<LeafName> leaves;
    for (SELECT_LEX* sel = first; sel; sel = sel->next_select()) {
        collect_leaf_names(sel, leaves);
    }
    if (leaves.empty()) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    std::string raw(tmp.ptr(), tmp.length());
    std::string sql = rewrite_table_names(raw, leaves, thd->db.str);
    if (sql.empty()) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db) return nullptr;

    // create_unit can fire before external_lock; open the tx now so a
    // pushed UNION inside START TRANSACTION sees the session's snapshot.
    if (register_trx(thd) != 0) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    std::vector<StoolapValue> params;
    std::vector<std::string> param_storage;
    if (!collect_bound_params(thd, params, param_storage)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    StoolapRows* raw_rows = nullptr;
    int rc;
    if (params.empty()) {
        rc = ctx->has_tx() ? stoolap_tx_query(ctx->tx(), sql.c_str(), &raw_rows)
                           : stoolap_query(db, sql.c_str(), &raw_rows);
    } else {
        const StoolapValue* pv = params.data();
        const int32_t pn = static_cast<int32_t>(params.size());
        rc = ctx->has_tx()
                 ? stoolap_tx_query_params(ctx->tx(), sql.c_str(), pv, pn,
                                           &raw_rows)
                 : stoolap_query_params(db, sql.c_str(), pv, pn, &raw_rows);
    }
    if (rc != STOOLAP_OK) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    PushdownPacked packed;
    packed.take(stoolap_mariadb::RowsPtr(raw_rows));
    if (!packed.ok) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
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

// SELECT_LEX::print(QT_PARSABLE) emits the inner SELECT in spec-canonical
// form so stoolap's parser accepts it; thd->query() would carry the outer
// statement text, not what we need to push.
bool build_derived_sql(THD* thd, SELECT_LEX* sel_lex, std::string& out_sql) {
    if (!thd || !sel_lex) return false;
    String tmp;
    sel_lex->print(
        thd, &tmp,
        static_cast<enum_query_type>(QT_PARSABLE | QT_TO_SYSTEM_CHARSET));
    if (!tmp.length()) return false;

    std::string raw(tmp.ptr(), tmp.length());
    std::vector<LeafName> leaves;
    collect_leaf_names(sel_lex, leaves);
    if (leaves.empty()) return false;

    out_sql = rewrite_table_names(raw, leaves, thd->db.str);
    return !out_sql.empty();
}

}  // namespace

namespace stoolap_pushdown {

derived_handler* create_stoolap_derived_handler(THD* thd, TABLE_LIST* derived) {
    if (!thd || !derived || !derived->derived) return nullptr;

    SELECT_LEX_UNIT* unit = derived->derived;
    SELECT_LEX* sel = unit->first_select();
    if (!sel) return nullptr;

    // Phase 3: single-SELECT derived only. UNIONed derived tables would
    // need the unit-shape SQL, deferred for now.
    if (sel->next_select()) return nullptr;

    // Reuse the SELECT pushdown predicate. Pass sel_unit=NULL because the
    // derived's inner SELECT is, from the predicate's POV, a single SELECT
    // to push -- not part of an outer UNION.
    if (!can_pushdown_select(thd, sel, /*sel_unit=*/nullptr)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    std::string sql;
    if (!build_derived_sql(thd, sel, sql)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    // Refuse prepared statements: LEX::param_list is statement-wide, but
    // we build SQL from only the inner SELECT. Stoolap re-numbers `?`
    // from 1 when reparsing isolated inner SQL, so outer placeholders
    // ahead of ours would silently mis-bind the derived's params.
    // print(QT_PARSABLE) currently substitutes bound literals; we don't
    // rely on that.
    if (thd->lex && thd->lex->param_list.elements > 0) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    StoolapDB* db = ctx->db();
    if (!db) return nullptr;

    // create_derived can run before external_lock; open the tx now.
    if (register_trx(thd) != 0) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    // No prepared-statement parameters here (we bailed above when
    // present). collect_bound_params is left in place because plain
    // SELECT pushdown still needs it; calling it for derived now is a
    // pure no-op that returns an empty vector.
    std::vector<StoolapValue> params;
    std::vector<std::string> param_storage;
    if (!collect_bound_params(thd, params, param_storage)) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    StoolapRows* raw = nullptr;
    int rc;
    if (params.empty()) {
        rc = ctx->has_tx() ? stoolap_tx_query(ctx->tx(), sql.c_str(), &raw)
                           : stoolap_query(db, sql.c_str(), &raw);
    } else {
        const StoolapValue* pv = params.data();
        const int32_t pn = static_cast<int32_t>(params.size());
        rc = ctx->has_tx()
                 ? stoolap_tx_query_params(ctx->tx(), sql.c_str(), pv, pn, &raw)
                 : stoolap_query_params(db, sql.c_str(), pv, pn, &raw);
    }
    if (rc != STOOLAP_OK) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    PushdownPacked packed;
    packed.take(stoolap_mariadb::RowsPtr(raw));
    if (!packed.ok) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }
    stoolap_mariadb::g_stats.pushdown_hits.fetch_add(1,
                                                     std::memory_order_relaxed);
    return new ha_stoolap_derived_handler(thd, std::move(sql),
                                          std::move(packed));
}

bool item_safe_for_byte_comparison(Item* item) {
    return !item_uses_ci_string(item);
}

}  // namespace stoolap_pushdown
