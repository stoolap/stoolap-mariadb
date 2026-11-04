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

#include "my_global.h"
#include "my_base.h"  // ha_rows

class THD;
class st_select_lex;
class st_select_lex_unit;
typedef class st_select_lex SELECT_LEX;
typedef class st_select_lex_unit SELECT_LEX_UNIT;
class select_handler;
class derived_handler;
class Item;
struct TABLE_LIST;
struct TABLE;
template <class T>
class List;

#include <string>

extern handlerton* stoolap_hton;

namespace stoolap_pushdown {

// Direct UPDATE / DELETE. can_direct_modify: the statement's shape
// allows it. plan_direct_modify: its SQL, generated from the Item tree
// (cond: the WHERE MariaDB pushed; fields: the SET targets), when every
// expression translates and every row fits; false sends the statement to
// the row pump. run_direct_modify: 0 + *affected, or the mapped error.
bool can_direct_modify(THD* thd);
bool plan_direct_modify(THD* thd, TABLE* table, Item* cond, List<Item>* fields,
                        std::string* sql);
int run_direct_modify(THD* thd, const std::string& sql, ha_rows* affected,
                      unsigned* errkey_out = nullptr);

// Pushdown factories. Wired in stoolap_init_func; NULL is a clean
// fall-through to row-pump for ineligible plans.
select_handler* create_stoolap_select_handler(THD* thd, SELECT_LEX* sel_lex,
                                              SELECT_LEX_UNIT* sel_unit);
select_handler* create_stoolap_unit_handler(THD* thd,
                                            SELECT_LEX_UNIT* sel_unit);
// Partial pushdown: the outer mixes engines, the derived is all-stoolap.
derived_handler* create_stoolap_derived_handler(THD* thd, TABLE_LIST* derived);

}  // namespace stoolap_pushdown
