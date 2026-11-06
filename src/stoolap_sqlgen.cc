/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

// SELECT pushdown SQL, generated from MariaDB's prepared Item tree.
//
// MariaDB creates the select_handler right after JOIN::prepare(): names
// are resolved and fix_fields() has run (so every Item knows its type and
// collation), but no optimizer rewrite has happened yet. Writing stoolap
// SQL from that tree, instead of forwarding the statement text, lets each
// construct be translated to its stoolap equivalent (NULL ordering, <=>,
// string literals) or refused when stoolap would compute something else.
// Refusing is always safe: MariaDB then runs the statement itself.

#define MYSQL_SERVER 1

// my_global.h must precede every other header, standard ones included.
#include "my_global.h"
#include "field.h"
#include "key.h"
#include "table.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_schema.h"
#include "sql_cte.h"
#include "item_sum.h"
#include "item_subselect.h"
#include "item_windowfunc.h"
#include "stoolap_types.h"
#include "sql_window.h"

#include "ha_stoolap.h"
#include "stoolap_sqlgen.h"
#include "stoolap_thd_inspect.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace stoolap_sqlgen {
namespace {

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

// What a value is to stoolap, for deciding whether an operation on it
// means the same thing on both sides.
// DATETIME covers DATE too, and TIME and TSTAMP (TIMESTAMP) are their
// own: stoolap holds each as an INTEGER that orders like the MariaDB
// value (stoolap_types.h), and a DATE packs as the DATETIME at midnight.
enum class Cls { INT, REAL, STR, DATETIME, TIME, TSTAMP, UNSAFE };

bool temporal(Cls c) {
    return c == Cls::DATETIME || c == Cls::TIME || c == Cls::TSTAMP;
}

bool numeric(Cls c) {
    return c == Cls::INT || c == Cls::REAL;
}

class Gen {
public:
    explicit Gen(THD* thd)
        : thd_(thd), trust_bin_(stoolap_thd_trust_binary_strings(thd) != 0) {
        out.reserve(1024);
    }

    // names: output column names, one per visible item; null for
    // positional names (c0, c1, ...).
    bool select(SELECT_LEX* sel, const std::vector<std::string>* names,
                const std::vector<Cls>* cols = nullptr);
    // A query expression: one SELECT, or set-operation legs.
    bool unit(SELECT_LEX_UNIT* u, const std::vector<std::string>* names);
    // A single-table UPDATE / DELETE (gen_modify).
    bool modify(SELECT_LEX* sel, Item* where);

    std::string out;
    const char* why = nullptr;
    SELECT_LEX* top = nullptr;  // the statement's own SELECT

    // Set for the SELECT of an EXISTS, whose select list is never read.
    bool exists_list_ = false;

private:
    // Appends a quoted identifier / a positional column name to out.
    void ident(std::string_view s) {
        out.push_back('"');
        if (s.find('"') == std::string_view::npos) {
            out.append(s);
        } else {
            for (char c : s) {
                if (c == '"') out.push_back('"');
                out.push_back(c);
            }
        }
        out.push_back('"');
    }
    void pos_name(uint n) {
        char buf[16];
        const int len = std::snprintf(buf, sizeof(buf), "\"c%u\"", n);
        out.append(buf, static_cast<size_t>(len));
    }

    bool fail(const char* w) {
        if (!why) why = w;
        return false;
    }

    bool from(SELECT_LEX* sel);
    bool join_list(List<TABLE_LIST>* jl);
    bool table_ref(TABLE_LIST* t);
    TABLE_LIST* mergeable_base(TABLE_LIST* t);
    bool condition(Item* it);
    bool node(Item* it);
    bool expr(Item* it);
    bool literal(Item* it);
    bool field(Item_field* f);
    bool func(Item_func* f);
    bool named_func(Item_func* f);
    bool cond(Item_cond* c);
    bool sum(Item_sum* s, bool over_window);
    bool window(Item_window_func* w);
    bool subquery(SELECT_LEX_UNIT* u);
    bool subselect(Item_subselect* s);
    bool in_subquery(Item* left, Item_subselect* s);
    bool in_optimizer(Item_in_optimizer* o);
    bool order_list(SQL_I_List<ORDER>* lst, bool directions);
    bool limit(SELECT_LEX* sel);
    bool branch(Item* it, Cls result);
    bool branches(Item_func* f, Item** args, uint n);
    bool frame_bound(Window_frame_bound* b);
    bool binop(Item* a, const char* op, Item* b);
    bool temporal_literal(Item* col, Item* k, const Type_handler* h,
                          std::string* lit);
    bool compare(Item_func* f, Item** a, const char* op);
    bool args_csv(Item** args, uint n);

    Cls cls(Item* it);
    bool cs_safe(const CHARSET_INFO* cs);
    bool str_cmp_safe(Item* it);
    bool comparable(Item* a, Item* b, const CHARSET_INFO* cmp_cs = nullptr);
    bool orderable(Item* it);
    bool same_family(Item** args, uint n);

    THD* thd_;
    bool trust_bin_;
    std::unordered_map<const TABLE_LIST*, std::string> alias_;
    // Columns of a derived table emitted as a subquery: the items a view
    // reference points at (field_translation) -> "alias"."column".
    std::unordered_map<const Item*, std::string> view_cols_;
    // WHERE conditions of derived tables merged into the current SELECT's
    // FROM without an ON to carry them (see table_ref).
    std::vector<Item*> merged_where_;
    // Set by table_ref for a merged derived table: its WHERE, for the
    // caller's ON.
    Item* merged_on_ = nullptr;
    int next_alias_ = 0;
    // An UPDATE's columns its SET has assigned so far: MariaDB's later
    // values read them assigned, stoolap's read the old row (modify)
    std::vector<const Field*> assigned_;
};

// ---------------------------------------------------------------------------
// Type safety
// ---------------------------------------------------------------------------

Cls Gen::cls(Item* it) {
    Item* r = it->real_item();
    if (r->type() == Item::FIELD_ITEM) {
        const Field* f = static_cast<Item_field*>(r)->field;
        if (!f) return Cls::UNSAFE;
        switch (f->real_type()) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_YEAR:
                return Cls::INT;
            case MYSQL_TYPE_LONGLONG:
                // Stored as i64: values above INT64_MAX do not exist.
                return f->is_unsigned() ? Cls::UNSAFE : Cls::INT;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
                return Cls::REAL;
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB:
                // Binary and non-utf8 strings are stored as hex, INET6 /
                // UUID as their text: neither compares as stoolap TEXT.
                if (stoolap_mariadb::field_enc(f) !=
                        stoolap_mariadb::Enc::PLAIN ||
                    !f->has_charset() || f->charset() == &my_charset_bin)
                    return Cls::UNSAFE;
                return Cls::STR;
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_NEWDATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_DATETIME2:
                return Cls::DATETIME;
            case MYSQL_TYPE_TIME:
            case MYSQL_TYPE_TIME2:
                return Cls::TIME;
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_TIMESTAMP2:
                return Cls::TSTAMP;
            default:
                // DECIMAL (TEXT in stoolap), ENUM / SET (index / mask),
                // BIT, JSON, GEOMETRY.
                return Cls::UNSAFE;
        }
    }
    if (r->type() == Item::SUM_FUNC_ITEM) {
        // SUM of integers is an exact DECIMAL in MariaDB and an integer in
        // stoolap: the same whole number.
        auto* sm = static_cast<Item_sum*>(r);
        if ((sm->sum_func() == Item_sum::SUM_FUNC ||
             sm->sum_func() == Item_sum::SUM_DISTINCT_FUNC) &&
            sm->get_arg_count() == 1 && cls(sm->get_arg(0)) == Cls::INT)
            return Cls::INT;
    }
    switch (it->cmp_type()) {
        case INT_RESULT:
            return it->unsigned_flag ? Cls::UNSAFE : Cls::INT;
        case REAL_RESULT:
            return Cls::REAL;
        case DECIMAL_RESULT:
            // A decimal literal (1.5) means the same as a float literal
            // against INT / DOUBLE values; a DECIMAL expression does not.
            return it->basic_const_item() ? Cls::REAL : Cls::UNSAFE;
        case STRING_RESULT:
            // A binary or non-utf8 result would be read back as hex.
            return stoolap_mariadb::text_is_utf8(it->collation.collation)
                       ? Cls::STR
                       : Cls::UNSAFE;
        default:
            return Cls::UNSAFE;
    }
}

