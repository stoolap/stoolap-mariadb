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

#include "stoolap_thd_inspect.h"

#include "my_global.h"
#include "sql_class.h"
#include "sql_lex.h"

// Exposed to ha_stoolap.cc (which intentionally doesn't include sql_class.h
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
// whole batch and the caller-side recovery never gets to run -- silently
// dropping every non-conflicting row in the batch.
extern "C" int stoolap_thd_needs_per_row_dup_handling(THD* thd) {
    if (!thd || !thd->lex) return 0;
    if (thd->lex->ignore) return 1;
    if (thd->lex->duplicates == DUP_REPLACE) return 1;
    if (thd->lex->duplicates == DUP_UPDATE) return 1;
    return 0;
}
