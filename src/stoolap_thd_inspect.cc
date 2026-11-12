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
#include "stoolap_bridge.h"

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

// A name as MariaDB names its file: lowercased first when it keeps names
// lowercased, as it does the table's own
static std::string file_name(const LEX_CSTRING& name) {
    std::string s(name.str, name.length);
    if (lower_case_table_names == 1)
        s.resize(my_casedn_str(files_charset_info, s.data()));
    char buf[FN_REFLEN];
    const uint n = tablename_to_filename(s.c_str(), buf, sizeof(buf));
    return std::string(buf, n);
}

bool stoolap_thd_foreign_keys(THD* thd, const char* child_db,
                              size_t child_db_length,
                              std::vector<StoolapThdForeignKey>* out) {
    if (!thd || !thd->lex) return true;
    List_iterator_fast<Key> it(thd->lex->alter_info.key_list);
    while (Key* k = it++) {
        if (k->type != Key::FOREIGN_KEY) continue;
        auto* fk = static_cast<Foreign_key*>(k);
        if (fk->columns.elements != 1 || fk->ref_columns.elements > 1)
            return false;
        StoolapThdForeignKey def;
        const Key_part_spec* col = fk->columns.head();
        def.column = stoolap_mariadb::stoolap_column_name(
            std::string_view(col->field_name.str, col->field_name.length));
        def.parent_path = file_name(
            fk->ref_db.length ? fk->ref_db
                              : LEX_CSTRING{child_db, child_db_length});
        def.parent_path += '/';
        def.parent_path += file_name(fk->ref_table);
        if (const Key_part_spec* ref = fk->ref_columns.head())
            def.parent_column = stoolap_mariadb::stoolap_column_name(
                std::string_view(ref->field_name.str, ref->field_name.length));
        def.on_delete = fk->delete_opt;
        def.on_update = fk->update_opt;
        out->push_back(std::move(def));
    }
    return true;
}

bool stoolap_thd_lock_fk_parents(THD* thd, const char* child_db,
                                 size_t child_db_length, const char* child_name,
                                 size_t child_name_length, bool temporary,
                                 handlerton* hton, std::string* missing,
                                 bool* other_engine) {
    if (!thd || !thd->lex) return true;
    // The table the statement creates, or the one an ALTER copies (the
    // table created is its copy), as MariaDB names it. A CREATE ... SELECT
    // has its table out of query_tables while it runs
    std::string self_db(child_db, child_db_length);
    std::string self_name(child_name, child_name_length);
    const TABLE_LIST* altered = thd->lex->query_tables;
    if (thd->lex->sql_command != SQLCOM_CREATE_TABLE && altered) {
        self_db.assign(altered->db.str, altered->db.length);
        self_name.assign(altered->table_name.str, altered->table_name.length);
    }
    table_case_convert(self_db.data(), static_cast<uint>(self_db.size()));
    table_case_convert(self_name.data(), static_cast<uint>(self_name.size()));
    List_iterator_fast<Key> it(thd->lex->alter_info.key_list);
    while (Key* k = it++) {
        if (k->type != Key::FOREIGN_KEY) continue;
        const auto* fk = static_cast<Foreign_key*>(k);
        // As MariaDB names a table in its locks
        std::string db = fk->ref_db.length
                             ? std::string(fk->ref_db.str, fk->ref_db.length)
                             : std::string(child_db, child_db_length);
        std::string name(fk->ref_table.str, fk->ref_table.length);
        table_case_convert(db.data(), static_cast<uint>(db.size()));
        table_case_convert(name.data(), static_cast<uint>(name.size()));
        // A self-reference: the statement holds its own table already. A
        // TEMPORARY table, stoolap's by its file in the tmpdir, has none:
        // its key names the permanent table of that name
        if (!temporary && db == self_db && name == self_name) continue;
        // A lock as strong held already (MariaDB takes one on each parent
        // of an ALTER ... ADD FOREIGN KEY) is not taken again; the parent
        // is still checked below
        if (!thd->mdl_context.is_lock_owner(MDL_key::TABLE, db.c_str(),
                                            name.c_str(),
                                            MDL_SHARED_NO_WRITE)) {
            MDL_request request;
            MDL_REQUEST_INIT(&request, MDL_key::TABLE, db.c_str(), name.c_str(),
                             MDL_SHARED_NO_WRITE, MDL_TRANSACTION);
            if (thd->mdl_context.acquire_lock(&request,
                                              thd->variables.lock_wait_timeout))
                return false;
        }
        // The locked table must be the one the key binds to. MariaDB finds
        // it by its letter case under lower_case_table_names = 0, stoolap
        // by its name in lower case; as stoolap keeps one table per name in
        // lower case, a stoolap table found by MariaDB's name is that one
        const LEX_CSTRING db_name{db.c_str(), db.size()};
        const LEX_CSTRING table_name{name.c_str(), name.size()};
        handlerton* found = nullptr;
        if (!ha_table_exists(thd, &db_name, &table_name, nullptr, nullptr,
                             &found) ||
            found != hton) {
            *missing = db + "." + name;
            *other_engine = found != nullptr;
            return false;
        }
    }
    return true;
}