// MariaDB compares strings through their collation: a ci collation
// folds case and accents, and a PAD SPACE one ('_bin' included) ignores
// trailing spaces. Stoolap compares bytes, which only a binary NO PAD
// collation agrees with, unless the session opted into byte semantics.
bool Gen::cs_safe(const CHARSET_INFO* cs) {
    // Other character sets are stored as hex (stoolap_types.h).
    if (!stoolap_mariadb::text_is_utf8(cs)) return false;
    if (trust_bin_) return true;
    return cs && (cs->state & MY_CS_BINSORT) && (cs->state & MY_CS_NOPAD);
}

bool Gen::str_cmp_safe(Item* it) {
    return cs_safe(it->collation.collation);
}

// cmp_cs: the collation MariaDB settled on for the comparison (a
// column's wins over a literal's), when the caller knows it; otherwise
// both operands must be safe on their own.
bool Gen::comparable(Item* a, Item* b, const CHARSET_INFO* cmp_cs) {
    // A NULL literal compares (to NULL / unknown) the same way to anything.
    if (a->type() == Item::NULL_ITEM || b->type() == Item::NULL_ITEM)
        return true;
    const Cls ca = cls(a);
    const Cls cb = cls(b);
    if (numeric(ca) && numeric(cb)) return true;
    if (ca == Cls::STR && cb == Cls::STR)
        return cmp_cs ? cs_safe(cmp_cs) : str_cmp_safe(a) && str_cmp_safe(b);
    // Temporal columns against their own kind: a literal or another kind
    // would need MariaDB's conversion rules.
    return ca == cb && temporal(ca);
}

bool Gen::orderable(Item* it) {
    const Cls c = cls(it);
    if (numeric(c) || temporal(c)) return true;
    return c == Cls::STR && str_cmp_safe(it);
}

// Arguments whose values mix into one result (COALESCE, IF, CASE
// branches): all numeric or all strings. NULL literals fit either.
bool Gen::same_family(Item** args, uint n) {
    bool any_num = false, any_str = false;
    for (uint i = 0; i < n; ++i) {
        if (args[i]->type() == Item::NULL_ITEM) continue;
        const Cls c = cls(args[i]);
        if (numeric(c))
            any_num = true;
        else if (c == Cls::STR)
            any_str = true;
        else
            return false;
    }
    return !(any_num && any_str);
}

// One value of a mixed result (IF / CASE / COALESCE): MariaDB converts it
// to the function's type, stoolap keeps each branch's own, so an integer
// branch of a DOUBLE result is cast (else 3 / 2 divides as integers).
bool Gen::branch(Item* it, Cls result) {
    if (result != Cls::REAL || cls(it) != Cls::INT) return expr(it);
    out += "CAST(";
    if (!expr(it)) return false;
    out += " AS FLOAT)";
    return true;
}

