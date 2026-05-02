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

// Defined in ha_stoolap.cc. Returns 1 when the session has set
// stoolap_trust_binary_strings = ON, opting into ci-collation bypass.
extern "C" int stoolap_thd_trust_binary_strings(THD* thd);

// Defined in ha_stoolap.cc. Returns 1 when the session has set
// stoolap_explain_pushdown = ON; the factory then runs EXPLAIN of the
// pushed SQL through stoolap and dumps the plan to the server log.
extern "C" int stoolap_thd_explain_pushdown(THD* thd);

// Defined in ha_stoolap.cc. Same logic external_lock uses to register
// the engine with MariaDB's tx manager and open a stoolap tx when the
// THD is inside an explicit BEGIN block. Pushdown factories call it
// defensively because create_select can fire before any handler's
// external_lock when the planner installs us as the whole-statement
// executor; without it a pushed SELECT inside START TRANSACTION (or
// WITH CONSISTENT SNAPSHOT) would run on the autocommit handle and
// miss the session's snapshot.
extern int register_trx(THD* thd);

// Defined in ha_stoolap.cc. Maps a stoolap error string to the right
// HA_ERR_* code and, for generic-class codes, surfaces the original
// text via my_printf_error so the user sees the real cause instead of
// "Got error 168". Direct DML used to skip this and always returned
// HA_ERR_GENERIC, hiding constraint failures behind error 1296.
extern int report_stoolap_error(const char* msg);

// Defined in ha_stoolap.cc. From a stoolap "unique constraint failed
// ... on column X" message, returns the matching MariaDB key index so
// the handler can publish errkey for ON DUPLICATE KEY / REPLACE.
extern unsigned guess_errkey(const char* msg, TABLE_SHARE* share);

// Exposed to ha_stoolap.cc (which intentionally doesn't include sql_lex.h
// to keep wsrep header dependencies minimal). Reports whether the THD's
// outermost SELECT has an explicit LIMIT clause; rnd_init uses this to
// pick between Tier 3 buffered scan and the streaming row pump.
extern "C" int stoolap_thd_has_explicit_limit(THD* thd) {
    if (!thd || !thd->lex) return 0;
    SELECT_LEX* sel = thd->lex->first_select_lex();
    if (!sel) return 0;
    return sel->limit_params.explicit_limit ? 1 : 0;
}

// Exposed to ha_stoolap.cc::cond_push so it can route UPDATE/DELETE
// through the direct path without including sql_class.h.
extern "C" int stoolap_thd_is_update_or_delete(THD* thd) {
    if (!thd || !thd->lex) return 0;
    const int cmd = thd->lex->sql_command;
    return (cmd == SQLCOM_UPDATE || cmd == SQLCOM_DELETE) ? 1 : 0;
}

// Exposed to ha_stoolap.cc::start_bulk_insert. Returns 1 when the
// INSERT shape needs MariaDB to drive per-row dup-key recovery:
// INSERT IGNORE (ignore=1, drop conflicting rows + emit warnings),
// REPLACE (DUP_REPLACE, delete-then-insert per dup), and
// INSERT ... ON DUPLICATE KEY UPDATE (DUP_UPDATE, update-existing
// per dup). For these the bulk batching path is unsafe: stoolap's
// stmt_exec_batch is all-or-nothing, so a single dup aborts the
// whole batch and the caller-side recovery never gets to run --
// silently dropping every non-conflicting row in the batch.
extern "C" int stoolap_thd_needs_per_row_dup_handling(THD* thd) {
    if (!thd || !thd->lex) return 0;
    if (thd->lex->ignore) return 1;
    if (thd->lex->duplicates == DUP_REPLACE) return 1;
    if (thd->lex->duplicates == DUP_UPDATE) return 1;
    return 0;
}

// Brewed mariadbd doesn't ship the my_print_error_service struct that
// `mysql/plugin.h` rewires my_error / my_printf_error through, so we undef
// the macros and bind directly to the exported symbols. This lets us hand
// the real stoolap error to the user instead of the generic "Got error 122
// (unspecified)" fallback.
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
 *  explicitly into derived tables' inner SELECT_LEX(s). Relying on the
 *  global TABLE_LIST chain (`next_global`) used to work but mixed in
 *  TABLE_LIST entries from other parts of the LEX (subqueries in WHERE,
 *  etc.); explicit recursion is both narrower and easier to reason about.
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

