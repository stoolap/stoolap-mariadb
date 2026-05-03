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

// Owns the packed buffer from stoolap_rows_fetch_all(); destructor calls
// stoolap_buffer_free. Avoids the std::vector::assign memcpy on the very
// path designed to cut FFI overhead.
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

// Process-wide handle; each handler clones via stoolap_clone for thread safety.
class Engine {
public:
    int open(std::string_view dsn);
    void close() noexcept;
    DbPtr clone_handle() const;
    StoolapDB* raw() const noexcept { return db_.get(); }
    const std::string& last_error() const noexcept { return last_error_; }
    void set_error(std::string msg) { last_error_ = std::move(msg); }

    // CREATE TABLE IF NOT EXISTS short-circuit cache. Per-handler open()
    // fires per-statement in tight loops; caching avoids repeated SQL
    // construction + parsing for tables we've already verified.
    // Invalidated by DROP/TRUNCATE/RENAME.
    bool is_reconciled(const std::string& name);
    void mark_reconciled(const std::string& name);
    void drop_reconciled(const std::string& name);
    void clear_reconciled();

    // Approximate cross-handler row count cache; the per-handler cache
    // dies on close() (per-statement in tight loops). HA_STATS_RECORDS_IS_EXACT
    // is unset so staleness only affects optimizer planning. Mutating
    // handlers invalidate; the next reader re-counts.
    bool records_lookup(const std::string& name, uint64_t* out);
    void records_set(const std::string& name, uint64_t count);
    void records_invalidate(const std::string& name);
    void records_drop(const std::string& name);

    // Process-wide AUTO_INCREMENT reservation. Per-connection counters
    // collide under concurrency / explicit INSERTs.
    // step/offset_mod model MariaDB's auto_increment_increment/offset
    // for multi-writer replication: the cursor must advance past EVERY
    // id MariaDB will logically issue, not just `count` contiguous ones.
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

// Stoolap status code -> short label, for logs.
const char* status_label(int32_t code);

// Status counters; relaxed atomics, snapshot by SHOW STATUS.
struct PushdownStats {
    std::atomic<uint64_t> pushdown_hits{0};
    std::atomic<uint64_t> pushdown_misses{0};
    std::atomic<uint64_t> buffered_scans{0};  // rnd_init via fetch_all
    std::atomic<uint64_t> buffered_rows{0};
    std::atomic<uint64_t> direct_modify_hits{0};
    // cached_records() miss that hit stoolap_(tx_)table_count (O(1)).
    // High values = the handler/tx/global caches aren't being reused.
    std::atomic<uint64_t> records_live_counts{0};
    // BOTH typed errcode AND prose pattern came back generic for a
    // non-empty stoolap message. case_18 pins this at 0.
    std::atomic<uint64_t> unmapped_errors{0};
    // Typed errcode was GENERIC but prose still classified the message.
    // Stoolap-side typed-error gap; not a plugin regression.
    std::atomic<uint64_t> typed_fallback_hits{0};

    // Per-phase ns; gated by g_perf_trace_enabled (~50ns/next_row off-path).
    std::atomic<uint64_t> perf_factory_setup_ns{0};
    std::atomic<uint64_t> perf_eager_query_ns{0};
    std::atomic<uint64_t> perf_init_scan_ns{0};
    std::atomic<uint64_t> perf_next_row_ns{0};
    std::atomic<uint64_t> perf_end_scan_ns{0};
    std::atomic<uint64_t> perf_query_count{0};
    std::atomic<uint64_t> perf_next_row_count{0};
};

// Hot-path read with memory_order_relaxed; bool read + branch, free off.
extern std::atomic<bool> g_perf_trace_enabled;

extern PushdownStats g_stats;

}  // namespace stoolap_mariadb