// The branch types must agree, and the result must be a type we map
// (a DECIMAL result prints differently).
bool Gen::branches(Item_func* f, Item** args, uint n) {
    return same_family(args, n) && cls(f) != Cls::UNSAFE;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

// `cols`: a set-operation leg's result column types (see unit()).
bool Gen::select(SELECT_LEX* sel, const std::vector<std::string>* names,
                 const std::vector<Cls>* cols) {
    if (sel->olap != UNSPECIFIED_OLAP_TYPE) return fail("WITH ROLLUP");
    if (sel->tvc) return fail("VALUES");
    const bool any_row = exists_list_;
    exists_list_ = false;
    if (sel->limit_params.with_ties) return fail("WITH TIES");
    if (sel->options & OPTION_FOUND_ROWS) return fail("SQL_CALC_FOUND_ROWS");

    // FROM first, so every table has an alias before any expression
    // (including correlated subqueries below) refers to it.
    std::string head;
    head.swap(out);
    out.reserve(512);
    if (sel->table_list.elements == 0) return fail("no table");
    std::vector<Item*> outer_merged;
    outer_merged.swap(merged_where_);
    const bool from_ok = from(sel);
    std::vector<Item*> where_parts;
    where_parts.swap(merged_where_);
    merged_where_.swap(outer_merged);
    if (!from_ok) return false;
    std::string from_sql;
    from_sql.swap(out);
    out.swap(head);

    // Without aggregates, an EXISTS sees rows whatever the list holds,
    // so it need not be translatable.
    if (any_row && !sel->with_sum_func) {
        out += "SELECT 1";
        out += from_sql;
    } else {
        const bool distinct = sel->options & SELECT_DISTINCT;
        out += distinct ? "SELECT DISTINCT " : "SELECT ";
        List_iterator_fast<Item> it(sel->item_list);
        uint n = 0;
        for (Item* item; (item = it++); ++n) {
            if (distinct && !orderable(item)) return fail("DISTINCT type");
            // The reader decodes a binary or non-utf8 result column from
            // hex: only a stored column of such a type comes back so.
            if (item->cmp_type() == STRING_RESULT &&
                !stoolap_mariadb::text_is_utf8(item->collation.collation) &&
                item->real_item()->type() != Item::FIELD_ITEM)
                return fail("binary / non-utf8 string result");
            if (n) out += ", ";
            if (cols) {
                // A leg's value must already have the result's type, or
                // be an integer that a DOUBLE result widens.
                if (n >= cols->size()) return fail("set operation columns");
                const Cls want = (*cols)[n], have = cls(item);
                if (item->type() != Item::NULL_ITEM && have != want &&
                    !(want == Cls::REAL && have == Cls::INT))
                    return fail("set operation column types");
                // MariaDB dedups a DISTINCT leg's own values, then
                // converts: 2^53 and 2^53 + 1 stay two rows, which a cast
                // inside the DISTINCT would merge.
                if (distinct && want == Cls::REAL && have == Cls::INT)
                    return fail("DISTINCT leg widened to DOUBLE");
                if (!branch(item, want)) return false;
            } else if (!expr(item)) {
                return false;
            }
            // Rows are read by position; names matter only to a derived
            // table's outer query and to a set operation's ORDER BY.
            if (names) {
                if (n >= names->size()) return fail("derived column names");
                out += " AS ";
                ident((*names)[n]);
            } else if (cols) {
                out += " AS ";
                pos_name(n);
            }
        }
        out += from_sql;
    }

    if (sel->where) where_parts.insert(where_parts.begin(), sel->where);
    for (size_t i = 0; i < where_parts.size(); ++i) {
        out += i ? " AND (" : " WHERE (";
        if (!condition(where_parts[i])) return false;
        out.push_back(')');
    }
    if (sel->group_list.elements) {
        out += " GROUP BY ";
        if (!order_list(&sel->group_list, false)) return false;
    }
    if (sel->having) {
        out += " HAVING ";
        if (!condition(sel->having)) return false;
    }
    // MariaDB sorts GROUP BY results by the group keys (in their given
    // directions) unless an ORDER BY says otherwise; ORDER BY NULL or
    // another constant leaves the order free, so the group order fits.
    // Inside a subquery the order only matters under a LIMIT.
    bool const_order = true;
    for (ORDER* o = sel->order_list.first; o; o = o->next)
        if (!o->item || !*o->item || !(*o->item)->const_item())
            const_order = false;
    if (!const_order) {
        out += " ORDER BY ";
        if (!order_list(&sel->order_list, true)) return false;
    } else if (sel->group_list.elements &&
               (sel == top || sel->limit_params.explicit_limit)) {
        out += " ORDER BY ";
        if (!order_list(&sel->group_list, true)) return false;
    }
    return limit(sel);
}

bool Gen::limit(SELECT_LEX* sel) {
    if (!sel->limit_params.explicit_limit) return true;
    Item* lim = sel->limit_params.select_limit;
    Item* off = sel->limit_params.offset_limit;
    if (!lim || !lim->const_item() || lim->is_null())
        return fail("LIMIT value");
    out += " LIMIT " + std::to_string(lim->val_uint());
    if (off) {
        if (!off->const_item() || off->is_null()) return fail("OFFSET value");
        out += " OFFSET " + std::to_string(off->val_uint());
    }
    return true;
}

// UNION / INTERSECT / EXCEPT [ALL] legs in their written order: stoolap
// applies them left to right with INTERSECT first, as MariaDB does.
bool Gen::unit(SELECT_LEX_UNIT* u, const std::vector<std::string>* names) {
    SELECT_LEX* first = u ? u->first_select() : nullptr;
    if (!first) return fail("empty query expression");
    if (!first->next_select()) return select(first, names);
    if (u->with_element && u->with_element->is_recursive)
        return fail("recursive CTE");
    exists_list_ = false;  // a leg's list decides what the others dedup

    // The result columns' types (MariaDB's aggregate over the legs); a
    // DISTINCT, INTERSECT or EXCEPT compares rows by them.
    bool dedup = false;
    for (SELECT_LEX* sl = first->next_select(); sl; sl = sl->next_select())
        if (sl->distinct || sl->get_linkage() != UNION_TYPE) dedup = true;
    std::vector<Cls> cols;
    List_iterator_fast<Item> ti(u->types);
    for (Item* t; (t = ti++);) {
        const Cls c = cls(t);
        if (c == Cls::UNSAFE) return fail("set operation column type");
        if (dedup && !orderable(t)) return fail("set operation compare type");
        cols.push_back(c);
    }

    for (SELECT_LEX* sl = first; sl; sl = sl->next_select()) {
        if (sl != first) {
            switch (sl->get_linkage()) {
                case UNION_TYPE:
                    out += " UNION ";
                    break;
                case INTERSECT_TYPE:
                    out += " INTERSECT ";
                    break;
                case EXCEPT_TYPE:
                    out += " EXCEPT ";
                    break;
                default:
                    return fail("set operation");
            }
            if (!sl->distinct) out += "ALL ";
        }
        // A leg's own ORDER BY / LIMIT needs parentheses stoolap lacks.
        if (sl->order_list.elements || sl->limit_params.explicit_limit)
            return fail("set operation leg ORDER BY / LIMIT");
        if (sl->item_list.elements != cols.size())
            return fail("set operation columns");
        if (!select(sl, sl == first ? names : nullptr, &cols)) return false;
    }

    // ORDER BY / LIMIT of the whole expression: its items are columns of
    // the result table, written by position.
    SELECT_LEX* g = u->global_parameters();
    if (!g) return true;
    if (g->limit_params.with_ties) return fail("WITH TIES");
    bool first_key = true;
    for (ORDER* o = g->order_list.first; o; o = o->next) {
        Item* it = (o->item && *o->item) ? (*o->item)->real_item() : nullptr;
        if (!it || it->type() != Item::FIELD_ITEM) return fail("order item");
        const Field* f = static_cast<Item_field*>(it)->field;
        if (!f || !u->table || f->table != u->table ||
            u->table->s->fields < cols.size())
            return fail("set operation ORDER BY");
        // INTERSECT / EXCEPT keep hidden counter columns in front.
        const uint hidden = u->table->s->fields - cols.size();
        if (f->field_index < hidden || f->field_index - hidden >= cols.size())
            return fail("set operation ORDER BY");
        if (!orderable(it)) return fail("ORDER / GROUP type");
        out += first_key ? " ORDER BY " : ", ";
        first_key = false;
        const uint i = f->field_index - hidden;
        if (names && i >= names->size()) return fail("derived column names");
        if (names)
            ident((*names)[i]);
        else
            pos_name(i);
        out += o->direction == ORDER::ORDER_DESC ? " DESC NULLS LAST"
                                                 : " ASC NULLS FIRST";
    }
    return limit(g);
}

bool Gen::from(SELECT_LEX* sel) {
    out += " FROM ";
    return join_list(&sel->top_join_list);
}

// MariaDB keeps a join list in reverse order (push_front while parsing);
// print_join() walks it backwards too. Prepare has already turned RIGHT
// JOIN into LEFT JOIN with swapped operands.
bool Gen::join_list(List<TABLE_LIST>* jl) {
    std::vector<TABLE_LIST*> tables;
    List_iterator_fast<TABLE_LIST> it(*jl);
    for (TABLE_LIST* t; (t = it++);)
        tables.push_back(t);
    if (tables.empty()) return fail("empty join list");
    for (size_t i = tables.size(); i-- > 0;) {
        TABLE_LIST* t = tables[i];
        if (t->sj_subq_pred) return fail("semi-join nest");
        const bool first = (i == tables.size() - 1);
        const bool outer = t->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT);
        if (!first) {
            if (outer)
                out += " LEFT JOIN ";
            else if (t->on_expr)
                out += " JOIN ";
            else
                out += " CROSS JOIN ";
        } else if (outer) {
            return fail("outer join on first table");
        }
        if (first && t->nested_join && !t->derived && !t->on_expr && !outer) {
            // Stoolap has no parenthesized join groups. A group that
            // opens the FROM and has no ON of its own means the same
            // without them.
            if (!join_list(&t->nested_join->join_list)) return false;
            continue;
        }
        merged_on_ = nullptr;
        if (!table_ref(t)) return false;
        Item* merged = merged_on_;
        merged_on_ = nullptr;
        if (t->on_expr) {
            if (first) return fail("ON on first table");
            out += " ON (";
            if (!condition(t->on_expr)) return false;
            out.push_back(')');
            // A merged derived table's WHERE filters its rows before the
            // join, which in an ON (outer joins included) means the same.
            if (merged) {
                out += " AND (";
                if (!condition(merged)) return false;
                out.push_back(')');
            }
        } else if (outer) {
            return fail("outer join without ON");
        } else if (merged) {
            merged_where_.push_back(merged);
        }
    }
    return true;
}

