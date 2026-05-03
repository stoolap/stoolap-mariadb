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

#include <stoolap.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace stoolap_mariadb {

struct DbDeleter {
    void operator()(StoolapDB* p) const noexcept {
        if (p) stoolap_close(p);
    }
};
struct TxDeleter {
    void operator()(StoolapTx* p) const noexcept {
        if (p) stoolap_tx_rollback(p);
    }
};
struct RowsDeleter {
    void operator()(StoolapRows* p) const noexcept {
        if (p) stoolap_rows_close(p);
    }
};
struct StmtDeleter {
    void operator()(StoolapStmt* p) const noexcept {
        if (p) stoolap_stmt_finalize(p);
    }
};

using DbPtr = std::unique_ptr<StoolapDB, DbDeleter>;
using TxPtr = std::unique_ptr<StoolapTx, TxDeleter>;
using RowsPtr = std::unique_ptr<StoolapRows, RowsDeleter>;
using StmtPtr = std::unique_ptr<StoolapStmt, StmtDeleter>;

/**
 * Owns the packed buffer returned by stoolap_rows_fetch_all(). Frees via
 * stoolap_buffer_free in the destructor. Lets the buffered scan path
 * skip the std::vector::assign copy that doubled memory bandwidth on
 * the very path meant to reduce FFI overhead.
 */
class StoolapBuffer {
public:
    StoolapBuffer() = default;
    ~StoolapBuffer() noexcept { reset(); }

    StoolapBuffer(StoolapBuffer&& o) noexcept : buf_(o.buf_), len_(o.len_) {
        o.buf_ = nullptr;
        o.len_ = 0;
    }
    StoolapBuffer& operator=(StoolapBuffer&& o) noexcept {
        if (this != &o) {
            reset();
            buf_ = o.buf_;
            len_ = o.len_;
            o.buf_ = nullptr;
            o.len_ = 0;
        }
        return *this;
    }

    StoolapBuffer(const StoolapBuffer&) = delete;
    StoolapBuffer& operator=(const StoolapBuffer&) = delete;

    /// Take ownership of a buffer returned by stoolap_rows_fetch_all().
    void take(uint8_t* buf, int64_t len) {
        reset();
        buf_ = buf;
        len_ = len;
    }

    void reset() noexcept {
        if (buf_) {
            stoolap_buffer_free(buf_, len_);
            buf_ = nullptr;
            len_ = 0;
        }
    }

    uint8_t* data() noexcept { return buf_; }
    const uint8_t* data() const noexcept { return buf_; }
    size_t size() const noexcept { return static_cast<size_t>(len_); }
    bool empty() const noexcept { return len_ == 0; }

private:
    uint8_t* buf_ = nullptr;
    int64_t len_ = 0;
};

/**
 * Process-wide database handle. Each MariaDB handler instance clones from this
 * via stoolap_clone() to get a thread-local handle.
 */
class Engine {
public:
    /** Open the underlying stoolap database. dsn must be a valid stoolap DSN. */
    int open(std::string_view dsn);

    /** Close the database. Idempotent. */
    void close() noexcept;

    /** Clone a per-thread handle. Caller owns the returned DbPtr. */
    DbPtr clone_handle() const;

    StoolapDB* raw() const noexcept { return db_.get(); }

    /** Last error message captured from a stoolap call. */
    const std::string& last_error() const noexcept { return last_error_; }

    void set_error(std::string msg) { last_error_ = std::move(msg); }

    /** Per-process cache of tables we've already reconciled (i.e. issued
     *  the CREATE TABLE IF NOT EXISTS for). ha_stoolap::open() runs every
     *  time MariaDB opens a handler instance -- in tight benchmark loops
     *  that's once per statement -- so caching avoids repeated SQL
     *  construction + parsing on the stoolap side for tables we've
     *  already verified exist.
     *
     *  Invalidated by drop_reconciled / clear_reconciled on DROP / TRUNCATE
     *  / RENAME so a subsequent open re-runs the create.
     */
    bool is_reconciled(const std::string& name);
    void mark_reconciled(const std::string& name);
    void drop_reconciled(const std::string& name);
    void clear_reconciled();

    /** Approximate row count cache shared across handler instances on
     *  the same stoolap table. The per-handler cache (`ha_stoolap::
     *  cached_records_`) dies when MariaDB closes the handler -- which
     *  in tight loops happens once per statement, defeating the cache.
     *  This process-wide layer survives that.
     *
     *  records_lookup() returns true and fills `out` on hit. Exact DDL paths
     *  may seed records_set(); ordinary live COUNT(*) results stay
     *  handler-local or transaction-local so snapshot-visible counts do not
     *  leak to other sessions. Mutations from any connection invalidate via
     *  records_invalidate(); DDL (DROP, RENAME) calls records_drop().
     *
     *  This is approximate: cross-connection writes that bypass the
     *  plugin (direct stoolap_exec) leave the cache stale, and we
     *  don't try to atomically delta the count -- mutating handlers
     *  invalidate the entry and the next reader re-counts. Stats must
     *  be approximate per HA_STATS_RECORDS_IS_EXACT not being set, so
     *  the consequences of staleness are limited to optimizer planning. */
    bool records_lookup(const std::string& name, uint64_t* out);
    void records_set(const std::string& name, uint64_t count);
    void records_invalidate(const std::string& name);
    void records_drop(const std::string& name);