// Returns true if any leaf table the rewriter would touch -- outer
// FROM, derived tables, AND every inner-unit subquery -- has a column
// whose name (case-insensitively) matches its bare table name. This
// is the unsafe shape for the text-based rewriter: every word-boundary
// occurrence of the table's bare name gets replaced with the
// stoolap-flat name, which mangles SET / WHERE column tokens that
// happen to share the table name.
//
// Mirrors collect_leaf_names()'s walk so the guard covers exactly
// the same set of names the rewriter will touch.
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

// Cross-leaf shadow detector. The text rewriter replaces every word-
// boundary occurrence of EVERY leaf table name with that table's
// stoolap-flat form. So even if a leaf's own table name doesn't
// collide with its columns, an OTHER leaf's table name can: e.g.
// `UPDATE u SET o = o + 1 WHERE EXISTS (SELECT 1 FROM o ...)` has
// outer leaf `u` (column `o`) and subquery leaf `o`. The bare `o`
// tokens in `SET o = o + 1` get rewritten to the flat form of table
// `o`, mangling the SET column. Refuse direct DML when any leaf's
// columns share a name with any (other) leaf's table.
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

    // Helper: walk a list of Items and report whether any contains an
    // unsafe field reference. Used for both the WHERE clause AND the
    // SET RHS expressions; both end up evaluated server-side and would
    // miscompare against Stoolap's bytewise / text storage.
    auto items_unsafe_for_pushdown = [&](Item* root) {
        if (!root) return false;
        const bool ci_unsafe =
            !stoolap_thd_trust_binary_strings(thd) && item_uses_ci_string(root);
        // DECIMAL bail is unconditional (type mismatch, not collation).
        const bool decimal_unsafe = item_uses_decimal_field(root);
        return ci_unsafe || decimal_unsafe;
    };

    if (items_unsafe_for_pushdown(sel->where)) return false;
    // Direct DML over a JOIN (multi-table UPDATE/DELETE is already
    // blocked above, but a single-table UPDATE may still carry a
    // semi-join-lifted on_expr). Walk every TABLE_LIST's ON-expressions.
    if (any_on_expr_matches_chain(sel->get_table_list(), item_uses_ci_string) &&
        !stoolap_thd_trust_binary_strings(thd))
        return false;
    if (any_on_expr_matches_chain(sel->get_table_list(),
                                  item_uses_decimal_field))
        return false;

    // The SET RHS expressions live in `thd->lex->value_list` for UPDATE
    // statements (DELETE has no SET, so this loop is empty there). A
    // scalar subquery like
    //   UPDATE u SET n = (SELECT COUNT(*) FROM o WHERE status='completed')
    // would otherwise direct-route to stoolap which compares bytewise
    // against rows storing 'COMPLETED' and silently writes the wrong
    // value. Same gate as the WHERE walk: ci is opt-in via
    // trust_binary_strings, DECIMAL is unconditional.
    if (cmd == SQLCOM_UPDATE) {
        List_iterator<Item> it(thd->lex->value_list);
        Item* expr;
        while ((expr = it++)) {
            if (items_unsafe_for_pushdown(expr)) return false;
        }
        // Also bail when the UPDATE TARGET column is DECIMAL. Stoolap
        // stores DECIMAL as TEXT; assignments like `SET dec_v = 2.50`
        // pass a numeric literal/param to stoolap which rounds through
        // float before writing back as TEXT, dropping scale and (for
        // high-precision DECIMALs) losing low-order digits. The SET
        // LHS columns live in `first_select_lex()->item_list` for
        // UPDATE statements; walk them and bail on any DECIMAL target.
        // The row-pump path is correct because Field::store on the
        // DECIMAL field receives the bound MariaDB value at full
        // precision and writes it through `extract_field` -> stoolap
        // text without an intermediate float conversion.
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

    // The direct-DML rewriter is text-based: it rewrites every
    // word-boundary occurrence of any leaf table's bare name to the
    // stoolap-flat name. If ANY of those leaves -- outer FROM, derived
    // tables, EXISTS / IN / scalar-subquery FROMs -- has a column
    // sharing its table name, that column token gets rewritten too.
    // Stoolap then sees an UPDATE/WHERE/SET clause assigning or
    // comparing against a quoted table identifier, which yields a
    // generic engine error (already after we've claimed the direct
    // path). Decline up front; the row-pump fallback handles those
    // statements correctly via per-row callback.
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
        const char* msg =
            ctx->has_tx() ? stoolap_tx_errmsg(ctx->tx()) : stoolap_errmsg(db);
        if (!msg || !*msg) msg = "unknown error";
        sql_print_error("stoolap: direct DML failed: %s", msg);
        // Map to the right HA_ERR_* (used to be a blanket
        // HA_ERR_GENERIC, which hid UNIQUE / FK / lock errors as a
        // single error class). For dup-key, also publish errkey via
        // the out-param so the handler can hand a real key index to
        // MariaDB's get_dup_key / ON DUPLICATE KEY routing.
        //
        // MariaDB's direct-DML path does not always invoke
        // print_error after the engine returns, so a mapped FK or
        // dup-key would otherwise rollback silently with no client
        // diagnostic. Always surface the stoolap text via
        // my_printf_error so the user sees a real reason. The
        // structured HA_ERR_* still drives SQLSTATE classification
        // and any handler-level retry logic.
        const int mapped = report_stoolap_error(msg);
        if (mapped == HA_ERR_FOUND_DUPP_KEY && errkey_out) {
            TABLE_LIST* tl = thd->lex->query_tables;
            TABLE_SHARE* share = (tl && tl->table) ? tl->table->s : nullptr;
            *errkey_out = guess_errkey(msg, share);
        }
        // MariaDB's direct-DML hooks don't always call print_error
        // after the engine returns, so the user would otherwise see a
        // silent rollback. Emit the concrete MariaDB error number for
        // the mapped codes so a UNIQUE collision shows up as 1062
        // (ER_DUP_ENTRY) and an FK violation as 1452
        // (ER_NO_REFERENCED_ROW_2) -- matching what the row-pump path
        // produces. report_stoolap_error already raises a 1296 for
        // the HA_ERR_GENERIC / HA_ERR_UNSUPPORTED bucket, so skip
        // those here to avoid double-publishing.
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
                // HA_ERR_GENERIC / HA_ERR_UNSUPPORTED already
                // surfaced via report_stoolap_error.
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

/**
 * Recursively check whether `item` references a non-binary string field
 * in a position where stoolap's byte-wise compare would diverge from
 * MariaDB's collation rules. Used by the ci-collation guard to bail
 * pushdown when the offending column appears in WHERE / HAVING / ORDER
 * BY / GROUP BY -- *not* when it's just projected back, where stoolap
 * is indistinguishable from MariaDB.
 *
 * Walks Item_field (terminal), Item_func / Item_sum (args[]), Item_cond
 * (argument_list), Item_ref (deref), and Item_subselect (recurse into
 * the inner unit's WHERE/HAVING/ORDER/GROUP). Anything else is a leaf.
 *
 * Without the subquery recursion, predicates like
 *   WHERE EXISTS (SELECT 1 FROM o WHERE o.status = 'completed')
 * would slip the guard and push to stoolap, which would then byte-match
 * 'completed' against rows that say 'COMPLETED' -- silently returning
 * fewer rows than MariaDB's ci compare expects.
 */
bool item_uses_ci_string(Item* item);
bool order_list_uses_ci_string(SQL_I_List<ORDER>& lst);
bool select_lex_pushdown_uses_ci(SELECT_LEX* sel, bool include_projection);

/** Run `pred` against every ON-expression reachable from the FROM list
 *  of `sel` -- the explicit `on_expr`, the optimizer-staged
 *  `prep_on_expr`, AND the semi-join lifted `sj_on_expr`. Recurses
 *  into nested-joins (`tl->nested_join->join_list`) so a multi-level
 *  outer join chain is fully covered. Returns true on the first
 *  predicate hit.
 *
 *  Used by both the ci and DECIMAL guards: a JOIN ON predicate that
 *  compares ci-collated columns or a DECIMAL column would silently
 *  miscompare on stoolap (ON r.s = l.s on `_general_ci` VARCHAR
 *  byte-matches on stoolap; same for DECIMAL stored as TEXT). */
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

/** Unit-level ci check: returns true when a multi-leg SELECT_LEX_UNIT
 *  would compare ci-string projections byte-wise on stoolap.
 *
 *  Three surfaces matter:
 *    - UNION DISTINCT / INTERSECT / EXCEPT dedup compares projection bytes.
 *    - Tail-level ORDER BY / GROUP BY (on global_parameters()) compares
 *      projection bytes.
 *    - Per-leg WHERE/HAVING/etc. via select_lex_pushdown_uses_ci.
 *
 *  Used by both create_stoolap_unit_handler (when the unit is the whole
 *  query) AND by the derived-table recursion in select_lex_pushdown_uses_ci
 *  (when the unit is materialised as a derived inside a larger plan that
 *  then pushes as a whole-SELECT). */
bool unit_pushdown_uses_ci(SELECT_LEX_UNIT* unit) {
    if (!unit) return false;
    SELECT_LEX* first = unit->first_select();
    if (!first) return false;
    const bool multi_leg = (first->next_select() != nullptr);

    // Per-leg compare-sensitive clauses get the same recursive treatment
    // as a stand-alone SELECT.
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

/** ci-string check for a SELECT considered as a whole-query pushdown target.
 *
 *  Same shape as the inline checks below, but recurses into derived
 *  TABLE_LIST entries -- their inner SELECTs run as part of the same
 *  pushed plan, so their compare-sensitive clauses must be ci-safe too.
 *  Without this recursion `SELECT COUNT(*) FROM (SELECT s FROM t WHERE
 *  s='x') d` would slip past: the outer SELECT has no ci surfaces and
 *  the derived's WHERE was never inspected, so stoolap byte-matched
 *  'x' against rows that say 'X'.
 *
 *  `include_projection` controls whether SELECT-list expressions are
 *  inspected. The outer pushdown context wants this on (a non-bare
 *  projection like `LOWER(s)` performs ci-sensitive work whose result
 *  flows out to the client). EXISTS subqueries set it to false: the
 *  EXISTS predicate is pure row-existence, so its SELECT list is
 *  semantically ignored and a projection like `EXISTS (SELECT
 *  LOWER(s) ...)` is byte-safe regardless of what's in the SELECT.
 *  The flag propagates through derived-TABLE_LIST recursion so a
 *  derived nested inside an EXISTS body is also free of projection
 *  inspection (its rows are materialized but never compared).
 */
bool select_lex_pushdown_uses_ci(SELECT_LEX* sel,
                                 bool include_projection = true) {
    if (!sel) return false;
    // Compare-sensitive clauses of THIS select.
    if (sel->where && item_uses_ci_string(sel->where)) return true;
    if (sel->having && item_uses_ci_string(sel->having)) return true;
    if (order_list_uses_ci_string(sel->order_list)) return true;
    if (order_list_uses_ci_string(sel->group_list)) return true;
    // JOIN ON predicates compare in the engine the same way WHERE
    // does. Without this walk, `LEFT JOIN ... ON r.s = l.s` over ci
    // VARCHAR pushed and stoolap byte-matched 'COMPLETED' vs
    // 'completed' to NULL.
    if (any_on_expr_matches_chain(sel->get_table_list(), item_uses_ci_string)) {
        return true;
    }
    if (include_projection) {
        // Bare field refs are byte-safe in plain SELECT pushdown
        // (`SELECT s FROM t` -- the bytes flow to the client unchanged).
        // Same for an Item_ref that resolves to a bare field: that's
        // how MariaDB models projections of derived-table columns
        // (`SELECT y.name FROM (SELECT name FROM t) y`), and the
        // bytes still flow through unchanged.
        // EXCEPT when SELECT DISTINCT is in play: dedup compares the
        // projected bytes positionally, same shape as UNION DISTINCT.
        // With ci collation MariaDB folds 'COMPLETED' and 'completed'
        // to one row, but stoolap byte-dedups to two. Treat bare ci
        // refs as unsafe in that mode.
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
    // Recurse into derived TABLE_LIST entries -- the derived's clauses
    // run inside the pushed plan and need the same protection.
    for (TABLE_LIST* tl = sel->get_table_list(); tl; tl = tl->next_local) {
        if (!tl->derived) continue;
        // For derived tables that ARE multi-leg units (UNION / INTERSECT
        // / EXCEPT), the unit-level set-op semantics also apply within
        // the pushed plan: dedup compares projection bytes, tail ORDER
        // BY compares projection bytes. Use the unit-level check so
        // those surfaces are caught here too.
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
            // Walk every leg of the subquery's unit (handles UNION-shaped
            // subqueries too). Use the recursive variant so derived
            // TABLE_LIST entries nested inside the subquery -- e.g.
            // `EXISTS (SELECT 1 FROM (SELECT s FROM t WHERE s='x') d)` --
            // get their compare-sensitive clauses inspected too. Items
            // that bridge to outer queries (correlated refs) are wrapped
            // as Item_field at this point; they're already covered by
            // FIELD_ITEM in the outer walk.
            //
            // EXISTS is a pure row-existence predicate -- its SELECT
            // list is semantically ignored. Skip projection inspection
            // entirely (both bare ci fields AND non-bare expressions
            // like LOWER(s) are harmless when the projection is never
            // compared). IN / scalar / ALL / ANY all consume the
            // projected value, so they still need full inspection
            // including bare fields.
            const bool exists_only =
                ss->substype() == Item_subselect::EXISTS_SUBS;
            for (SELECT_LEX* sel = u->first_select(); sel;
                 sel = sel->next_select()) {
                if (select_lex_pushdown_uses_ci(
                        sel, /*include_projection=*/!exists_only))
                    return true;
                if (exists_only) continue;
                // Stricter than the outer-pushdown projection rule: a
                // scalar/IN subquery `(SELECT col)` whose result row is
                // consumed by an outer compare needs to bail even on a
                // bare ci field, because that outer compare runs
                // bytewise on stoolap. select_lex_pushdown_uses_ci
                // intentionally lets bare FIELD_ITEM through for the
                // outer SELECT pushdown case (pure projection without
                // an outer compare); re-walk here to catch it.
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

// True when `item` references a DECIMAL column anywhere -- directly,
// inside a function, inside a subquery's compare surface, or via a
// nested derived table. DECIMAL columns are stored as Stoolap TEXT, so
// any pushed predicate that compares a DECIMAL column against a
// numeric literal silently misses (text 'v=1.50' vs numeric 1.50
// returns no match in stoolap). Pure projection of a bare DECIMAL
// field IS safe (the text round-trips through Field::store), so this
// walker mirrors the ci-string one and skips bare FIELD_ITEM in the
// projection-only case via the `select_lex_uses_decimal_field`
// recursion below. This guard is NOT gated by stoolap_trust_binary_strings:
// DECIMAL is a type-mismatch issue, orthogonal to collation.
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

// True when `item` contains an Item_param (a `?` placeholder) anywhere
// in its tree. dynamic_cast catches the bound case: at EXECUTE time
// Item_param::type() returns CONST_ITEM (or NULL_ITEM if bound to
// NULL), not PARAM_ITEM, so a tag-based check would miss it.
// dynamic_cast hits the actual subclass regardless of declared type.
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

// Counter walker: returns the number of Item_param nodes reachable
// from `item`. Mirrors item_contains_param's recursion.
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

// Count Item_params reachable from WHERE / HAVING surfaces (stoolap
// binds those correctly). Recurses into derived tables and inner
// units so subquery predicates get counted at every depth.
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

// Returns true when this SELECT (or any nested derived / subquery)
// has a `?` placeholder in a projection / ORDER BY / GROUP BY surface
// where stoolap silently returns NULL instead of the bound value.
// Predicates (WHERE / HAVING) are NOT checked: stoolap binds those
// correctly and our pushdown depends on it.
//
// This is the per-SELECT walker, safe to call per-leg of a unit.
// The statement-wide count backstop (compare `param_list.elements`
// against placeholders found in predicates across the whole
// statement) is intentionally NOT applied here -- a per-leg call
// would see the statement total but only its own leg's predicate
// count, falsely bailing on a prepared UNION whose params split
// across legs. The factory wrappers below apply the backstop at
// the top scope (single-SELECT factory or unit factory) instead.
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
    // Recurse into derived TABLE_LISTs (their projections become
    // values in the outer plan).
    for (TABLE_LIST* tl = sel->get_table_list(); tl; tl = tl->next_local) {
        if (!tl->derived) continue;
        for (SELECT_LEX* inner = tl->derived->first_select(); inner;
             inner = inner->next_select()) {
            if (select_param_in_unsafe_surface(inner)) return true;
        }
    }
    // Subqueries (Item_subselect) live in inner units.
    for (SELECT_LEX_UNIT* u = sel->first_inner_unit(); u; u = u->next_unit()) {
        for (SELECT_LEX* sub = u->first_select(); sub;
             sub = sub->next_select()) {
            if (select_param_in_unsafe_surface(sub)) return true;
        }
    }
    return false;
}

// Statement-scope backstop: bail when the predicate placeholders we
// can account for don't sum to `thd->lex->param_list.elements`. The
// shortfall means a placeholder lives in an Item subclass our
// per-SELECT walker doesn't reach -- conservatively reject pushdown
// rather than hand a NULL projection back to the user.
//
// `predicate_param_count` is the running tally across whatever
// SELECTs the caller already walked (single SELECT, or every leg of
// a unit plus global_parameters). It must NOT be re-computed per
// leg; that's the whole reason this lives outside
// `select_param_in_unsafe_surface`.
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
    // global_parameters() carries the unit-level ORDER BY / GROUP BY /
    // LIMIT (e.g. `... UNION ... ORDER BY ?`); a placeholder there is
    // also non-predicate and bails.
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

// Tally predicate-only placeholders across every SELECT in a unit
// (legs + global_parameters). Used by the unit factory to pair with
// `statement_param_backstop_unsafe`.
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

/** Build a stoolap-flavoured SQL string for the current SELECT. Reuses the
 *  same identifier rewriter as the direct-DML path so naming stays in sync. */
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

/**
 * Collect bound parameter values from LEX::param_list into a StoolapValue
 * array. Returns false if any param has an unsupported type so the caller
 * can bail to the row pump.
 *
 * `storage` keeps owning copies of any string values so the StoolapValue
 * text pointers stay valid for the duration of the stoolap call.
 *
 * In PREPARE phase params are NO_VALUE; we don't get here in that case
 * (factory bails earlier on is_stmt_prepare()). In EXECUTE phase each
 * Item_param has a bound value reachable via val_int/val_real/val_str.
 */
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
                // Bind DECIMAL as exact TEXT, never as FLOAT. val_real()
                // would round through double, losing low-order digits
                // for integer values above 2^53 (DECIMAL params often
                // come from JDBC / numeric ranges that exceed double's
                // 53-bit mantissa) and dropping scale on real DECIMALs.
                // Stoolap accepts a TEXT param against a numeric column
                // via implicit conversion at full precision -- verified
                // for INT64 values above 2^53.
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

/** SELECT pushdown eligibility. Returning false here makes the factory
 *  return NULL so MariaDB falls back to the row-pump path; that's the
 *  correct behaviour for any case stoolap can't reproduce semantically. */
bool can_pushdown_select(THD* thd, SELECT_LEX* sel_lex,
                         SELECT_LEX_UNIT* sel_unit) {
    if (!thd || !sel_lex) return false;

    // Phase 1: single-SELECT only. Unit pushdown (UNION/EXCEPT/INTERSECT)
    // and partial pushdown (sel_lex inside a unit) are deferred -- we'd
    // need to emit the right unit-shape SQL and stoolap accepts the
    // surface form, but the column-binding rules differ enough to warrant
    // dedicated tests before turning it on.
    if (sel_unit) return false;

    // PREPARE phase: we don't have the bound parameters yet, so we'd
    // serialize "?" placeholders and stoolap would reject them. EXECUTE
    // phase, by contrast, lands here with bound params reachable through
    // LEX::param_list -- collect_bound_params() picks them up later and
    // routes the call through stoolap_query_params().
    if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare()) return false;

    // Stored-procedure context: thd->query() inside SP execution carries
    // the literal SP body text, so SP locals appear by name (`i`, not
    // `5`); stoolap would reject the resulting SQL. Substituting them
    // requires an Item-walker that's not feasible from the plugin (no
    // hookable Item processor). Defer -- SP loops fall to the row pump.
    if (thd->spcont) return false;

    // Triggers / sub-statements: side-effect-sensitive; let the row pump
    // handle them so MariaDB's semantics are preserved.
    if (thd->in_sub_stmt) return false;

    if (thd->lex->sql_command != SQLCOM_SELECT) return false;
    // lex->result is set for ANY SELECT execution path -- in particular,
    // it's a select_send (or its binary-protocol cousin) for normal
    // prepared-statement EXECUTE, which we DO want to push. Bail only
    // for INTO @var / INTO OUTFILE / INTO DUMPFILE, which subclass
    // select_result_interceptor.
    if (thd->lex->result && thd->lex->result->result_interceptor() != nullptr)
        return false;

    if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT) return false;
    if (sel_lex->uncacheable & UNCACHEABLE_RAND) return false;

    // FOR UPDATE / LOCK IN SHARE MODE: stoolap has its own MVCC semantics,
    // but the mapping from MariaDB's lock surface to stoolap's snapshot
    // model is non-trivial. Keep these on the row pump for now.
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

    // Bare COUNT(*) over a single base table with no filters: keep this off
    // Stoolap pushdown for now. Stoolap's bare COUNT(*) can use table-count
    // metadata that is not MVCC-safe around in-flight / rolled-back
    // transactions. Bailing here makes MariaDB use the row-pump aggregate,
    // which counts visible rows rather than metadata.
    //
    // Restricted to single base-table FROM. For
    // `SELECT COUNT(*) FROM (SELECT ... GROUP BY ...) z` the FROM has
    // one entry but it's a derived; records() doesn't know what to
    // count there -- pushing the whole query is the only way to win.
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
                    // COUNT(*) parses as COUNT(0) -- the arg is an integer
                    // literal. COUNT(col) gives an Item_field. Bail only
                    // on the literal-arg shape so COUNT(col), which
                    // excludes NULLs and needs a real query, keeps pushing.
                    if (a && a->const_item()) return false;
                }
            }
        }
    }

    // Stoolap compares strings byte-wise. MariaDB's default VARCHAR
    // collation is utf8mb3_general_ci, which case-folds. If a non-binary
    // string column appears in WHERE/HAVING/ORDER BY/GROUP BY the pushed
    // query would return different rows or order than the row-pump path
    // (e.g. `s = 'banana'` skipping 'BANANA'). Bail in those clauses --
    // unless the user has explicitly opted into byte-wise semantics via
    // SET stoolap_trust_binary_strings = 1.
    //
    // Pure projection of a ci-string column -- `SELECT s FROM ...` with
    // no collation-dependent comparison -- is fine: stoolap returns the
    // bytes, MariaDB writes them to the result row, no collation work
    // happens either side. But projection items that *aren't* bare
    // field refs (scalar subqueries, functions like LOWER/UPPER) DO
    // perform compare-like work whose ci semantics matter; check those.
    if (!stoolap_thd_trust_binary_strings(thd)) {
        // One walker that covers WHERE/HAVING/ORDER/GROUP, non-bare-field
        // projection items (catches `SELECT (SELECT MAX(s)...)` shapes),
        // and crucially recurses into derived TABLE_LIST units so that
        // `SELECT COUNT(*) FROM (SELECT s FROM t WHERE s='x') d` doesn't
        // bypass the guard via the derived's WHERE.
        if (select_lex_pushdown_uses_ci(sel_lex)) return false;
    }
    // DECIMAL bail is unconditional (not gated by trust_binary_strings).
    // Stoolap stores DECIMAL columns as TEXT; pushed numeric predicates
    // against them silently miss because stoolap compares text vs
    // numeric and returns no rows.
    if (select_lex_uses_decimal_field(sel_lex)) return false;
    // Stoolap currently binds prepared params only in predicate
    // surfaces (WHERE / HAVING). Placeholders in the projection,
    // ORDER BY, or GROUP BY come back as NULL instead of the bound
    // value. Bail when the SELECT has any param outside predicates
    // -- but only when the statement actually has bound params
    // (avoids penalising plain SELECTs that share the gating path).
    // Per-SELECT walker: detects placeholders in projection / ORDER /
    // GROUP / derived / inner-unit. Statement-wide count backstop runs
    // at the factory level (after this returns), not here -- a per-leg
    // call from the unit factory would otherwise compare a single
    // leg's predicate count against the statement total and falsely
    // bail on prepared UNIONs whose params split across legs.
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

    // Eagerly run the query so we can fall back to the row pump on any
    // stoolap-side error (unknown function, type mismatch, plan failure).
    // Once we return non-NULL, MariaDB has committed to this handler --
    // a later failure inside init_scan/next_row would surface as a query
    // error to the user instead of a clean fallback. stoolap_prepare()
    // only catches syntax errors; semantic errors like "Function not
    // found: MICROSECOND" surface only at execute time, which is why the
    // validation has to be a real query, not just a prepare.
    //
    // Why not stoolap_prepare + stoolap_stmt_query with a per-THD stmt
    // cache (which would skip stoolap's normalize/hash/RwLock cache
    // lookup on every call)? Tested twice with single-slot caching:
    // (a) LEFT JOIN+GROUP BY: 776us baseline -> 789us cached. (b) 5K
    // SELECT-by-id prepared: 30.40us baseline -> 29.87us cached. Both
    // within run-to-run noise. Stoolap's existing semantic cache
    // already keeps the parsed plan warm for repeated SQL strings, so
    // an additional plugin-side stmt cache is redundant. If a future
    // stoolap release changes that property, revisit. Until then the
    // direct query path is the simplest correct option.
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
        // Don't surface the error here -- the caller falls back to the
        // row pump which may complete the query successfully (server-side
        // function applied after our handler streams base rows).
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

    // No second SELECT means this isn't really a unit -- the SELECT
    // factory handles that case. Avoid claiming the work twice.
    if (!first->next_select()) return nullptr;

    // Per-leg eligibility: each SELECT in the union chain must be
    // pushable on its own (every leaf is stoolap, no FOR UPDATE, no
    // ci-string in WHERE/HAVING/ORDER/GROUP). One ineligible leg sinks
    // the whole unit. The bare-COUNT(*) carve-out fires per leg too,
    // so a UNION of literal-COUNT-only SELECTs falls back; rare in
    // practice, and MariaDB's records() shortcut doesn't combine
    // through UNION anyway, so the pushed path wouldn't strictly win
    // on those.
    for (SELECT_LEX* sel = first; sel; sel = sel->next_select()) {
        if (!can_pushdown_select(thd, sel, /*sel_unit=*/nullptr)) {
            stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
                1, std::memory_order_relaxed);
            return nullptr;
        }
    }

    // Set-op-level ci guard. The per-leg pushdown check intentionally
    // lets bare ci-string projections through (byte-safe in a single
    // SELECT pushdown). At the UNIT level, UNION DISTINCT / INTERSECT /
    // EXCEPT dedup compares projection bytes, and a tail-level ORDER BY
    // or GROUP BY does the same. Stoolap's compare is byte-wise so a ci
    // VARCHAR projected from any leg diverges: 'COMPLETED' and
    // 'completed' dedup to one row in MariaDB but stay separate in
    // stoolap.
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
    // Same prepared-param surface check as the SELECT factory: bail
    // when any leg has a placeholder in projection / ORDER / GROUP,
    // OR when the unit-level statement-scope count backstop trips.
    // The backstop pairs the *unit-wide* predicate-param tally
    // (across all legs + global_parameters) against the statement
    // total; per-leg comparison would falsely bail on prepared UNIONs
    // whose placeholders split across legs.
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

    // Build SQL via SELECT_LEX_UNIT::print() -- canonical UNION form
    // with all legs and any tail clauses (ORDER BY / LIMIT after the
    // final UNION). The same QT_PARSABLE flag that worked for the
    // derived path applies here.
    String tmp;
    unit->print(
        &tmp, static_cast<enum_query_type>(QT_PARSABLE | QT_TO_SYSTEM_CHARSET));
    if (!tmp.length()) {
        stoolap_mariadb::g_stats.pushdown_misses.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    // Collect leaves from EVERY leg so the rewriter can normalise table
    // identifiers across the whole union.
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

    // See create_stoolap_select_handler for why this defensive call is
    // needed: ensures the stoolap tx is open if the THD is inside an
    // explicit BEGIN, even when create_unit fires before external_lock.
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

/**
 * Build SQL for a single SELECT_LEX. Unlike the whole-query case where we
 * use thd->query() (the user's original text), the derived path needs
 * just the inner SELECT in isolation, so we let MariaDB's parser print()
 * it canonically. QT_PARSABLE keeps the output round-trippable through
 * any SQL parser that follows the spec, including stoolap's.
 */
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

    // Decline prepared derived pushdown until per-SELECT parameter
    // mapping exists. The factory builds SQL from only the inner
    // SELECT, but `LEX::param_list` is statement-wide. If the OUTER
    // statement has placeholders that appear in the SQL text BEFORE
    // the derived's placeholders, stoolap (which numbers `?` from 1
    // when reparsing the isolated inner SQL) would silently bind the
    // wrong THD-side values to the derived's placeholders. Today
    // MariaDB's `print(QT_PARSABLE)` happens to substitute bound
    // literals so the bug doesn't bite empirically, but that's an
    // implementation detail. Bail here so future MariaDB releases or
    // not-yet-substituted Item_param shapes can't introduce silent
    // result-row corruption. Whole-SELECT and unit pushdown stay safe
    // because their factories build SQL from the entire statement.
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

    // See create_stoolap_select_handler for the same reasoning: open the
    // stoolap tx if we're inside an explicit BEGIN before the eager
    // query, since create_derived can run before external_lock.
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
    // Reuse the same walker the SELECT eligibility check uses for
    // ci-collation guarding. An Item is "safe" iff it doesn't reference
    // a non-binary string column.
    return !item_uses_ci_string(item);
}

}  // namespace stoolap_pushdown
