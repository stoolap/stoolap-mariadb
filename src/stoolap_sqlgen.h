/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <string>

class THD;
class Item;
class st_select_lex;
class st_select_lex_unit;

namespace stoolap_sqlgen {

// Writes stoolap SQL equivalent to `sel` (a prepared, not yet optimized
// SELECT; see create_stoolap_select_handler) into *out, built from its
// Item tree rather than the statement text. Returns false, with a short
// reason in *why, when any part has no stoolap equivalent with the same
// MariaDB semantics; the caller then leaves the statement to MariaDB.
// The result has one output column per visible select-list item, in
// order.
bool gen_select(THD* thd, st_select_lex* sel, std::string* out,
                std::string* why);

// The same for a whole query expression (UNION / INTERSECT / EXCEPT legs
// with its ORDER BY / LIMIT, or one SELECT), as the unit and derived
// handlers run it. Result columns come out in order, named c0, c1, ...
bool gen_unit(THD* thd, st_select_lex_unit* unit, std::string* out,
              std::string* why);

// The SET value gen_modify wrapped in a range or length check, if any:
// stoolap fails the statement with an integer overflow for a row whose
// value does not fit `column`, which MariaDB reports as `error`
// (ER_WARN_DATA_OUT_OF_RANGE or ER_DATA_TOO_LONG).
// The overflow a ModifyGuard check fails the statement with, and stoolap's
// whole message for it: told by the message as a whole, since a user's
// value may hold its words (a duplicate key names the value), and by
// operands no other overflow is likely to have.
inline constexpr const char* kGuardOverflow = "9223372036854775807 + 4242";
inline constexpr const char* kGuardOverflowError =
    "type error: Integer overflow in arithmetic operation: "
    "9223372036854775807 and 4242";

struct ModifyGuard {
    std::string column;
    unsigned error = 0;
};

// The rows an UPDATE that writes only the rows it changes finds, which
// MariaDB reports apart ("Rows matched"): `sql` counts them, empty when
// the UPDATE's count is already the rows found. Run before the UPDATE,
// which may change what the condition takes.
struct ModifyCount {
    std::string sql;
};

// The single-table UPDATE or DELETE whose SELECT_LEX is `sel`, as the
// direct DML hooks run it: `where` is the condition MariaDB pushed (null
// for none), an UPDATE's SET list its item_list and LEX::value_list.
// `rows_through_sql`: the row pump would write the table's rows through
// SQL (a table a FOREIGN KEY names), naming each by its primary key, so
// an UPDATE of a table with none is written here.
bool gen_modify(THD* thd, st_select_lex* sel, Item* where, std::string* out,
                std::string* why, ModifyGuard* guard = nullptr,
                ModifyCount* count = nullptr, bool rows_through_sql = false);

}  // namespace stoolap_sqlgen
