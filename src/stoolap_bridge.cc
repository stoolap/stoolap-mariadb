/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "stoolap_bridge.h"

#include <string>

namespace stoolap_mariadb {

int Engine::open(std::string_view dsn) {
    StoolapDB* raw = nullptr;
    std::string dsn_z(dsn);
    int32_t rc = stoolap_open(dsn_z.c_str(), &raw);
    if (rc != STOOLAP_OK) {
        const char* msg = stoolap_errmsg(nullptr);
        last_error_.assign(msg ? msg : "stoolap_open failed");
        return rc;
    }
    db_.reset(raw);
    return STOOLAP_OK;
}

void Engine::close() noexcept {
    db_.reset();
}

DbPtr Engine::clone_handle() const {
    if (!db_) return nullptr;
    StoolapDB* raw = nullptr;
    if (stoolap_clone(db_.get(), &raw) != STOOLAP_OK) {
        return nullptr;
    }
    return DbPtr(raw);
}

bool Engine::is_reconciled(const std::string& name) {
    std::lock_guard<std::mutex> lk(reconciled_mu_);
    return reconciled_.count(name) > 0;
}

void Engine::mark_reconciled(const std::string& name) {
    std::lock_guard<std::mutex> lk(reconciled_mu_);
    reconciled_.insert(name);
}

void Engine::drop_reconciled(const std::string& name) {
    std::lock_guard<std::mutex> lk(reconciled_mu_);
    reconciled_.erase(name);
}

void Engine::clear_reconciled() {
    std::lock_guard<std::mutex> lk(reconciled_mu_);
    reconciled_.clear();
}

bool Engine::records_lookup(const std::string& name, uint64_t* out) {
    std::lock_guard<std::mutex> lk(records_mu_);
    auto it = records_.find(name);
    if (it == records_.end()) return false;
    if (it->second == UINT64_MAX) return false;  // invalidated
    if (out) *out = it->second;
    return true;
}

void Engine::records_set(const std::string& name, uint64_t count) {
    std::lock_guard<std::mutex> lk(records_mu_);
    records_[name] = count;
}

void Engine::records_invalidate(const std::string& name) {
    std::lock_guard<std::mutex> lk(records_mu_);
    auto it = records_.find(name);
    if (it != records_.end()) it->second = UINT64_MAX;
}

void Engine::records_drop(const std::string& name) {
    std::lock_guard<std::mutex> lk(records_mu_);
    records_.erase(name);
}

namespace {
// Round `cur` up to the smallest value satisfying (val % step == offset_mod).
// Saturates to UINT64_MAX on overflow rather than wrapping (an overflowed
// AI cursor will fail the next INSERT cleanly via duplicate-key, which is
// preferable to silently issuing wrong ids).
inline uint64_t round_up_to_modulus(uint64_t cur, uint64_t step,
                                    uint64_t offset_mod) {
    if (step <= 1) return cur;
    if (offset_mod >= step) offset_mod %= step;
    const uint64_t cur_mod = cur % step;
    if (cur_mod == offset_mod) return cur;
    const uint64_t bump = (offset_mod > cur_mod)
                              ? (offset_mod - cur_mod)
                              : (step - (cur_mod - offset_mod));
    if (bump > UINT64_MAX - cur) return UINT64_MAX;
    return cur + bump;
}

// Compute ai_next_ advance for a step-aware reservation. Reserves `count`
// values stepped by `step` starting at `first`. The cursor advances past
// the *last* issued value so a concurrent session never collides with
// any id MariaDB will use within the current statement's batch.
inline uint64_t advance_after_reservation(uint64_t first, uint64_t count,
                                          uint64_t step) {
    if (count <= 1) return (first == UINT64_MAX) ? UINT64_MAX : first + 1;
    // last = first + (count - 1) * step
    const uint64_t span_factor = count - 1;
    if (step != 0 && span_factor > UINT64_MAX / step) return UINT64_MAX;
    const uint64_t span = span_factor * step;
    if (span > UINT64_MAX - first) return UINT64_MAX;
    const uint64_t last = first + span;
    return (last == UINT64_MAX) ? UINT64_MAX : last + 1;
}
}  // namespace

bool Engine::ai_reserve(const std::string& name, uint64_t count, uint64_t step,
                        uint64_t offset_mod, uint64_t* first) {
    if (count == 0) count = 1;
    if (step == 0) step = 1;
    std::lock_guard<std::mutex> lk(ai_mu_);
    auto it = ai_next_.find(name);
    if (it == ai_next_.end()) return false;
    const uint64_t f = round_up_to_modulus(it->second, step, offset_mod);
    if (first) *first = f;
    it->second = advance_after_reservation(f, count, step);
    return true;
}

void Engine::ai_seed_and_reserve(const std::string& name, uint64_t seed,
                                 uint64_t count, uint64_t step,
                                 uint64_t offset_mod, uint64_t* first) {
    if (count == 0) count = 1;
    if (step == 0) step = 1;
    std::lock_guard<std::mutex> lk(ai_mu_);
    uint64_t& next = ai_next_[name];
    if (next < seed) next = seed;
    const uint64_t f = round_up_to_modulus(next, step, offset_mod);
    if (first) *first = f;
    next = advance_after_reservation(f, count, step);
}

void Engine::ai_note_explicit(const std::string& name, uint64_t value) {
    if (value == UINT64_MAX) return;
    std::lock_guard<std::mutex> lk(ai_mu_);
    uint64_t& next = ai_next_[name];
    if (next <= value) next = value + 1;
}

void Engine::ai_invalidate() {
    std::lock_guard<std::mutex> lk(ai_mu_);
    ai_next_.clear();
}

const char* status_label(int32_t code) {
    switch (code) {
        case STOOLAP_OK:
            return "OK";
        case STOOLAP_ERROR:
            return "ERROR";
        case STOOLAP_ROW:
            return "ROW";
        case STOOLAP_DONE:
            return "DONE";
        default:
            return "UNKNOWN";
    }
}

PushdownStats g_stats{};
std::atomic<bool> g_perf_trace_enabled{false};

}  // namespace stoolap_mariadb
