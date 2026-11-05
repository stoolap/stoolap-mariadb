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

    // cond_push: return value is the unhandled remainder MariaDB must
    // apply on top; NULL means "engine handled everything". We accept
    // only the direct-UPDATE/DELETE path where stoolap re-evaluates the
    // WHERE; a SELECT-row-pump cond translator is future work (returning
    // null without filtering would mass-mutate the table).
    const COND* cond_push(const COND* cond) override;
    int direct_update_rows_init(List<Item>* update_fields) override;
    int direct_update_rows(ha_rows* update_rows, ha_rows* found_rows) override;
    int direct_delete_rows_init() override;
    int direct_delete_rows(ha_rows* delete_rows) override;

    // 11.4 gates direct DML on these (base impl returns
    // HA_ERR_WRONG_COMMAND). We do the modify in pre_direct_*_rows and
    // stash the count so direct_*_rows can short-circuit if called.
    int pre_direct_update_rows_init(List<Item>* update_fields) override;
    int pre_direct_update_rows() override;
    int pre_direct_delete_rows_init() override;
    int pre_direct_delete_rows() override;

    // Bulk INSERT: accumulate write_row params into one batch, inserted
    // directly in one call (stoolap_stmt_exec_batch for a table a foreign
    // key names).
    void start_bulk_insert(ha_rows rows, uint flags) override;
    int end_bulk_insert() override;
    // Eager flush at kBulkFlushRows threshold; bulk_active_ stays true.
    int flush_bulk_buffer(bool partial = false);
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

    // Push BOTH bounds in one ranged SELECT (default impl streams past
    // the end bound and discards rows MariaDB-side).
    int read_range_first(const key_range* start_key, const key_range* end_key,
                         bool eq_range, bool sorted) override;
    int read_range_next() override;

    int info(uint flag) override;
    ha_rows records() override;
    ha_rows records_in_range(uint inx, const key_range* min_key,
                             const key_range* max_key,
                             page_range* res) override;

protected:
    // Inflate ref-cost on ci-leading keys so the planner doesn't pick
    // a small-outer / large-ci-inner join that drives O(outer*inner)
    // index_next full scans.
    IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                                 ulonglong blocks) override;

    // Default scan_time uses stats.data_file_length (always 0 here);
    // approximate as stats.records so keyread_time has a real number.
    IO_AND_CPU_COST scan_time() override;

public:
    void get_auto_increment(ulonglong offset, ulonglong increment,
                            ulonglong nb_desired_values, ulonglong* first_value,
                            ulonglong* nb_reserved_values) override;

    THR_LOCK_DATA** store_lock(THD* thd, THR_LOCK_DATA** to,
                               enum thr_lock_type lock_type) override;
    int external_lock(THD* thd, int lock_type) override;

