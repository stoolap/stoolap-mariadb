/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "stoolap_thd_context.h"

namespace stoolap_mariadb {

ThdContext::ThdContext(Engine* engine) : engine_(engine) {}

ThdContext::~ThdContext() {
    // Safety net: if a connection is killed mid-transaction, roll back.
    if (tx_) {
        StoolapTx* raw = tx_.release();
        stoolap_tx_rollback(raw);
    }
    invalidate_dirty_records();
}

StoolapDB* ThdContext::db() {
    if (!db_ && engine_) db_ = engine_->clone_handle();
    return db_.get();
}

int ThdContext::begin(int32_t isolation) {
    if (tx_) return STOOLAP_OK;  // idempotent
    if (!db()) return STOOLAP_ERROR;

    StoolapTx* raw = nullptr;
    int rc = stoolap_begin_with_isolation(db_.get(), isolation, &raw);
    if (rc != STOOLAP_OK) return rc;
    record_counts_.clear();
    tx_.reset(raw);
    return STOOLAP_OK;
}

int ThdContext::commit() {
    if (!tx_) return STOOLAP_OK;
    StoolapTx* raw = tx_.release();
    int rc = stoolap_tx_commit(raw);
    invalidate_dirty_records();
    record_counts_.clear();
    return rc;
}

int ThdContext::rollback() {
    if (!tx_) return STOOLAP_OK;
    StoolapTx* raw = tx_.release();
    int rc = stoolap_tx_rollback(raw);
    invalidate_dirty_records();
    record_counts_.clear();
    return rc;
}

StoolapStmt* ThdContext::bulk_stmt_get(const std::string& sql) {
    auto it = bulk_stmt_cache_.find(sql);
    return it == bulk_stmt_cache_.end() ? nullptr : it->second.get();
}

void ThdContext::bulk_stmt_put(std::string sql, StmtPtr stmt) {
    bulk_stmt_cache_.emplace(std::move(sql), std::move(stmt));
}

void ThdContext::note_records_dirty(const std::string& table) {
    if (!table.empty()) dirty_record_tables_.insert(table);
}

bool ThdContext::records_lookup(const std::string& table, uint64_t* out) const {
    auto it = record_counts_.find(table);
    if (it == record_counts_.end()) return false;
    if (out) *out = it->second;
    return true;
}

void ThdContext::records_set(const std::string& table, uint64_t count) {
    if (!table.empty()) record_counts_[table] = count;
}

void ThdContext::records_invalidate(const std::string& table) {
    if (!table.empty()) record_counts_.erase(table);
}

void ThdContext::records_adjust(const std::string& table, int64_t delta) {
    auto it = record_counts_.find(table);
    if (it == record_counts_.end() || delta == 0) return;

    uint64_t magnitude = 0;
    if (delta < 0) {
        magnitude = static_cast<uint64_t>(-(delta + 1)) + 1;
        if (magnitude > it->second) {
            record_counts_.erase(it);
            return;
        }
        it->second -= magnitude;
        return;
    }

    magnitude = static_cast<uint64_t>(delta);
    if (magnitude > UINT64_MAX - it->second) {
        record_counts_.erase(it);
        return;
    }
    it->second += magnitude;
}

void ThdContext::invalidate_dirty_records() {
    if (engine_) {
        for (const std::string& table : dirty_record_tables_) {
            engine_->records_invalidate(table);
        }
    }
    dirty_record_tables_.clear();
}

}  // namespace stoolap_mariadb
