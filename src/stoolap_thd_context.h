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

#include "stoolap_bridge.h"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace stoolap_mariadb {

class Engine;

// Per-THD ctx via thd_get/set_ha_data. Owns a per-connection StoolapDB
// clone (lazy; global handle is single-threaded) and the active StoolapTx.
// Destroyed by close_connection.
class ThdContext {
public:
    explicit ThdContext(Engine* engine);
    ~ThdContext();

    ThdContext(const ThdContext&) = delete;
    ThdContext& operator=(const ThdContext&) = delete;

    StoolapDB* db();
    StoolapTx* tx() const { return tx_.get(); }
    bool has_tx() const { return static_cast<bool>(tx_); }

    int begin(int32_t isolation = STOOLAP_ISOLATION_READ_COMMITTED);
    int commit();
    int rollback();

    // Mark a table dirty so commit/rollback invalidates other sessions'
    // cached COUNT for it once the outcome is known.
    void note_records_dirty(const std::string& table);

    // Tx-local COUNT cache; safe only inside this THD's active tx
    // (sees uncommitted writes / snapshot view).
    bool records_lookup(const std::string& table, uint64_t* out) const;
    void records_set(const std::string& table, uint64_t count);
    void records_invalidate(const std::string& table);
    void records_adjust(const std::string& table, int64_t delta);
    void invalidate_dirty_records();

    // Connection-scoped prepared-stmt cache for bulk INSERT SQL
    // templates. The handler is created fresh per-statement so a per-
    // handler cache would die between batches.
    StoolapStmt* bulk_stmt_get(const std::string& sql);
    void bulk_stmt_put(std::string sql, StmtPtr stmt);

    // MariaDB doesn't pass the user's SAVEPOINT name to engines; we
    // generate "sp<id>" from a per-connection counter. Stoolap-side
    // savepoints are tx-scoped so id reuse across transactions is safe.
    uint64_t next_savepoint_id() { return ++next_savepoint_id_; }

private:
    Engine* engine_;
    DbPtr db_;
    TxPtr tx_;

    std::unordered_map<std::string, StmtPtr> bulk_stmt_cache_;
    std::unordered_set<std::string> dirty_record_tables_;
    std::unordered_map<std::string, uint64_t> record_counts_;
    uint64_t next_savepoint_id_ = 0;
};

}  // namespace stoolap_mariadb