// A derived table (CTE, view) MariaDB would merge, over one base table:
// that table, else nullptr. Written as the table itself, its WHERE moved
// to the ON or the outer WHERE, and its columns as their expressions, it
// lets stoolap join and stop at a LIMIT instead of materializing.
TABLE_LIST* Gen::mergeable_base(TABLE_LIST* t) {
    SELECT_LEX_UNIT* u = t->derived;
    SELECT_LEX* d = u ? u->first_select() : nullptr;
    if (!d || !d->is_mergeable() || d->have_window_funcs() || d->tvc ||
        d->olap != UNSPECIFIED_OLAP_TYPE ||
        (d->uncacheable & UNCACHEABLE_RAND) ||
        (u->with_element && u->with_element->is_recursive))
        return nullptr;
    if (d->top_join_list.elements != 1) return nullptr;
    TABLE_LIST* b = d->top_join_list.head();
    if (!b || b->derived || b->nested_join || b->on_expr ||
        (b->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT)) || !b->table ||
        !b->table->s)
        return nullptr;
    // On the inner side of an outer join a NULL-complemented row must
    // read NULL in every column: true of b's own columns, not of a
    // constant or an expression like COALESCE(x, 0).
    const bool outer_inner =
        (t->outer_join & (JOIN_TYPE_LEFT | JOIN_TYPE_RIGHT)) ||
        (t->embedding && t->embedding->outer_join);
    if (outer_inner) {
        List_iterator_fast<Item> it(d->item_list);
        for (Item* item; (item = it++);)
            if (item->real_item()->type() != Item::FIELD_ITEM) return nullptr;
    }
    // The same unit reached twice (a CTE used twice shares it) would give
    // one table two aliases.
    if (alias_.count(b)) return nullptr;
    return b;
}