private:
    // Lazy per-handler clone; main path is the THD-context clone (warm,
    // per-connection, keeps stoolap's parse cache warm across statements).
    // db_raw() may return null; db_ensure() lazy-allocates.
    stoolap_mariadb::DbPtr db_;
    StoolapDB* db_raw() const { return db_.get(); }
    StoolapDB* db_ensure();

    // THD ctx clone for DDL (falls back to db_ensure on no-THD paths).
    // A single StoolapDB* must not be used concurrently per the C ABI;
    // routing DDL through a clone keeps DDL on different connections
    // from racing the global executor / error buffer.
    StoolapDB* ddl_db();

    // Every read is a direct read (stoolap_read) into cursor_, which the
    // handler keeps for all its reads: rnd_next parses the cursor's current
    // packed chunk, which the cursor owns, and fetches the next when it
    // runs out and the read has more (reading_). Chunks start small when
    // the statement has a LIMIT and grow.
    stoolap_mariadb::CursorPtr cursor_;
    bool reading_ = false;
    uint32_t cursor_chunk_ = 0;
    StoolapCursor* read_cursor();
    int load_chunk(int rc, const uint8_t* buf, int64_t len);
    void end_read() {
        if (reading_) stoolap_cursor_end(cursor_.get());
        reading_ = false;
    }
    // At the end of a scan: also lets go of a buffer a large chunk grew,
    // which a handler cached with its TABLE would keep otherwise.
    void release_read() {
        reset_scan_state();
        if (cursor_) stoolap_cursor_end(cursor_.get());
    }
    int open_read(const StoolapCond* conds, int32_t nconds,
                  const int32_t* order, int32_t norder, bool desc,
                  uint32_t first_chunk = 0);
    int index_end_read(uchar* buf, bool desc);
    int next_chunk();
    const uint8_t* pkt_ = nullptr;
    size_t pkt_len_ = 0;
    // stoolap's row id of the row the last read returned, which
    // update_row / delete_row / position name it by. Not valid after a
    // write.
    int64_t cur_row_id_ = 0;
    bool cur_row_id_valid_ = false;
    // Direct writes; -1 for a table a foreign key names (write via SQL).
    int insert_direct(const StoolapValue* values, int32_t nrows,
                      int64_t* affected);
    int update_row_direct(const uchar* old_data, const uchar* new_data);
    int write_error(StoolapDB* db, const char* what);
    StoolapDB* direct_write_db();
    bool key_row_id(const uchar* record, int64_t* id);
    size_t scan_buf_pos_ = 0;
    uint32_t scan_buf_cols_ = 0;
    uint32_t scan_buf_rows_left_ = 0;
    // Cell -> field map of the read's columns.
    std::vector<int> scan_buf_cell_to_field_;

    // Centralized reset: a stale cursor left over from a prior read would
    // shadow a fresh one. Every scan entry point routes through here.
    void reset_scan_state() {
        end_read();
        pkt_ = nullptr;
        pkt_len_ = 0;
        scan_buf_pos_ = 0;
        scan_buf_cols_ = 0;
        scan_buf_rows_left_ = 0;
        key_filter_active_ = false;
        range_filter_active_ = false;
    }

    std::string stoolap_table_;

    // get_dup_key() resets errkey then calls info(HA_STATUS_ERRKEY); we
    // stash the most recent dup key here. Reset to MAX_KEY on each write.
    uint last_dup_key_ = MAX_KEY;

    // Sticky from index_init: false lets index_read_map skip ORDER BY.
    bool index_sorted_ = false;

    // index_read_map sets these for an exact lookup on a key part stoolap
    // cannot compare as MariaDB does (see key_part_needs_filter): those
    // parts stay out of the engine WHERE, and index_next keeps only rows
    // whose key key_cmp() matches against this copy of the search key.
    bool key_filter_active_ = false;
    KEY* key_filter_key_ = nullptr;
    std::vector<uchar> key_filter_buf_;

    // read_range_filtered: a range over a key part stoolap cannot order
    // as MariaDB does. read_range_next keeps the rows MariaDB's key
    // comparison puts inside the start bound (this copy) and end_range.
    int read_range_filtered(const key_range* start_key,
                            const key_range* end_key, bool sorted);
    bool range_in_bounds();
    bool range_filter_active_ = false;
    std::vector<uchar> range_start_buf_;
    uint range_start_len_ = 0;
    ha_rkey_function range_start_flag_ = HA_READ_KEY_EXACT;
    bool range_has_start_ = false;

    // pre_direct_*_rows ran the modify and stashed the count.
    bool direct_modify_in_pre_ = false;
    ha_rows direct_modify_affected_pre_ = 0;

    // The direct read's projection, the columns of its cells, rebuilt
    // only when read_set | write_set changes (a join probes the same
    // columns once per outer row).
    void refresh_projection();
    std::vector<uint32_t> proj_words_scratch_;
    std::vector<uint32_t> read_cols_words_;
    std::vector<int32_t> read_cols_;

    // index_read_map's key conditions and ORDER BY columns, kept to reuse
    // their storage across the probes of a join; ir_text_ holds the text
    // the condition values point into.
    std::vector<StoolapCond> ir_conds_;
    std::vector<int32_t> ir_order_;
    std::vector<std::string> ir_text_;

    // INSERT SQL template; stoolap's semantic cache hashes it.
    std::string insert_sql_;

    // Bulk-INSERT prepared stmt: cached because the batch ABI needs a
    // stmt handle, not because stoolap's cache wouldn't reuse the SQL.
    stoolap_mariadb::StmtPtr bulk_insert_stmt_;

    bool bulk_active_ = false;
    // True when the chunk-flush path opened the outer tx (autocommit
    // bulk INSERT); flush_bulk_buffer rolls back on failure.
    bool bulk_owns_tx_ = false;
    std::vector<StoolapValue> bulk_params_;
    // deque, not vector: bulk_params_[i].text.ptr points into the i-th
    // holder; vector realloc would dangle short-string pointers.
    std::deque<std::string> bulk_text_holders_;

    // Approximate row-count cache (HA_STATS_RECORDS_IS_EXACT isn't set).
    // Invalidated by local writes; cross-connection writes leave it
    // stale until the next local mutation -- acceptable for stats.
    ha_rows cached_records_ = 0;
    bool cached_records_valid_ = false;

    ha_rows cached_records();
    void invalidate_records_cache();
    void adjust_records_cache(int64_t delta);
    // All count-changing mutation sites MUST route through these wrappers.
    // Direct cached_records_ / g_engine.records_* writes are bugs.
    void apply_count_delta(int64_t delta);
    void set_count_exact(uint64_t value);
};

// Stoolap name of the table behind `share`, derived from its path the
// same way create() and open() derive it.
std::string stoolap_table_name(const TABLE_SHARE* share);