bool stoolap_thd_dropped_tables(THD* thd, std::vector<std::string>* paths) {
    if (!thd || !thd->lex || thd->lex->sql_command != SQLCOM_DROP_TABLE)
        return false;
    // A permanent table holds its MDL ticket; a TEMPORARY one of its name,
    // which the DROP takes instead, holds none
    for (const TABLE_LIST* t = thd->lex->query_tables; t; t = t->next_global)
        if (t->mdl_request.ticket)
            paths->push_back(file_name(t->db) + '/' + file_name(t->table_name));
    return true;
}

bool stoolap_thd_altered_table(
    THD* thd, std::string* path,
    std::vector<std::pair<std::string, std::string>>* renames,
    std::vector<std::string>* dropped) {
    if (!thd || !thd->lex) return false;
    LEX* lex = thd->lex;
    if (lex->sql_command != SQLCOM_ALTER_TABLE &&
        lex->sql_command != SQLCOM_CREATE_INDEX &&
        lex->sql_command != SQLCOM_DROP_INDEX)
        return false;
    const TABLE_LIST* t = lex->query_tables;
    if (!t) return false;
    // The opened table's own path: a TEMPORARY one is a file in the
    // tmpdir, not the permanent table of its name
    if (t->table)
        path->assign(t->table->s->normalized_path.str,
                     t->table->s->normalized_path.length);
    else
        *path = file_name(t->db) + '/' + file_name(t->table_name);
    List_iterator_fast<Alter_drop> drops(lex->alter_info.drop_list);
    while (const Alter_drop* d = drops++) {
        if (d->type == Alter_drop::COLUMN)
            dropped->push_back(stoolap_mariadb::stoolap_column_name(d->name));
    }
    // CHANGE old new ...
    List_iterator_fast<Create_field> fields(lex->alter_info.create_list);
    while (const Create_field* f = fields++) {
        if (f->change.length)
            renames->emplace_back(
                stoolap_mariadb::stoolap_column_name(
                    std::string_view(f->change.str, f->change.length)),
                stoolap_mariadb::stoolap_column_name(
                    std::string_view(f->field_name.str, f->field_name.length)));
    }
    // RENAME COLUMN old TO new
    List_iterator_fast<Alter_column> columns(lex->alter_info.alter_list);
    while (const Alter_column* c = columns++) {
        if (c->new_name.length)
            renames->emplace_back(
                stoolap_mariadb::stoolap_column_name(
                    std::string_view(c->name.str, c->name.length)),
                stoolap_mariadb::stoolap_column_name(
                    std::string_view(c->new_name.str, c->new_name.length)));
    }
    return true;
}
