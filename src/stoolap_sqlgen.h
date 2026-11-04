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
class st_select_lex;

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

}  // namespace stoolap_sqlgen
