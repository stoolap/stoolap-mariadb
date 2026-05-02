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

#include "my_global.h"
#include "handler.h"
#include "table.h"
#include "thr_lock.h"

#include "stoolap_bridge.h"

#include <deque>
#include <string>
#include <vector>

class ha_stoolap final : public handler {
public:
    ha_stoolap(handlerton* hton, TABLE_SHARE* table_arg);
    ~ha_stoolap() override = default;

    const char* table_type() const override { return "STOOLAP"; }

    ulonglong table_flags() const override;
    ulong index_flags(uint inx, uint part, bool all_parts) const override;

    uint max_supported_keys() const override { return 16; }
    uint max_supported_key_length() const override { return 1024; }
    uint max_supported_key_part_length() const override { return 1024; }

    int open(const char* name, int mode, uint test_if_locked) override;
    int close() override;

    int create(const char* name, TABLE* form,
               HA_CREATE_INFO* create_info) override;
    int delete_table(const char* name) override;
    int rename_table(const char* from, const char* to) override;

    int write_row(const uchar* buf) override;
    int update_row(const uchar* old_data, const uchar* new_data) override;
    int delete_row(const uchar* buf) override;
    int delete_all_rows() override;

    // Direct UPDATE / DELETE: MariaDB hands the entire statement to us in
    // one call instead of scanning matching rows itself and calling back
    // per row. Implementation forwards the rewritten SQL to stoolap and
    // returns the affected count.
    //
    // cond_push contract: return value is the *unhandled remainder* the
    // engine couldn't push, which MariaDB must apply on top. NULL means
    // "I accepted everything and will only return matching rows".
    //
    // Tight scope today:
    //   - UPDATE / DELETE: when the WHERE only references columns
    //     stoolap and MariaDB compare byte-wise (numeric, date/time,
    //     binary-collated strings -- or any string when the user has
    //     SET stoolap_trust_binary_strings = 1), we accept the WHERE
    //     and return null. try_direct_modify forwards thd->query()
    //     verbatim, so stoolap re-evaluates the WHERE itself. This
    //     closes the gate that was keeping ha_direct_*_rows out of
    //     reach for any UPDATE / DELETE with a real WHERE clause.
    //   - SELECT: we still return cond. Real WHERE-to-SQL translation
    //     for the row-pump scan path is future work; pushing the same
    //     trick blindly there would tell MariaDB we filtered when we
    //     didn't, returning wrong rows.
    const COND* cond_push(const COND* cond) override;
    int direct_update_rows_init(List<Item>* update_fields) override;
    int direct_update_rows(ha_rows* update_rows, ha_rows* found_rows) override;
    int direct_delete_rows_init() override;
    int direct_delete_rows(ha_rows* delete_rows) override;

    // MariaDB 11.4's UPDATE/DELETE path gates direct DML on the engine
    // accepting the pre_direct_*_init hooks first; the default base
    // implementation returns HA_ERR_WRONG_COMMAND, so without these
    // overrides the direct path is never reached and statements fall
    // back to the per-row callback loop. Some call sites then run the
    // actual modify in pre_direct_*_rows (instead of direct_*_rows);
    // we run the modify in pre_direct_*_rows and stash the affected
    // count, so direct_*_rows can short-circuit if also called.
    int pre_direct_update_rows_init(List<Item>* update_fields) override;
    int pre_direct_update_rows() override;
    int pre_direct_delete_rows_init() override;
    int pre_direct_delete_rows() override;

    // Bulk INSERT: MariaDB calls start_bulk_insert before a multi-row
    // INSERT or INSERT ... SELECT, then a series of write_row, then
    // end_bulk_insert. We accumulate rows in a single packed parameter
    // vector and ship them via stoolap_stmt_exec_batch -- one FFI call
    // and one transaction, instead of N of each.
    void start_bulk_insert(ha_rows rows, uint flags) override;
    int end_bulk_insert() override;
    /** Eager flush of the buffered batch. Called from write_row when
     *  bulk_params_ crosses the row threshold (kBulkFlushRows in the
     *  .cc) so memory doesn't grow proportional to the full statement
     *  for large INSERT ... VALUES, INSERT ... SELECT, and LOAD DATA.
     *  Keeps bulk_active_ true; the prepared StoolapStmt persists on
     *  the THD context across flushes so each chunk reuses the same
     *  parsed plan. */
    int flush_bulk_buffer();
    int analyze(THD* thd, HA_CHECK_OPT* check_opt) override;

