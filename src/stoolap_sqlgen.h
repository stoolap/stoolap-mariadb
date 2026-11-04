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
struct TABLE;
template <class T>
class List;
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

// Direct UPDATE / DELETE of `table` (the statement's only table) with
// the WHERE MariaDB pushed (`cond`, null for none). For UPDATE, `fields`
// and `values` are the SET list; with `changed_only` only rows whose
// value changes are updated, so stoolap's count is MariaDB's "changed".
// *guard, when not empty, is a query that returns a row if some row
// would need MariaDB's own conversion (out of range, too long, NULL into
// NOT NULL): then the statement must not run directly.
bool gen_update(THD* thd, TABLE* table, Item* cond, List<Item>& fields,
                List<Item>& values, bool changed_only, std::string* out,
                std::string* guard, std::string* why);
bool gen_delete(THD* thd, TABLE* table, Item* cond, std::string* out,
                std::string* why);

}  // namespace stoolap_sqlgen
