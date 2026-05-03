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

extern handlerton* stoolap_hton;

namespace stoolap_pushdown {

// 0 + *affected on success; HA_ERR_WRONG_COMMAND signals fall-back to
// the per-row update_row/delete_row path.
int try_direct_modify(THD* thd, ha_rows* affected,
                      unsigned* errkey_out = nullptr);

// Eligibility predicate; same checks try_direct_modify runs internally.
bool can_direct_modify(THD* thd);

// Pushdown factories. Wired in stoolap_init_func; NULL is a clean
// fall-through to row-pump for ineligible plans.
select_handler* create_stoolap_select_handler(THD* thd, SELECT_LEX* sel_lex,
                                              SELECT_LEX_UNIT* sel_unit);
select_handler* create_stoolap_unit_handler(THD* thd,
                                            SELECT_LEX_UNIT* sel_unit);
// Partial pushdown: the outer mixes engines, the derived is all-stoolap.
derived_handler* create_stoolap_derived_handler(THD* thd, TABLE_LIST* derived);

// True when the Item tree only references columns where stoolap and
// MariaDB compare byte-wise. trust_binary_strings opts out.
bool item_safe_for_byte_comparison(Item* item);

}  // namespace stoolap_pushdown