    int rnd_init(bool scan) override;
    int rnd_next(uchar* buf) override;
    int rnd_pos(uchar* buf, uchar* pos) override;
    int rnd_end() override;
    void position(const uchar* record) override;

    int index_init(uint keynr, bool sorted) override;
    int index_end() override;
    int index_read_map(uchar* buf, const uchar* key, key_part_map keypart_map,
                       enum ha_rkey_function find_flag) override;
    int index_next(uchar* buf) override;
    int index_first(uchar* buf) override;
    int index_last(uchar* buf) override;

    // Default read_range_first/next call index_read_map(start) and then
    // per-row check compare_key(end) -- which means MariaDB streams rows
    // past the end bound from stoolap and discards them. Override to push
    // BOTH bounds to stoolap in a single ranged SELECT, so stoolap stops
    // scanning at the end of the requested range.
    int read_range_first(const key_range* start_key, const key_range* end_key,
                         bool eq_range, bool sorted) override;
    int read_range_next() override;

    int info(uint flag) override;
    ha_rows records() override;
    ha_rows records_in_range(uint inx, const key_range* min_key,
                             const key_range* max_key,
                             page_range* res) override;

protected:
    /** Cost gate for ci-leading-key indexes. The handler runs a full
     *  table scan in index_next per ref probe (only collation-correct
     *  option without a stoolap-side ci index), so the optimizer must
     *  see ref access on those keys as no cheaper than a full scan.
     *  Without this, the planner can put a small outer table on the
     *  outside and probe a large ci-keyed inner once per outer row,
     *  driving O(outer * inner) scans where it expected O(outer). */
    IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                                 ulonglong blocks) override;

    /** Scan cost. Default impl uses stats.data_file_length, which we
     *  leave at 0 (stoolap is in-memory or remote); without an
     *  override the optimizer sees scan_time = 0 and free ref access.
     *  Approximate as `stats.records` "cost units" per scan, which
     *  gives keyread_time a real number to multiply by for the ci
     *  cost gate. */
    IO_AND_CPU_COST scan_time() override;

public:
    void get_auto_increment(ulonglong offset, ulonglong increment,
                            ulonglong nb_desired_values, ulonglong* first_value,
                            ulonglong* nb_reserved_values) override;

    THR_LOCK_DATA** store_lock(THD* thd, THR_LOCK_DATA** to,
                               enum thr_lock_type lock_type) override;
    int external_lock(THD* thd, int lock_type) override;