bool Gen::table_ref(TABLE_LIST* t) {
    if (t->derived) {
        if (TABLE_LIST* b = mergeable_base(t)) {
            if (!table_ref(b)) return false;
            merged_on_ = t->derived->first_select()->where;
            return true;
        }
        // A derived table (or CTE / view) is written as a subquery from
        // its unit. A mergeable one is only merged into this select by
        // JOIN::optimize(), which a pushed select skips, so its unit
        // still holds its own WHERE here, and outer references to its
        // columns are view references into field_translation.
        SELECT_LEX_UNIT* u = t->derived;
        const std::string alias = "d" + std::to_string(next_alias_++);
        alias_[t] = alias;
        std::vector<std::string> names;
        if (t->field_translation) {
            for (Field_translator* ft = t->field_translation;
                 ft < t->field_translation_end; ++ft) {
                names.emplace_back(ft->name.str, ft->name.length);
                view_cols_[ft->item] = alias + "." + quote_ident(names.back());
            }
        } else if (t->table) {
            for (uint i = 0; i < t->table->s->fields; ++i) {
                const Field* f = t->table->field[i];
                names.emplace_back(f->field_name.str, f->field_name.length);
            }
        } else {
            return fail("derived without columns");
        }
        out.push_back('(');
        if (!unit(u, &names)) return false;
        out += ") AS " + alias;
        return true;
    }
    if (t->nested_join) return fail("parenthesized join");
    if (!t->table || !t->table->s) return fail("table without share");
    const std::string alias = "t" + std::to_string(next_alias_++);
    alias_[t] = alias;
    ident(stoolap_table_name(t->table->s));
    out += " AS " + alias;
    return true;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

// WHERE / HAVING / ON / AND / OR / NOT operands: MariaDB tests any value
// for truth (0.5 is true, 'abc' is false); stoolap only accepts booleans
// there, so non-boolean operands are refused.
bool Gen::condition(Item* it) {
    if (!it->type_handler()->is_bool_type())
        return fail("non-boolean condition");
    return node(it);
}

// A value. A boolean expression (a comparison, AND, IN, ...) is the
// integer 1 or 0 to MariaDB wherever it is used as one, in arithmetic, a
// comparison, a CASE branch, a select list; stoolap's BOOLEAN is not a
// number (TRUE + 1 is NULL, TRUE = 1 is false), so it is cast. As a
// condition it stays as it is (condition()).
bool Gen::expr(Item* it) {
    if (it->type() == Item::FIELD_ITEM || !it->type_handler()->is_bool_type())
        return node(it);
    out += "CAST(";
    if (!node(it)) return false;
    out += " AS INTEGER)";
    return true;
}

bool Gen::node(Item* it) {
    switch (it->type()) {
        case Item::FIELD_ITEM:
            return field(static_cast<Item_field*>(it));
        case Item::REF_ITEM: {
            auto* r = static_cast<Item_ref*>(it);
            switch (r->ref_type()) {
                case Item_ref::REF:
                case Item_ref::DIRECT_REF:
                case Item_ref::VIEW_REF:
                case Item_ref::OUTER_REF:
                case Item_ref::AGGREGATE_REF:
                    break;
                default:
                    return fail("reference type");
            }
            if (!r->ref || !*r->ref) return fail("dangling reference");
            if (r->ref_type() == Item_ref::VIEW_REF) {
                auto v = view_cols_.find(*r->ref);
                if (v != view_cols_.end()) {
                    out += v->second;
                    return true;
                }
            }
            return expr(*r->ref);
        }
        case Item::CONST_ITEM:
        case Item::NULL_ITEM:
        case Item::PARAM_ITEM:
            return literal(it);
        case Item::FUNC_ITEM:
            return func(static_cast<Item_func*>(it));
        case Item::COND_ITEM:
            return cond(static_cast<Item_cond*>(it));
        case Item::SUM_FUNC_ITEM:
            return sum(static_cast<Item_sum*>(it), false);
        case Item::WINDOW_FUNC_ITEM:
            return window(static_cast<Item_window_func*>(it));
        case Item::SUBSELECT_ITEM:
            return subselect(static_cast<Item_subselect*>(it));
        default:
            return fail("item type");
    }
}

bool Gen::literal(Item* it) {
    if (!it->const_item()) return fail("non-constant literal");
    if (it->is_null()) {
        out += "NULL";
        return true;
    }
    switch (it->cmp_type()) {
        case INT_RESULT: {
            const longlong v = it->val_int();
            if (it->unsigned_flag && v < 0) return fail("unsigned literal");
            out += std::to_string(v);
            return true;
        }
        case REAL_RESULT: {
            const double d = it->val_real();
            if (!std::isfinite(d)) return fail("non-finite literal");
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            out += buf;
            if (!std::strpbrk(buf, ".eE")) out += ".0";
            return true;
        }
        case DECIMAL_RESULT: {
            my_decimal tmp;
            const my_decimal* d = it->val_decimal(&tmp);
            if (!d) return fail("decimal literal");
            String s;
            if (!d->to_string(&s)) return fail("decimal literal");
            // Stoolap reads a decimal literal as a double: past 15
            // significant digits it would not give the same number back.
            int digits = 0;
            bool leading = true;
            for (size_t i = 0; i < s.length(); ++i) {
                const char c = s.ptr()[i];
                if (c < '0' || c > '9') continue;
                if (leading && c == '0') continue;
                leading = false;
                ++digits;
            }
            if (digits > 15) return fail("decimal literal past a double");
            out.append(s.ptr(), s.length());
            if (!std::memchr(s.ptr(), '.', s.length())) out += ".0";
            return true;
        }
        case STRING_RESULT: {
            String tmp;
            const String* s = it->val_str(&tmp);
            if (!s) return fail("string literal");
            out.push_back('\'');
            for (size_t i = 0; i < s->length(); ++i) {
                const char c = s->ptr()[i];
                // Stoolap's lexer keeps some backslash sequences and
                // processes others; keep literals free of both cases.
                if (c == '\\' || c == '\0') return fail("string escape");
                if (c == '\'') out.push_back('\'');
                out.push_back(c);
            }
            out.push_back('\'');
            return true;
        }
        default:
            // Temporal literals and rows need MariaDB's conversions.
            return fail("literal type");
    }
}

bool Gen::field(Item_field* f) {
    if (!f->field || !f->field->table) return fail("unbound field");
    for (const Field* a : assigned_)
        if (a == f->field) return fail("SET reads a column it assigned");
    const TABLE_LIST* tl = f->field->table->pos_in_table_list;
    auto a = alias_.find(tl);
    if (a == alias_.end()) return fail("field of unknown table");
    out += a->second;
    out.push_back('.');
    ident(std::string_view(f->field->field_name.str,
                           f->field->field_name.length));
    return true;
}

// A DATE / DATETIME or TIME column against a constant string, compared
// the way MariaDB's comparator would (its compare type decides: a DATE
// column against '2024-02-29 10:00' compares as DATETIME): the string
// parsed with the comparison flags and packed like the column
// (stoolap_types.h). Strings that parse with a warning, or to a date
// with a zero part (sql_mode decides those), are left to MariaDB.
// h is null for BETWEEN, whose comparator MariaDB does not expose: the
// column's kind decides there, as it does in MariaDB for a string bound
// (a DATE BETWEEN times of day also compares as DATETIME).
bool Gen::temporal_literal(Item* col, Item* k, const Type_handler* h,
                           std::string* lit) {
    const Cls c = cls(col);
    if (c != Cls::DATETIME && c != Cls::TIME) return false;
    if (!k->basic_const_item() || k->is_null()) return false;
    bool want_time = c == Cls::TIME;
    if (h) {
        switch (h->field_type()) {
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_NEWDATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_DATETIME2:
                want_time = false;
                break;
            case MYSQL_TYPE_TIME:
            case MYSQL_TYPE_TIME2:
                want_time = true;
                break;
            default:
                return false;
        }
    }
    if (want_time != (c == Cls::TIME)) return false;
    MYSQL_TIME mt;
    if (k->cmp_type() == TIME_RESULT) {
        // Of the column's own kind: MariaDB compares a TIME column with a
        // DATETIME bound (or a DATE one with a TIME bound) as DATETIME.
        switch (k->field_type()) {
            case MYSQL_TYPE_TIME:
            case MYSQL_TYPE_TIME2:
                if (!want_time) return false;
                break;
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_NEWDATE:
            case MYSQL_TYPE_DATETIME:
            case MYSQL_TYPE_DATETIME2:
                if (want_time) return false;
                break;
            default:
                return false;
        }
        // A temporal literal MariaDB made while preparing (BETWEEN turns
        // its string bounds into these): its packed DATETIME / TIME, the
        // value the comparator reads, is already pack_time()'s.
        const longlong packed =
            want_time ? k->val_time_packed(thd_) : k->val_datetime_packed(thd_);
        if (k->null_value) return false;
        unpack_time(
            packed, &mt,
            want_time ? MYSQL_TIMESTAMP_TIME : MYSQL_TIMESTAMP_DATETIME);
        if (!want_time && (!mt.year || !mt.month || !mt.day)) return false;
        *lit = std::to_string(static_cast<long long>(packed));
        return true;
    }
    if (k->cmp_type() != STRING_RESULT) return false;
    char buf[80];
    String tmp(buf, sizeof(buf), &my_charset_bin);
    const String* s = k->val_str(&tmp);
    if (!s || !s->charset() || s->charset()->mbminlen != 1) return false;
    MYSQL_TIME_STATUS st;
    my_time_status_init(&st);
    if (want_time) {
        if (str_to_datetime_or_date_or_time(s->ptr(), s->length(), &mt,
                                            C_TIME_INVALID_DATES, &st,
                                            TIME_MAX_HOUR, UINT_MAX32) ||
            mt.time_type != MYSQL_TIMESTAMP_TIME)
            return false;
    } else {
        if (str_to_datetime_or_date(s->ptr(), s->length(), &mt,
                                    C_TIME_INVALID_DATES, &st) ||
            !mt.year || !mt.month || !mt.day)
            return false;
    }
    if (st.warnings || st.nanoseconds) return false;
    *lit = std::to_string(static_cast<long long>(pack_time(&mt)));
    return true;
}

bool Gen::compare(Item_func* f, Item** a, const char* op) {
    // =, <>, <, <=, >, >= and <=> are all Item_bool_rowready_func2.
    auto* bf = static_cast<Item_bool_func2*>(f);
    std::string lit;
    if (temporal_literal(a[0], a[1], bf->compare_type_handler(), &lit)) {
        out.push_back('(');
        if (!expr(a[0])) return false;
        out += op;
        out += lit;
        out.push_back(')');
        return true;
    }
    if (temporal_literal(a[1], a[0], bf->compare_type_handler(), &lit)) {
        out.push_back('(');
        out += lit;
        out += op;
        if (!expr(a[1])) return false;
        out.push_back(')');
        return true;
    }
    if (!comparable(a[0], a[1], bf->compare_collation()))
        return fail("comparison");
    return binop(a[0], op, a[1]);
}

bool Gen::binop(Item* a, const char* op, Item* b) {
    out.push_back('(');
    if (!expr(a)) return false;
    out += op;
    if (!expr(b)) return false;
    out.push_back(')');
    return true;
}

bool Gen::args_csv(Item** args, uint n) {
    for (uint i = 0; i < n; ++i) {
        if (i) out += ", ";
        if (!expr(args[i])) return false;
    }
    return true;
}

bool Gen::cond(Item_cond* c) {
    const char* sep;
    switch (c->functype()) {
        case Item_func::COND_AND_FUNC:
            sep = " AND ";
            break;
        case Item_func::COND_OR_FUNC:
            sep = " OR ";
            break;
        default:
            return fail("condition type");
    }
    List_iterator_fast<Item> it(*c->argument_list());
    out.push_back('(');
    bool first = true;
    for (Item* arg; (arg = it++); first = false) {
        if (!first) out += sep;
        if (!condition(arg)) return false;
    }
    out.push_back(')');
    return !first || fail("empty condition");
}

bool Gen::func(Item_func* f) {
    // A sql_mode variant (sql_mode=ORACLE's CONCAT skips NULLs, its DECODE
    // matches NULL to NULL) keeps the plain function's name and functype;
    // only its schema tells it apart. The plain one has none, or MariaDB's.
    const Schema* schema = f->schema();
    if (schema && schema != &mariadb_schema)
        return fail("sql_mode function variant");
    Item** a = f->arguments();
    const uint n = f->argument_count();
    switch (f->functype()) {
        case Item_func::EQ_FUNC:
        case Item_func::NE_FUNC:
        case Item_func::LT_FUNC:
        case Item_func::LE_FUNC:
        case Item_func::GT_FUNC:
        case Item_func::GE_FUNC: {
            static const char* const ops[] = {" = ",  " <> ", " < ",
                                              " <= ", " > ",  " >= "};
            const int i = f->functype() == Item_func::EQ_FUNC   ? 0
                          : f->functype() == Item_func::NE_FUNC ? 1
                          : f->functype() == Item_func::LT_FUNC ? 2
                          : f->functype() == Item_func::LE_FUNC ? 3
                          : f->functype() == Item_func::GT_FUNC ? 4
                                                                : 5;
            if (n != 2) return fail("comparison");
            return compare(f, a, ops[i]);
        }
        case Item_func::EQUAL_FUNC:  // <=>
            if (n != 2) return fail("<=>");
            return compare(f, a, " IS NOT DISTINCT FROM ");
        case Item_func::ISNULL_FUNC:
        case Item_func::ISNOTNULL_FUNC:
            if (n != 1) return fail("IS NULL");
            out.push_back('(');
            if (!expr(a[0])) return false;
            out += f->functype() == Item_func::ISNULL_FUNC ? " IS NULL)"
                                                           : " IS NOT NULL)";
            return true;
        case Item_func::NOT_FUNC:
            if (n != 1) return fail("NOT");
            out += "(NOT ";
            if (!condition(a[0])) return false;
            out.push_back(')');
            return true;
        case Item_func::BETWEEN: {
            auto* b = static_cast<Item_func_between*>(f);
            const CHARSET_INFO* cc = b->compare_collation();
            std::string lo, hi;
            if (n == 3 && temporal_literal(a[0], a[1], nullptr, &lo) &&
                temporal_literal(a[0], a[2], nullptr, &hi)) {
                out.push_back('(');
                if (!expr(a[0])) return false;
                out += b->negated ? " NOT BETWEEN " : " BETWEEN ";
                out += lo + " AND " + hi + ")";
                return true;
            }
            if (n != 3 || !comparable(a[0], a[1], cc) ||
                !comparable(a[0], a[2], cc))
                return fail("BETWEEN");
            out.push_back('(');
            if (!expr(a[0])) return false;
            out += b->negated ? " NOT BETWEEN " : " BETWEEN ";
            if (!expr(a[1])) return false;
            out += " AND ";
            if (!expr(a[2])) return false;
            out.push_back(')');
            return true;
        }
        case Item_func::IN_FUNC: {
            auto* in = static_cast<Item_func_in*>(f);
            if (n < 2 || a[0]->cols() != 1) return fail("IN");
            for (uint i = 1; i < n; ++i)
                if (!comparable(a[0], a[i], in->compare_collation()))
                    return fail("IN value");
            out.push_back('(');
            if (!expr(a[0])) return false;
            out += in->negated ? " NOT IN (" : " IN (";
            if (!args_csv(a + 1, n - 1)) return false;
            out += "))";
            return true;
        }
        case Item_func::LIKE_FUNC: {
            auto* l = static_cast<Item_func_like*>(f);
            // LIKE matches characters without padding, so a binary
            // collation agrees with stoolap; ci ones fold case.
            const CHARSET_INFO* cs = l->compare_collation();
            if (n != 2 || cls(a[0]) != Cls::STR || l->escape != '\\' ||
                !(trust_bin_ || (cs && (cs->state & MY_CS_BINSORT))))
                return fail("LIKE");
            if (!a[1]->basic_const_item() || cls(a[1]) != Cls::STR)
                return fail("LIKE pattern");
            out.push_back('(');
            if (!expr(a[0])) return false;
            out += l->negated ? " NOT LIKE " : " LIKE ";
            // literal() refuses backslashes, so no escape is in play.
            if (!literal(a[1])) return false;
            out.push_back(')');
            return true;
        }
        case Item_func::IN_OPTIMIZER_FUNC:
            return in_optimizer(static_cast<Item_in_optimizer*>(f));
        case Item_func::NEG_FUNC:
            if (n != 1 || !numeric(cls(a[0])) || cls(f) == Cls::UNSAFE)
                return fail("unary minus");
            out += "(-";
            if (!expr(a[0])) return false;
            out.push_back(')');
            return true;
        case Item_func::CASE_SEARCHED_FUNC: {
            // args: when1..whenN, then1..thenN, [else]
            const uint whens = n / 2;
            const bool with_else = n % 2;
            if (!branches(f, a + whens, n - whens))
                return fail("CASE result types");
            const Cls rc = cls(f);
            out += "(CASE";
            for (uint i = 0; i < whens; ++i) {
                out += " WHEN ";
                if (!condition(a[i])) return false;
                out += " THEN ";
                if (!branch(a[whens + i], rc)) return false;
            }
            if (with_else) {
                out += " ELSE ";
                if (!branch(a[n - 1], rc)) return false;
            }
            out += " END)";
            return true;
        }
        case Item_func::CASE_SIMPLE_FUNC: {
            // args: expr, when1..whenN, then1..thenN, [else]
            const uint whens = (n - 1) / 2;
            const bool with_else = (n % 2) == 0;
            const CHARSET_INFO* cc =
                static_cast<Item_func_case*>(f)->compare_collation();
            for (uint i = 0; i < whens; ++i)
                if (!comparable(a[0], a[1 + i], cc))
                    return fail("CASE value types");
            if (!branches(f, a + 1 + whens, n - 1 - whens))
                return fail("CASE result types");
            const Cls rc = cls(f);
            out += "(CASE ";
            if (!expr(a[0])) return false;
            for (uint i = 0; i < whens; ++i) {
                out += " WHEN ";
                if (!expr(a[1 + i])) return false;
                out += " THEN ";
                if (!branch(a[1 + whens + i], rc)) return false;
            }
            if (with_else) {
                out += " ELSE ";
                if (!branch(a[n - 1], rc)) return false;
            }
            out += " END)";
            return true;
        }
        default:
            return named_func(f);
    }
}

bool Gen::named_func(Item_func* f) {
    Item** a = f->arguments();
    const uint n = f->argument_count();
    const std::string_view name = f->func_name();

    if (name == "+" || name == "-" || name == "*" || name == "/") {
        // Integer and float arithmetic only: '/' on integers and anything
        // involving DECIMAL give MariaDB an exact DECIMAL result.
        const Cls rc = cls(f);
        if (n != 2 || !numeric(cls(a[0])) || !numeric(cls(a[1])) ||
            !numeric(rc) || (name == "/" && rc != Cls::REAL))
            return fail("arithmetic");
        const std::string op = " " + std::string(name) + " ";
        return binop(a[0], op.c_str(), a[1]);
    }
    if (name == "abs") {
        if (n != 1 || !numeric(cls(a[0])) || !numeric(cls(f)))
            return fail("ABS");
        out += "ABS(";
        if (!expr(a[0])) return false;
        out.push_back(')');
        return true;
    }
    if (name == "coalesce" || name == "ifnull") {
        if (n < 1 || !branches(f, a, n)) return fail("COALESCE types");
        out += name == "coalesce" ? "COALESCE(" : "IFNULL(";
        for (uint i = 0; i < n; ++i) {
            if (i) out += ", ";
            if (!branch(a[i], cls(f))) return false;
        }
        out.push_back(')');
        return true;
    }
    if (name == "nullif") {
        // Item_func_nullif carries a third argument, an internal copy of
        // the first one; only the first two are the SQL arguments.
        if ((n != 2 && n != 3) || !comparable(a[0], a[1]))
            return fail("NULLIF");
        out += "NULLIF(";
        if (!args_csv(a, 2)) return false;
        out.push_back(')');
        return true;
    }
    if (name == "if") {
        if (n != 3 || !branches(f, a + 1, 2)) return fail("IF");
        out += "(CASE WHEN ";
        if (!condition(a[0])) return false;
        out += " THEN ";
        if (!branch(a[1], cls(f))) return false;
        out += " ELSE ";
        if (!branch(a[2], cls(f))) return false;
        out += " END)";
        return true;
    }
    if (name == "char_length" || name == "character_length") {
        // Stoolap's LENGTH counts characters, like CHAR_LENGTH. (MariaDB's
        // LENGTH counts bytes and has no stoolap counterpart.)
        if (n != 1 || cls(a[0]) != Cls::STR) return fail("CHAR_LENGTH");
        out += "LENGTH(";
        if (!expr(a[0])) return false;
        out.push_back(')');
        return true;
    }
    if (name == "concat") {
        for (uint i = 0; i < n; ++i)
            if (cls(a[i]) != Cls::STR) return fail("CONCAT argument");
        // Stoolap's CONCAT skips NULLs; its || returns NULL, as CONCAT
        // does in MariaDB.
        out.push_back('(');
        for (uint i = 0; i < n; ++i) {
            if (i) out += " || ";
            if (!expr(a[i])) return false;
        }
        out.push_back(')');
        return true;
    }
    // Not translated: TRIM (stoolap strips all whitespace, MariaDB only
    // spaces) and UPPER / LOWER (MariaDB cases by the collation).
    return fail("function");
}

// IN / EXISTS subqueries reach us wrapped: <in_optimizer>(left, subq) for
// IN, <in_optimizer>(1, exists(...)) for EXISTS. A subquery MariaDB
// already rewrote (IN-to-EXISTS) carries trigger conditions, which
// expr() refuses, so only untransformed ones get through.
bool Gen::in_optimizer(Item_in_optimizer* o) {
    Item** a = o->arguments();
    if (o->argument_count() != 2 || a[1]->type() != Item::SUBSELECT_ITEM)
        return fail("in_optimizer shape");
    auto* s = static_cast<Item_subselect*>(a[1]);
    switch (s->substype()) {
        case Item_subselect::EXISTS_SUBS:
            out += "(EXISTS (";
            exists_list_ = true;
            if (!subquery(s->unit)) return false;
            out += "))";
            return true;
        case Item_subselect::IN_SUBS:
            // The optimizer's first argument is the IN's left operand.
            return in_subquery(a[0], s);
        default:
            return fail("ANY / ALL subquery");
    }
}

bool Gen::subselect(Item_subselect* s) {
    switch (s->substype()) {
        case Item_subselect::SINGLEROW_SUBS:
            out.push_back('(');
            if (!subquery(s->unit)) return false;
            out.push_back(')');
            return true;
        case Item_subselect::EXISTS_SUBS:
            out += "(EXISTS (";
            exists_list_ = true;
            if (!subquery(s->unit)) return false;
            out += "))";
            return true;
        case Item_subselect::IN_SUBS:
            // Not wrapped in <in_optimizer> yet: that happens in
            // JOIN::optimize(), which a pushed select skips.
            return in_subquery(static_cast<Item_in_subselect*>(s)->left_exp(),
                               s);
        default:
            return fail("ANY / ALL subquery");
    }
}

bool Gen::in_subquery(Item* left, Item_subselect* s) {
    if (!left || left->cols() != 1) return fail("IN subquery left");
    SELECT_LEX* inner = s->unit ? s->unit->first_select() : nullptr;
    if (!inner) return fail("IN subquery columns");
    for (SELECT_LEX* sl = inner; sl; sl = sl->next_select()) {
        if (sl->item_list.elements != 1) return fail("IN subquery columns");
        if (!comparable(left, sl->item_list.head()))
            return fail("IN subquery types");
    }
    out.push_back('(');
    if (!expr(left)) return false;
    out += " IN (";
    if (!subquery(s->unit)) return false;
    out += "))";
    return true;
}

bool Gen::subquery(SELECT_LEX_UNIT* u) {
    std::string saved;
    saved.swap(out);
    if (!unit(u, nullptr)) return false;
    saved += out;
    out.swap(saved);
    return true;
}

bool Gen::sum(Item_sum* s, bool over_window) {
    Item** a = s->get_args();
    const uint n = s->get_arg_count();
    switch (s->sum_func()) {
        case Item_sum::COUNT_FUNC:
            if (n != 1) return fail("COUNT arity");
            // COUNT(*) is COUNT(0) after parsing.
            if (a[0]->basic_const_item() && !a[0]->is_null()) {
                out += "COUNT(*)";
                return true;
            }
            out += "COUNT(";
            if (!expr(a[0])) return false;
            out.push_back(')');
            return true;
        case Item_sum::COUNT_DISTINCT_FUNC:
            if (over_window || n != 1 || !orderable(a[0]))
                return fail("COUNT DISTINCT");
            out += "COUNT(DISTINCT ";
            if (!expr(a[0])) return false;
            out.push_back(')');
            return true;
        case Item_sum::SUM_FUNC:
        case Item_sum::SUM_DISTINCT_FUNC:
            if (n != 1 || !numeric(cls(a[0]))) return fail("SUM");
            if (over_window && s->sum_func() == Item_sum::SUM_DISTINCT_FUNC)
                return fail("SUM DISTINCT window");
            out +=
                s->sum_func() == Item_sum::SUM_FUNC ? "SUM(" : "SUM(DISTINCT ";
            if (!expr(a[0])) return false;
            out.push_back(')');
            return true;
        case Item_sum::AVG_FUNC:
            // AVG of integers is an exact DECIMAL in MariaDB, a double in
            // stoolap; over doubles both are doubles.
            if (n != 1 || cls(a[0]) != Cls::REAL) return fail("AVG");
            out += "AVG(";
            if (!expr(a[0])) return false;
            out.push_back(')');
            return true;
        case Item_sum::MIN_FUNC:
        case Item_sum::MAX_FUNC:
            if (n != 1 || !orderable(a[0])) return fail("MIN / MAX");
            out += s->sum_func() == Item_sum::MIN_FUNC ? "MIN(" : "MAX(";
            if (!expr(a[0])) return false;
            out.push_back(')');
            return true;
        default:
            return fail("aggregate");
    }
}

bool Gen::frame_bound(Window_frame_bound* b) {
    if (!b) return fail("frame bound");
    if (b->precedence_type == Window_frame_bound::CURRENT) {
        out += "CURRENT ROW";
        return true;
    }
    if (!b->offset) {
        out += "UNBOUNDED";
    } else {
        if (b->offset->cmp_type() != INT_RESULT || !b->offset->const_item())
            return fail("frame offset");
        const longlong v = b->offset->val_int();
        if (v < 0) return fail("frame offset");
        out += std::to_string(v);
    }
    out += b->precedence_type == Window_frame_bound::PRECEDING ? " PRECEDING"
                                                               : " FOLLOWING";
    return true;
}

bool Gen::window(Item_window_func* w) {
    Item_sum* f = w->window_func();
    Window_spec* spec = w->window_spec;
    if (!f || !spec) return fail("window");
    bool ranking = true;
    switch (f->sum_func()) {
        case Item_sum::ROW_NUMBER_FUNC:
            out += "ROW_NUMBER()";
            break;
        case Item_sum::RANK_FUNC:
            out += "RANK()";
            break;
        case Item_sum::DENSE_RANK_FUNC:
            out += "DENSE_RANK()";
            break;
        case Item_sum::LAG_FUNC:
        case Item_sum::LEAD_FUNC: {
            const uint n = f->get_arg_count();
            if (n < 1 || n > 3) return fail("LAG / LEAD arity");
            out += f->sum_func() == Item_sum::LAG_FUNC ? "LAG(" : "LEAD(";
            if (!args_csv(f->get_args(), n)) return false;
            out.push_back(')');
            break;
        }
        default:
            // Aggregates over a window: only with an explicit ROWS frame
            // (the default frames' tie handling is not checked yet).
            ranking = false;
            if (!spec->window_frame) return fail("window default frame");
            if (!sum(f, /*over_window=*/true)) return false;
            break;
    }
    out += " OVER (";
    bool any = false;
    if (spec->partition_list && spec->partition_list->elements) {
        out += "PARTITION BY ";
        if (!order_list(spec->partition_list, false)) return false;
        any = true;
    }
    if (spec->order_list && spec->order_list->elements) {
        if (any) out.push_back(' ');
        out += "ORDER BY ";
        if (!order_list(spec->order_list, true)) return false;
        any = true;
    }
    if (!ranking) {
        Window_frame* fr = spec->window_frame;
        if (fr->units != Window_frame::UNITS_ROWS ||
            fr->exclusion != Window_frame::EXCL_NONE)
            return fail("window frame");
        if (any) out.push_back(' ');
        out += "ROWS BETWEEN ";
        if (!frame_bound(fr->top_bound)) return false;
        out += " AND ";
        if (!frame_bound(fr->bottom_bound)) return false;
    }
    out.push_back(')');
    return true;
}

// GROUP BY / PARTITION BY (directions == false) and ORDER BY lists. Every
// key must order the same way on both sides; NULLs go first ascending
// and last descending, as in MariaDB (stoolap's default is the reverse).
bool Gen::order_list(SQL_I_List<ORDER>* lst, bool directions) {
    bool first = true;
    for (ORDER* o = lst->first; o; o = o->next, first = false) {
        if (!o->item || !*o->item) return fail("order item");
        Item* it = *o->item;
        if (!orderable(it)) return fail("ORDER / GROUP type");
        if (!first) out += ", ";
        if (!expr(it)) return false;
        if (directions) {
            out += o->direction == ORDER::ORDER_DESC ? " DESC NULLS LAST"
                                                     : " ASC NULLS FIRST";
        }
    }
    return true;
}

// A single-table UPDATE / DELETE. Stoolap's UPDATE takes no alias, so
// the table's own name qualifies its columns.
bool Gen::modify(SELECT_LEX* sel, Item* where) {
    TABLE_LIST* t = sel->get_table_list();
    if (!t || t->next_local || t->derived || t->nested_join || !t->table ||
        !t->table->s)
        return fail("modified table");
    const std::string name = quote_ident(stoolap_table_name(t->table->s));
    alias_[t] = name;
    if (thd_->lex->sql_command == SQLCOM_UPDATE) {
        out += "UPDATE " + name + " SET ";
        List_iterator<Item> fit(sel->item_list);
        List_iterator<Item> vit(thd_->lex->value_list);
        Item* f;
        Item* v;
        bool first = true;
        while ((f = fit++)) {
            if (!(v = vit++)) return fail("SET without a value");
            if (f->type() != Item::FIELD_ITEM) return fail("SET target");
            const Field* fl = static_cast<Item_field*>(f)->field;
            if (!fl || fl->table != t->table) return fail("SET target");
            // MariaDB converts the value to the column (Field::store),
            // stoolap by its own rules: 1.20 into a VARCHAR is '1.20' to
            // one and '1.2' to the other, 2.7 into an INT 3 and 2. Only a
            // value of the column's class (an integer into a float) or a
            // NULL is written the same.
            const Cls tc = cls(f);
            const Cls vc = cls(v);
            const bool null = v->const_item() && v->is_null();
            if (!null && (tc == Cls::UNSAFE ||
                          (tc != vc && !(tc == Cls::REAL && vc == Cls::INT))))
                return fail("SET converts its value");
            if (!first) out += ", ";
            first = false;
            ident(std::string_view(fl->field_name.str, fl->field_name.length));
            out += " = ";
            if (!expr(v)) return false;
            assigned_.push_back(fl);
        }
        assigned_.clear();  // WHERE reads the old row in both
    } else {
        out += "DELETE FROM " + name;
    }
    if (where) {
        out += " WHERE ";
        if (!condition(where)) return false;
    }
    return true;
}

}  // namespace

bool gen_modify(THD* thd, st_select_lex* sel, Item* where, std::string* out,
                std::string* why) {
    Gen g(thd);
    g.top = sel;
    if (!g.modify(sel, where)) {
        if (why) *why = g.why ? g.why : "unknown";
        return false;
    }
    *out = std::move(g.out);
    return true;
}

bool gen_select(THD* thd, st_select_lex* sel, std::string* out,
                std::string* why) {
    Gen g(thd);
    g.top = sel;
    if (sel->next_select() || !g.select(sel, nullptr)) {
        if (why) *why = g.why ? g.why : "set operation leg";
        return false;
    }
    *out = std::move(g.out);
    return true;
}

bool gen_unit(THD* thd, st_select_lex_unit* unit, std::string* out,
              std::string* why) {
    Gen g(thd);
    if (!g.unit(unit, nullptr)) {
        if (why) *why = g.why ? g.why : "unknown";
        return false;
    }
    *out = std::move(g.out);
    return true;
}

}  // namespace stoolap_sqlgen
