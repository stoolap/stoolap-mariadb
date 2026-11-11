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
#include <utility>
#include <vector>

class THD;
struct handlerton;

// A single-column FOREIGN KEY as MariaDB's parser read it; the parent by
// its file path, "db/table" as tablename_to_filename encodes them.
struct StoolapThdForeignKey {
    std::string column;
    std::string parent_path;
    std::string parent_column;  // empty: the parent's primary key
    int on_delete;              // enum_fk_option
    int on_update;
};

// The FOREIGN KEYs of the statement's CREATE / ALTER TABLE: MariaDB keeps
// none for an engine that does not claim them. A parent naming no
// database is in `child_db` (`child_db_length` bytes), the child's
// database by name. False for a composite one, which stoolap does not
// enforce.
// The table the statement's copying ALTER TABLE (or CREATE / DROP INDEX)
// rebuilds, by its file path, the columns it renames (old name, new name)
// and the columns it drops; false for any other statement.
bool stoolap_thd_altered_table(
    THD* thd, std::string* path,
    std::vector<std::pair<std::string, std::string>>* renames,
    std::vector<std::string>* dropped);

bool stoolap_thd_foreign_keys(THD* thd, const char* child_db,
                              size_t child_db_length,
                              std::vector<StoolapThdForeignKey>* out);

// Locks each parent of the statement's FOREIGN KEYs against a copying
// ALTER of it until the statement ends (MDL_SHARED_NO_WRITE, which the
// ALTER's MDL_SHARED_UPGRADABLE excludes): the key then names the table
// the ALTER leaves, or the ALTER, waiting for it, finds the key and is
// refused. A self-reference is skipped; a parent locked as strongly
// already is not locked again. False when a wait fails, with the error
// raised, or when a parent is no table of engine `hton` by that name,
// named in `missing` ("db.table"), with `other_engine` set when it is a
// table of another engine.
bool stoolap_thd_lock_fk_parents(THD* thd, const char* child_db,
                                 size_t child_db_length, const char* child_name,
                                 size_t child_name_length, handlerton* hton,
                                 std::string* missing, bool* other_engine);

// sql_class.h-dependent helpers: live in stoolap_thd_inspect.cc to keep
// the heavy sql_class/wsrep include chain out of most TUs.
extern "C" int stoolap_thd_is_update_or_delete(THD* thd);
extern "C" int stoolap_thd_needs_per_row_dup_handling(THD* thd);

// THDVAR-backed; live in ha_stoolap.cc because THDVAR resolves against
// the sysvar symbols generated there.
extern "C" int stoolap_thd_trust_binary_strings(THD* thd);
extern "C" int stoolap_thd_explain_pushdown(THD* thd);
