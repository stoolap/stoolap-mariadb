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

extern "C" int stoolap_thd_has_explicit_limit(THD* thd) {
    if (!thd || !thd->lex) return 0;
    SELECT_LEX* sel = thd->lex->first_select_lex();
    if (!sel) return 0;
    return sel->limit_params.explicit_limit ? 1 : 0;
}

extern "C" int stoolap_thd_is_update_or_delete(THD* thd) {
    if (!thd || !thd->lex) return 0;
    const int cmd = thd->lex->sql_command;
    return (cmd == SQLCOM_UPDATE || cmd == SQLCOM_DELETE) ? 1 : 0;
}

// True for INSERT IGNORE / REPLACE / ODKU. For these the all-or-nothing
// stmt_exec_batch path is unsafe: one dup aborts the batch and silently
// drops every non-conflicting row, bypassing MariaDB's per-row recovery.
extern "C" int stoolap_thd_needs_per_row_dup_handling(THD* thd) {
    if (!thd || !thd->lex) return 0;
    if (thd->lex->ignore) return 1;
    if (thd->lex->duplicates == DUP_REPLACE) return 1;
    if (thd->lex->duplicates == DUP_UPDATE) return 1;
    return 0;
}