    /**
     * Process-wide AUTO_INCREMENT reservation cache. Per-connection
     * counters are not safe: two sessions can both cache "next=2" and
     * collide, or one session can miss another's explicit INSERT(id=100).
     * These helpers reserve ids under a single mutex while still avoiding
     * SELECT MAX(col) per row once a table has been seeded.
     *
     * `step` and `offset_mod` model MariaDB's session
     * `auto_increment_increment` / `auto_increment_offset`. Multi-writer
     * replication relies on the engine returning ids that satisfy
     * `id % step == offset_mod` and on the engine's internal cursor
     * advancing past *every* id in MariaDB's logical sequence (not just
     * the count contiguous ids the engine literally hands back). With
     * step=1 the behaviour is the historical "reserve count contiguous
     * ids" path, so existing call sites remain correct.
     */
    bool ai_reserve(const std::string& name, uint64_t count, uint64_t step,
                    uint64_t offset_mod, uint64_t* first);
    void ai_seed_and_reserve(const std::string& name, uint64_t seed,
                             uint64_t count, uint64_t step, uint64_t offset_mod,
                             uint64_t* first);
    void ai_note_explicit(const std::string& name, uint64_t value);
    void ai_invalidate();

private:
    DbPtr db_;
    std::string last_error_;
    std::mutex reconciled_mu_;
    std::unordered_set<std::string> reconciled_;

    std::mutex records_mu_;
    // value of UINT64_MAX = invalidated, awaiting recount
    std::unordered_map<std::string, uint64_t> records_;

    std::mutex ai_mu_;
    std::unordered_map<std::string, uint64_t> ai_next_;
};

/** Convert a stoolap status code to a short human-readable label (for logs). */
const char* status_label(int32_t code);

/**
 * Process-wide engine counters surfaced via SHOW STATUS.
 * Increment from any handler thread (atomic, relaxed). Read by the
 * status_vars callbacks in the plugin descriptor; the SHOW STATUS code
 * snapshots on demand.
 */
struct PushdownStats {
    std::atomic<uint64_t> pushdown_hits{0};  // select_handler took the query
    std::atomic<uint64_t> pushdown_misses{
        0};  // factory returned NULL, fell to row pump
    std::atomic<uint64_t> buffered_scans{
        0};  // rnd_init completed via fetch_all (Tier 3)
    std::atomic<uint64_t> buffered_rows{0};  // rows materialised through Tier 3
    std::atomic<uint64_t> direct_modify_hits{
        0};  // direct_update_rows / direct_delete_rows took it
    std::atomic<uint64_t> records_live_counts{
        0};  // cached_records() cache miss that had to fetch a fresh
             // count from stoolap. After PR-B this is an O(1) atomic
             // load via stoolap_(tx_)table_count rather than a real
             // SELECT COUNT(*); the counter still measures cache
             // effectiveness (high values mean handler/tx/global
             // caches are not being reused) but the cost per miss
             // is now negligible.
    std::atomic<uint64_t> unmapped_errors{
        0};  // BOTH the typed errcode AND the prose pattern table came back
             // generic. Either stoolap added a STOOLAP_ERR_* code we don't
             // recognise OR it returned a brand-new message wording. Either
             // way the user sees ER_GET_ERRMSG (1296) with the raw text.
    std::atomic<uint64_t> typed_fallback_hits{
        0};  // typed errcode was STOOLAP_ERR_GENERIC but the prose pattern
             // still classified the message into a specific HA_ERR_*. Means
             // stoolap returned a generic code for an error we know how to
             // pin down by text -- a stoolap-side typed-error gap. When this
             // and unmapped_errors both stay 0 across a real run, the prose
             // fallback can be retired.

    // Per-phase timing buckets for pushdown latency profiling. Gated by
    // perf_trace_enabled below (default OFF) because the per-row clocks
    // and atomic increments are visible on microsecond-scale point
    // queries -- ~50ns per next_row that the user shouldn't pay just for
    // having the plugin loaded. Flip with `SET GLOBAL stoolap_perf_trace=1`
    // when investigating; flip back to drop the cost.
    std::atomic<uint64_t> perf_factory_setup_ns{
        0};  // factory time minus eager_query
    std::atomic<uint64_t> perf_eager_query_ns{0};  // stoolap_(tx_)query call
    std::atomic<uint64_t> perf_init_scan_ns{0};
    std::atomic<uint64_t> perf_next_row_ns{0};  // summed over all calls
    std::atomic<uint64_t> perf_end_scan_ns{0};
    std::atomic<uint64_t> perf_query_count{0};
    std::atomic<uint64_t> perf_next_row_count{0};
};

// Process-wide on/off switch for the Stoolap_perf_* counters. Loaded with
// memory_order_relaxed in the hot paths -- a single bool read + branch,
// effectively free when off. Toggled by the stoolap_perf_trace system
// variable's update callback.
extern std::atomic<bool> g_perf_trace_enabled;

extern PushdownStats g_stats;

}  // namespace stoolap_mariadb
