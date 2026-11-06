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

#include "stoolap_mariadb.h"

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

struct CursorDeleter {
    void operator()(StoolapCursor* p) const noexcept {
        if (p) stoolap_cursor_close(p);
    }
};

using DbPtr = std::unique_ptr<StoolapDB, DbDeleter>;
using TxPtr = std::unique_ptr<StoolapTx, TxDeleter>;
using RowsPtr = std::unique_ptr<StoolapRows, RowsDeleter>;
using CursorPtr = std::unique_ptr<StoolapCursor, CursorDeleter>;

struct RowBufferDeleter {
    void operator()(StoolapRowBuffer* p) const noexcept {
        if (p) stoolap_row_buffer_free(p);
    }
};
using RowBufferPtr = std::unique_ptr<StoolapRowBuffer, RowBufferDeleter>;

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
    std::atomic<uint64_t> buffered_scans{0};  // direct reads (stoolap_read)
    std::atomic<uint64_t> buffered_rows{0};
    std::atomic<uint64_t> direct_modify_hits{0};
    // cached_records() miss that ran a live COUNT(*).
    // High values = the handler/tx/global caches aren't being reused.
    std::atomic<uint64_t> records_live_counts{0};
    // Non-empty stoolap message no stoolap_error_class rule covers.
    // case_18 pins this at 0.
    std::atomic<uint64_t> unmapped_errors{0};

    // Per-phase ns; gated by g_perf_trace_enabled (~50ns/next_row off-path).
    std::atomic<uint64_t> perf_factory_setup_ns{0};
    std::atomic<uint64_t> perf_eager_query_ns{0};
    std::atomic<uint64_t> perf_init_scan_ns{0};
    std::atomic<uint64_t> perf_next_row_ns{0};
    std::atomic<uint64_t> perf_end_scan_ns{0};
    std::atomic<uint64_t> perf_query_count{0};
    std::atomic<uint64_t> perf_next_row_count{0};
};

// `flat`, the escaped (db, table) name, as stoolap holds it: a long one
// shortened (stoolap_fit_table_name).
inline std::string fit_table_name(std::string flat) {
    char out[STOOLAP_MAX_TABLE_NAME];
    const size_t n = stoolap_fit_table_name(flat.data(), flat.size(), out);
    if (n) flat.assign(out, n);
    return flat;
}

// Stoolap's name for the table at MariaDB's path "./db/tbl" (a file path:
// its names as tablename_to_filename encodes them). `_` becomes `_0` and
// `/` becomes `_1`, so distinct (db, tbl) pairs never collide (a naive
// `/` -> `__` meets db=`p`,tbl=`q__r` and db=`p__q`,tbl=`r`); one past
// stoolap's name limit is shortened (fit_table_name).
inline std::string stoolap_table_from_path(const char* mariadb_path) {
    if (!mariadb_path) return std::string();
    std::string_view s(mariadb_path);
    if (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.remove_prefix(2);
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '_')
            out.append("_0");
        else if (c == '/')
            out.append("_1");
        else
            out.push_back(c);
    }
    return fit_table_name(std::move(out));
}

// Hot-path read with memory_order_relaxed; bool read + branch, free off.
extern std::atomic<bool> g_perf_trace_enabled;

extern PushdownStats g_stats;

}  // namespace stoolap_mariadb
