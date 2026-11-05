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
#include "sql_table.h"  // tablename_to_filename
#include "stoolap_mariadb.h"

static_assert(FK_OPTION_UNDEF == STOOLAP_FK_UNDEF &&
                  FK_OPTION_RESTRICT == STOOLAP_FK_RESTRICT &&
                  FK_OPTION_NO_ACTION == STOOLAP_FK_NO_ACTION &&
                  FK_OPTION_CASCADE == STOOLAP_FK_CASCADE &&
                  FK_OPTION_SET_NULL == STOOLAP_FK_SET_NULL &&
                  FK_OPTION_SET_DEFAULT == STOOLAP_FK_SET_DEFAULT,
              "STOOLAP_FK_* are MariaDB's enum_fk_option");

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

bool stoolap_thd_foreign_keys(THD* thd, const char* child_db,
                              size_t child_db_length,
                              std::vector<StoolapThdForeignKey>* out) {
    if (!thd || !thd->lex) return true;
    // A name as MariaDB names its file: lowercased first when it keeps
    // names lowercased, as it does the child's
    auto file_name = [](const LEX_CSTRING& name) {
        std::string s(name.str, name.length);
        if (lower_case_table_names == 1)
            s.resize(my_casedn_str(files_charset_info, s.data()));
        char buf[FN_REFLEN];
        const uint n = tablename_to_filename(s.c_str(), buf, sizeof(buf));
        return std::string(buf, n);
    };
    List_iterator_fast<Key> it(thd->lex->alter_info.key_list);
    while (Key* k = it++) {
        if (k->type != Key::FOREIGN_KEY) continue;
        auto* fk = static_cast<Foreign_key*>(k);
        if (fk->columns.elements != 1 || fk->ref_columns.elements > 1)
            return false;
        StoolapThdForeignKey def;
        const Key_part_spec* col = fk->columns.head();
        def.column.assign(col->field_name.str, col->field_name.length);
        def.parent_path = file_name(
            fk->ref_db.length ? fk->ref_db
                              : LEX_CSTRING{child_db, child_db_length});
        def.parent_path += '/';
        def.parent_path += file_name(fk->ref_table);
        if (const Key_part_spec* ref = fk->ref_columns.head())
            def.parent_column.assign(ref->field_name.str,
                                     ref->field_name.length);
        def.on_delete = fk->delete_opt;
        def.on_update = fk->update_opt;
        out->push_back(std::move(def));
    }
    return true;
}
