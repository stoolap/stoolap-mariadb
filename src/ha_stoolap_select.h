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

/**
 * Direct UPDATE / DELETE pushdown: take the THD's current SQL text (with
 * MariaDB-side identifiers), rewrite table names to stoolap's flat form,
 * and execute as a single statement against the THD's stoolap handle.
 * Used by ha_stoolap::direct_update_rows / direct_delete_rows so qualifying
 * UPDATEs and DELETEs avoid MariaDB's per-row callback loop -- one stoolap
 * SQL statement does the whole job in stoolap's own engine instead.
 *
 * Returns 0 on success with `*affected` populated, or a HA_ERR_* code.
 * HA_ERR_WRONG_COMMAND signals MariaDB to fall back to the per-row
 * update_row/delete_row path; that's the right answer for unsafe-to-push
 * cases (SP locals, prepared phase, FOR UPDATE, multi-table UPDATE, etc.).
 */
int try_direct_modify(THD* thd, ha_rows* affected,
                      unsigned* errkey_out = nullptr);

/** Eligibility predicate. The same statement-level checks try_direct_modify
 *  applies internally; called from direct_update_rows_init /
 *  direct_delete_rows_init so MariaDB knows up front whether to use the
 *  direct path. */
bool can_direct_modify(THD* thd);

/**
 * Whole-SELECT pushdown factories. Wired into `stoolap_hton->create_select`
 * and `stoolap_hton->create_unit` from stoolap_init_func.
 *
 * Each returns a select_handler* that owns the rewritten SQL and a stoolap
 * rows iterator, or NULL when the query isn't pushdown-eligible (cross-
 * engine join, SP context, prepare phase, side effects, etc.). Returning
 * NULL is a clean fall-through: MariaDB will execute the query through the
 * row-pump path (rnd_next) instead.
 */
select_handler* create_stoolap_select_handler(THD* thd, SELECT_LEX* sel_lex,
                                              SELECT_LEX_UNIT* sel_unit);

select_handler* create_stoolap_unit_handler(THD* thd,
                                            SELECT_LEX_UNIT* sel_unit);

/**
 * Partial-pushdown factory: when the outer SELECT mixes engines, the whole
 * query can't push (cross-engine JOIN) but a fully-stoolap derived table
 * inside it can. MariaDB calls this once per derived TABLE_LIST after
 * select_handler returns NULL. We push the derived's inner SELECT to
 * stoolap and stream rows back; MariaDB JOINs them with the outer engine's
 * tables itself.
 */
derived_handler* create_stoolap_derived_handler(THD* thd, TABLE_LIST* derived);

/**
 * Returns true when the Item tree only references columns where stoolap
 * and MariaDB compare byte-wise (numeric, date/time, binary-collated
 * strings). Used by cond_push() to decide whether the engine can safely
 * accept a WHERE expression for direct DML -- when accepted, MariaDB
 * routes UPDATE/DELETE through ha_direct_*_rows and try_direct_modify
 * forwards thd->query() to stoolap, which re-evaluates the WHERE itself.
 *
 * Returns false if any non-binary string column is referenced (under
 * MariaDB's default ci collation, stoolap's bytewise compare would skip
 * case-fold matches that MariaDB expects). When the session sets
 * stoolap_trust_binary_strings = 1 the caller bypasses this check.
 */
bool item_safe_for_byte_comparison(Item* item);

}  // namespace stoolap_pushdown