private:
    /** Per-handler stoolap handle, allocated lazily.
     *
     *  Normal handler ops route through the THD-context clone, which is
     *  longer-lived (per connection) and keeps stoolap's parsed-query
     *  cache warm across statements. The per-handler db_ exists only as
     *  a fallback for paths that can't reach a THD ctx, and DOES NOT get
     *  allocated in open() -- a fresh handler instance is cheap to create.
     *
     *  - `db_raw()` returns the current pointer, possibly null. Use this
     *    when passing to warm_db() as the fallback: warm_db prefers the
     *    THD ctx, so a null fallback is fine there.
     *  - `db_ensure()` lazy-allocates and returns non-null (or null on
     *    OOM). Use this in DDL paths that genuinely need a handle. */
    stoolap_mariadb::DbPtr db_;
    StoolapDB* db_raw() const { return db_.get(); }
    StoolapDB* db_ensure();

    /** DDL-scope StoolapDB*: prefers the THD context clone (warm, also
     *  used for this connection's DML), falls back to db_ensure() if
     *  the call site has no THD. Required to honour the Stoolap C ABI
     *  contract that a single handle must not be used concurrently;
     *  routing every DDL through a THD-local clone keeps two
     *  CREATE/DROP/RENAME calls on different connections from racing
     *  the global executor / error buffer. */
    StoolapDB* ddl_db();

    /** Active scan cursor, set up by rnd_init / drained by rnd_next. */
    stoolap_mariadb::RowsPtr scan_;

    /** Pre-fetched scan buffer (packed binary from stoolap_rows_fetch_all).
     *  When `scan_buf_` holds data, rnd_next parses rows out of it
     *  in-process instead of issuing one FFI call per row + N more per
     *  cell. Tradeoff: we eagerly materialise the whole result set, so
     *  this is only used for full table scans (rnd_init), where MariaDB
     *  is going to stream the rows out anyway. Index probes and ranged
     *  scans stay on the streaming path because they often LIMIT early.
     *
     *  The StoolapBuffer wrapper takes direct ownership of the buffer
     *  stoolap allocated, so we skip the memcpy that an std::vector
     *  copy would impose on a multi-megabyte fetch. */
    stoolap_mariadb::StoolapBuffer scan_buf_;
    size_t scan_buf_pos_ = 0;          // current parse offset
    uint32_t scan_buf_cols_ = 0;       // column count from header
    uint32_t scan_buf_rows_left_ = 0;  // rows still to deliver
    /** Inverse of scan_proj_: maps a stoolap result cell back to the
     *  MariaDB field index. Built once in rnd_init when projection is
     *  narrower than `*`, so the buffered rnd_next can store each cell
     *  into the right field without scanning scan_proj_ per row. */
    std::vector<int> scan_buf_cell_to_field_;

    /** Drop both the streaming cursor and the buffered scan state.
     *
     *  rnd_next checks `scan_buf_` first and only falls through to
     *  `scan_` when the buffer is empty. If a previous `rnd_init`
     *  populated scan_buf_ and then the handler is re-entered via the
     *  index path (index_init / index_first / index_read_map / ...) WITHOUT
     *  the intervening rnd_end -- e.g. a future executor reuse pattern
     *  or any path that opens a fresh stream without ending the prior
     *  one -- a freshly-assigned `scan_` would be silently shadowed by
     *  the stale buffer and rnd_next would deliver rows from the prior
     *  scan. Centralizing the reset here keeps the invariant trivial
     *  to enforce at every scan entry point. */
    void reset_scan_state() {
        scan_.reset();
        scan_buf_.reset();
        scan_buf_pos_ = 0;
        scan_buf_cols_ = 0;
        scan_buf_rows_left_ = 0;
        scan_buf_cell_to_field_.clear();
        ci_collation_filter_active_ = false;
        ci_collation_key_info_ = nullptr;
        ci_collation_nparts_ = 0;
    }

    /** Stoolap-side table name derived from the path passed to open()/create(). */
    std::string stoolap_table_;

    /** MariaDB's get_dup_key() resets `errkey`, then calls info() with
     *  HA_STATUS_ERRKEY expecting it to be re-populated. We remember the
     *  most recent constraint-violation key here so info() can hand it
     *  back. Reset to MAX_KEY on every fresh write. */
    uint last_dup_key_ = MAX_KEY;

    /** Build the SELECT-list for a fresh scan: each Field that's in read_set
     *  or write_set becomes a quoted column name, and scan_proj_ is updated
     *  so rnd_next can map back from Field index to result-column index.
     *  When read_set is empty (e.g. SELECT COUNT(*) WHERE ...) the SELECT
     *  list collapses to a literal sentinel so we still get one row per
     *  stoolap row but with minimal FFI traffic. */
    std::string build_scan_columns();

    /** Sticky `sorted` flag from index_init: when false the caller (an
     *  exact secondary-key probe, ref-join, etc.) doesn't need rows in
     *  index order, so index_read_map can skip ORDER BY and let stoolap's
     *  planner pick the cheapest scan. */
    bool index_sorted_ = false;

    /** Set by index_read_map when the leading key part is a ci-collated
     *  string column and the user has not opted into byte semantics.
     *  In that mode index_read_map drops the engine-side WHERE and
     *  index_next ci-compares EVERY bound key part of each fetched row
     *  against the search key bytes (kept in table->record[1] by
     *  key_restore), using each Field's real CHARSET_INFO comparator.
     *  All-parts-must-match guards composite ci-leading keys like
     *  KEY(s, n): if we only compared the leading part, a JOIN ON
     *  st.s = so.s AND st.n = so.n would emit rows whose `n` differs
     *  from the search key. The flag is reset in index_init /
     *  index_end / reset_scan_state. */
    bool ci_collation_filter_active_ = false;
    KEY* ci_collation_key_info_ = nullptr;
    uint ci_collation_nparts_ = 0;

    /** True when any field on this table is a BLOB / TEXT type. Set in
     *  open(). When true, rnd_pos() must NOT memcpy the captured ref
     *  back into record[0]: the BLOB Field's in-record bytes are a
     *  (length, pointer) trio whose pointer references memory owned
     *  by the prior rnd_next's stoolap value-store. The next rnd_next
     *  overwrites that allocation, so blindly restoring the bytes
     *  resurrects a freed pointer. Until rnd_pos is rebuilt to
     *  re-fetch from stoolap by primary key, fail the call rather
     *  than silently return garbage. */
    bool has_blob_field_ = false;

    /** Set when pre_direct_update_rows or pre_direct_delete_rows ran the
     *  modify. If MariaDB then also calls direct_*_rows we short-circuit
     *  with the stashed affected count so we don't double-execute. */
    bool direct_modify_in_pre_ = false;
    ha_rows direct_modify_affected_pre_ = 0;

    /** For each Field index i, the position of that column in the current
     *  scan's stoolap result set, or -1 if the column was not projected.
     *  Set by every scan-opener (rnd_init, read_range_first, index_*) just
     *  before the SELECT runs; consumed by rnd_next when reading values.
     *  Lets us issue narrow projections (only the columns MariaDB's
     *  read_set/write_set actually need) instead of `SELECT *` and keep
     *  the field->column index mapping straight afterwards. */
    std::vector<int> scan_proj_;

    /** Cached INSERT SQL template `INSERT INTO "tbl" VALUES ($1, $2, ...)`.
     *  Per-row write_row uses this string directly; stoolap's semantic
     *  cache hashes it and reuses the parsed plan across calls. */
    std::string insert_sql_;

    /** Single prepared StoolapStmt for the bulk INSERT path (used by
     *  stoolap_stmt_exec_batch / stoolap_tx_stmt_exec inside
     *  end_bulk_insert). Prepared lazily on first flush and reused
     *  across subsequent batches on the same handler instance. Finalized
     *  via the StmtPtr destructor on close(). The bulk path is the only
     *  place that genuinely needs a StoolapStmt -- the batch ABI
     *  requires a statement handle -- so caching it isn't a duplicate of
     *  stoolap's semantic cache, it's just avoiding the prepare
     *  round-trip per batch. */
    stoolap_mariadb::StmtPtr bulk_insert_stmt_;

    // Bulk INSERT state. Active between start_bulk_insert and
    // end_bulk_insert: write_row appends row params here instead of
    // executing per row. Flushed via stoolap_stmt_exec_batch.
    bool bulk_active_ = false;
    /** True when the chunk-flush path opened a stoolap transaction on
     *  behalf of an autocommit bulk INSERT. Read at flush_bulk_buffer
     *  time to know we should rollback (rather than commit) on chunk
     *  failure; cleared at start_bulk_insert. */
    bool bulk_owns_tx_ = false;
    std::vector<StoolapValue> bulk_params_;
    // std::deque (not vector) so push_back never moves existing elements:
    // bulk_params_[i].text.ptr points into bulk_text_holders_[i].data(), and
    // for short-string-optimised values that pointer would dangle if the
    // vector reallocated mid-batch.
    std::deque<std::string> bulk_text_holders_;
    // Cached row count for records() and info(HA_STATUS_VARIABLE).
    // MariaDB's planner asks for this on every statement; without a
    // cache we'd issue SELECT COUNT(*) per query just for stats. The
    // cache is invalidated by mutating ops on this handler instance
    // (write_row, delete_row, delete_all_rows, end_bulk_insert,
    // direct_update/delete_rows). Cross-connection writes leave the
    // value stale until the next mutation here, which is fine for
    // approximate optimizer stats (stats.records is documented as
    // approximate; HA_STATS_RECORDS_IS_EXACT isn't set).
    ha_rows cached_records_ = 0;
    bool cached_records_valid_ = false;

    /// Read the cached row count, refreshing from stoolap on miss.
    /// Falls back to the previous stats.records on stoolap-side errors.
    ha_rows cached_records();
    /// Drop the cached count from handler-local plus tx-local/global layers
    /// -- next read re-queries.
    void invalidate_records_cache();
    /// Apply a successful INSERT/DELETE delta to local exact tx caches, or
    /// invalidate shared caches when no tx-local cache can safely own it.
    void adjust_records_cache(int64_t delta);
};
