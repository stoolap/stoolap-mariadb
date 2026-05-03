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

/**
 * Per-THD context, attached via `thd_get_ha_data` / `thd_set_ha_data`.
 *
 * Owns:
 *   - a thread-local `StoolapDB` clone (created lazily on first use; the
 *     global engine handle is single-threaded and unsafe to share),
 *   - an optional `StoolapTx` for the THD's currently open transaction.
 *
 * Lifetime is bounded by the THD: `close_connection` deletes the context.
 */
class ThdContext {
public:
    explicit ThdContext(Engine* engine);
    ~ThdContext();

    ThdContext(const ThdContext&) = delete;
    ThdContext& operator=(const ThdContext&) = delete;

    /** Lazily clone the engine's DB handle on first call. */
    StoolapDB* db();

    StoolapTx* tx() const { return tx_.get(); }
    bool has_tx() const { return static_cast<bool>(tx_); }

    /** Begin a stoolap transaction. No-op if one is already active. */
    int begin(int32_t isolation = STOOLAP_ISOLATION_READ_COMMITTED);

    /** Commit the active transaction (or no-op). Consumes the Tx handle. */
    int commit();

    /** Rollback the active transaction (or no-op). Consumes the Tx handle. */
    int rollback();

    /** Note that this transaction may have changed a table's row count.
     *  Other sessions are allowed to cache the old committed COUNT(*)
     *  while this tx is in flight, so commit/rollback must invalidate
     *  those table entries once the final outcome is known. */
    void note_records_dirty(const std::string& table);

    /** Transaction-local COUNT(*) cache. Safe to reuse only inside this THD's
     *  active stoolap transaction because it can include this session's
     *  uncommitted writes and snapshot view. */
    bool records_lookup(const std::string& table, uint64_t* out) const;
    void records_set(const std::string& table, uint64_t count);
    void records_invalidate(const std::string& table);
    void records_adjust(const std::string& table, int64_t delta);

    /** Invalidate and clear all row-count cache entries dirtied by this tx. */
    void invalidate_dirty_records();

    /**
     * Cached prepared StoolapStmt for bulk INSERT, keyed by the SQL
     * template (`INSERT INTO "<table>" VALUES ($1, $2, ...)`). The
     * handler is created fresh per statement, so a per-handler cache
     * dies between batches; this connection-scoped map keeps the
     * prepared stmt alive across statements on the same connection,
     * matching the direct stoolap benchmark pattern of one prepare per
     * hot template.
     *
     * Returns nullptr if the SQL hasn't been prepared yet; the caller
     * prepares once and stores via bulk_stmt_put. Lifetime tied to the
     * ThdContext (cleared at disconnect).
     */
    StoolapStmt* bulk_stmt_get(const std::string& sql);
    void bulk_stmt_put(std::string sql, StmtPtr stmt);

    /** Allocate the next monotonic savepoint id for this connection.
     *  MariaDB does not pass the user's SAVEPOINT name into the
     *  hton->savepoint_set callback, so we generate our own
     *  byte-stable name ("sp<id>") and stash the id in the engine-
     *  private chunk MariaDB allocates for us. The id space is
     *  per-connection and resets on disconnect (when the ThdContext
     *  is destroyed); stoolap-side savepoint state is tx-scoped and
     *  vanishes with commit/rollback so re-using ids across
     *  transactions is safe. */
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
