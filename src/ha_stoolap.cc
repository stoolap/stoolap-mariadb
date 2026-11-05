/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#define MYSQL_SERVER 1

#include "ha_stoolap.h"

#include "my_global.h"
#include "mysql/plugin.h"
#include "field.h"
#include "key.h"
#include "sql_priv.h"  // OPTION_NOT_AUTOCOMMIT, OPTION_BEGIN
#include "sql_string.h"
#include "log.h"
#include "mysqld_error.h"  // ER_GET_ERRMSG (used by report_stoolap_error)

#include "stoolap_thd_context.h"
#include "stoolap_packet.h"
#include "ha_stoolap_select.h"
#include "stoolap_thd_inspect.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

handlerton* stoolap_hton = nullptr;
stoolap_mariadb::Engine g_engine;

// External linkage: ha_stoolap_select.cc's pushdown factories call
// register_trx() defensively before the eager query, since create_select
// can fire before any handler's external_lock.
stoolap_mariadb::ThdContext* get_thd_ctx(THD* thd) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) {
        ctx = new stoolap_mariadb::ThdContext(&g_engine);
        thd_set_ha_data(thd, stoolap_hton, ctx);
    }
    return ctx;
}

// REPEATABLE-READ / SERIALIZABLE map to STOOLAP_ISOLATION_SNAPSHOT;
// without this, MariaDB's default RR ran as read-committed inside stoolap
// and saw post-BEGIN commits from other sessions.
int register_trx(THD* thd) {
    auto* ctx = get_thd_ctx(thd);

    trans_register_ha(thd, /*all=*/false, stoolap_hton, /*flags=*/0);

    const bool in_explicit_txn =
        thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (in_explicit_txn) {
        trans_register_ha(thd, /*all=*/true, stoolap_hton, /*flags=*/0);
        if (!ctx->has_tx()) {
            const int iso = thd_tx_isolation(thd);
            const int32_t stoolap_iso =
                (iso == ISO_REPEATABLE_READ || iso == ISO_SERIALIZABLE)
                    ? STOOLAP_ISOLATION_SNAPSHOT
                    : STOOLAP_ISOLATION_READ_COMMITTED;
            int rc = ctx->begin(stoolap_iso);
            if (rc != STOOLAP_OK) {
                sql_print_error("stoolap: BEGIN failed: %s",
                                stoolap_errmsg(ctx->db()));
                return HA_ERR_GENERIC;
            }
        }
    }
    return 0;
}

// `message` remains valid only until the next FFI call on the originating
// handle, so consume the view inline before issuing any further FFI on
// that handle. Never NULL; normalised to "".
struct StoolapErrorView {
    const char* message;
};

StoolapErrorView fetch_db_error(StoolapDB* db);
StoolapErrorView fetch_tx_error(StoolapTx* tx);
int report_stoolap_error(const StoolapErrorView& v);
int raise_commit_error(const char* msg);

namespace {

/** System variable: DSN passed to stoolap_open() at plugin init. */
char* stoolap_dsn_var = nullptr;

handler* stoolap_create_handler(handlerton* hton, TABLE_SHARE* table,
                                MEM_ROOT* mem_root) {
    return new (mem_root) ha_stoolap(hton, table);
}

/* ---------- Handlerton callbacks ---------- */

int stoolap_commit_cb(handlerton* /*hton*/, THD* thd, bool all) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) return 0;
    if (ctx->end_statement(true) != STOOLAP_OK) {
        const char* msg = stoolap_errmsg(ctx->db());
        sql_print_error("stoolap: statement COMMIT failed: %s",
                        (msg && *msg) ? msg : "(no detail)");
        return raise_commit_error(msg);
    }

    // Real COMMIT is signalled either by `all=true` (explicit COMMIT) or by
    // the end-of-statement marker for an autocommit session.
    const bool real_commit =
        all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
    if (!real_commit || !ctx->has_tx()) return 0;

    int rc = ctx->commit();
    if (rc != STOOLAP_OK) {
        // stoolap_tx_commit consumes the tx handle; its error is left in
        // the thread-local slot stoolap_errmsg(NULL) reads.
        const char* msg = stoolap_errmsg(nullptr);
        sql_print_error("stoolap: COMMIT failed: %s",
                        (msg && *msg) ? msg : "(no detail)");
        return raise_commit_error(msg);
    }
    return 0;
}

int stoolap_rollback_cb(handlerton* /*hton*/, THD* thd, bool all) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (!ctx) return 0;
    if (ctx->end_statement(false) != STOOLAP_OK) {
        sql_print_error("stoolap: statement ROLLBACK failed: %s",
                        stoolap_errmsg(ctx->db()));
    }

    const bool real_rollback =
        all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
    if (!real_rollback || !ctx->has_tx()) return 0;

    int rc = ctx->rollback();
    if (rc != STOOLAP_OK) {
        sql_print_error("stoolap: ROLLBACK failed: %s",
                        stoolap_errmsg(ctx->db()));
        return HA_ERR_GENERIC;
    }
    return 0;
}

int stoolap_close_connection_cb(handlerton* /*hton*/, THD* thd) {
    auto* ctx = static_cast<stoolap_mariadb::ThdContext*>(
        thd_get_ha_data(thd, stoolap_hton));
    if (ctx) {
        delete ctx;
        thd_set_ha_data(thd, stoolap_hton, nullptr);
    }
    return 0;
}

// Anchor the snapshot at BEGIN time. Without this, the tx wouldn't
// open until the first statement's external_lock fires and a concurrent
// commit between BEGIN and the first SELECT would slide into the snapshot.
int stoolap_start_consistent_snapshot_cb(handlerton* /*hton*/, THD* thd) {
    auto* ctx = get_thd_ctx(thd);
    if (ctx->has_tx()) return 0;  // already open
    int rc = ctx->begin(STOOLAP_ISOLATION_SNAPSHOT);
    if (rc != STOOLAP_OK) {
        sql_print_error("stoolap: consistent snapshot BEGIN failed: %s",
                        stoolap_errmsg(ctx->db()));
        return HA_ERR_GENERIC;
    }
    trans_register_ha(thd, /*all=*/false, stoolap_hton, /*flags=*/0);
    trans_register_ha(thd, /*all=*/true, stoolap_hton, /*flags=*/0);
    return 0;
}

// Engine-private chunk MariaDB allocates per SAVEPOINT (sized via
// hton->savepoint_offset). MariaDB doesn't pass the user's name into
// our callback, so we synthesise "sp<id>" from a per-connection counter.
struct StoolapSavepointSlot {
    uint64_t id;
    uint8_t name_len;
    char name[24];  // "sp" + 20-digit uint64 max
};

void format_savepoint_slot(StoolapSavepointSlot* slot, uint64_t id) {
    slot->id = id;
    int n = std::snprintf(slot->name, sizeof(slot->name), "sp%llu",
                          static_cast<unsigned long long>(id));
    if (n < 0) n = 0;
    if (n >= static_cast<int>(sizeof(slot->name))) {
        n = static_cast<int>(sizeof(slot->name)) - 1;
    }
    slot->name_len = static_cast<uint8_t>(n);
}

// Runs "<verb> <slot name>" on the open tx; stoolap executes SAVEPOINT /
// RELEASE SAVEPOINT / ROLLBACK TO SAVEPOINT as SQL on a tx handle.
int exec_savepoint_sql(stoolap_mariadb::ThdContext* ctx, const char* verb,
                       const StoolapSavepointSlot* slot) {
    char sql[64];
    std::snprintf(sql, sizeof(sql), "%s %.*s", verb,
                  static_cast<int>(slot->name_len), slot->name);
    if (stoolap_tx_exec(ctx->tx(), sql, nullptr) != STOOLAP_OK) {
        auto verr = fetch_tx_error(ctx->tx());
        sql_print_error("stoolap: %s failed: %s", verb, verr.message);
        return report_stoolap_error(verr);
    }
    return 0;
}

int stoolap_savepoint_set_cb(handlerton* /*hton*/, THD* thd, void* sv) {
    auto* ctx = get_thd_ctx(thd);
    if (!ctx || !ctx->has_tx()) {
        sql_print_error("stoolap: SAVEPOINT without an active tx");
        return HA_ERR_GENERIC;
    }
    auto* slot = static_cast<StoolapSavepointSlot*>(sv);
    format_savepoint_slot(slot, ctx->next_savepoint_id());
    return exec_savepoint_sql(ctx, "SAVEPOINT", slot);
}

int stoolap_savepoint_release_cb(handlerton* /*hton*/, THD* thd, void* sv) {
    auto* ctx = get_thd_ctx(thd);
    // Tx already gone (commit/rollback ran first): savepoint dissolved with it.
    if (!ctx || !ctx->has_tx()) return 0;
    return exec_savepoint_sql(ctx, "RELEASE SAVEPOINT",
                              static_cast<StoolapSavepointSlot*>(sv));
}

int stoolap_savepoint_rollback_cb(handlerton* /*hton*/, THD* thd, void* sv) {
    auto* ctx = get_thd_ctx(thd);
    if (!ctx || !ctx->has_tx()) return 0;
    int rc = exec_savepoint_sql(ctx, "ROLLBACK TO SAVEPOINT",
                                static_cast<StoolapSavepointSlot*>(sv));
    if (rc) return rc;
    // We track count deltas at tx granularity, not per-savepoint, so any
    // adjusted count is now wrong; re-fetch on next read.
    ctx->invalidate_dirty_records();
    return 0;
}

/* ---------- Tx-aware execution helpers ---------- */

// THD-context db() is cloned per-connection so its parse cache stays
// warm across statements; ha_stoolap::db_ is reset per-handler-close.
static StoolapDB* warm_db(stoolap_mariadb::ThdContext* ctx,
                          StoolapDB* fallback) {
    if (ctx) {
        if (StoolapDB* d = ctx->db()) return d;
    }
    return fallback;
}

int query_via(stoolap_mariadb::ThdContext* ctx, StoolapDB* fallback,
              const char* sql, StoolapRows** out_rows) {
    if (ctx && ctx->has_tx()) {
        return stoolap_tx_query(ctx->tx(), sql, out_rows);
    }
    return stoolap_query(warm_db(ctx, fallback), sql, out_rows);
}

}  // namespace (helpers below have external linkage so
// ha_stoolap_select.cc can call them.)

namespace {

StoolapErrorView make_view(const char* msg) {
    return StoolapErrorView{msg ? msg : ""};
}

}  // namespace

StoolapErrorView fetch_db_error(StoolapDB* db) {
    return make_view(db ? stoolap_errmsg(db) : nullptr);
}

StoolapErrorView fetch_tx_error(StoolapTx* tx) {
    return make_view(tx ? stoolap_tx_errmsg(tx) : nullptr);
}

// MariaDB key behind stoolap's n-th table-level unique constraint, or
// MAX_KEY. Stoolap numbers CREATE TABLE's table-level PRIMARY KEY / UNIQUE
// clauses in order, and stoolap_create_table emits a composite PRIMARY KEY
// first, then each non-PK unique key in key order. A single-column PRIMARY
// KEY is inline and takes no number.
uint unique_key_by_ordinal(TABLE_SHARE* share, uint n) {
    if (share->primary_key < share->keys &&
        share->key_info[share->primary_key].user_defined_key_parts > 1) {
        if (n == 0) return share->primary_key;
        --n;
    }
    for (uint i = 0; i < share->keys; ++i) {
        if (i == share->primary_key) continue;
        if (!(share->key_info[i].flags & HA_NOSAME)) continue;
        if (n-- == 0) return i;
    }
    return MAX_KEY;
}

// The MariaDB key a duplicate names: the PRIMARY KEY, or the key behind
// the unique constraint stoolap numbers; else the PRIMARY KEY, or key 0.
uint errkey_from_view(const StoolapErrorView& v, TABLE_SHARE* share) {
    if (!share) return 0;
    int32_t key = STOOLAP_KEY_NONE;
    stoolap_error_class(v.message, &key);
    if (key >= 0) {
        const uint k = unique_key_by_ordinal(share, static_cast<uint>(key));
        if (k != MAX_KEY) return k;
    }
    if (share->primary_key < share->keys) return share->primary_key;
    return 0;
}

// The handler error of a stoolap error class.
int ha_error_of(int32_t cls) {
    switch (cls) {
        case STOOLAP_ERR_DUP_KEY:
            return HA_ERR_FOUND_DUPP_KEY;
        case STOOLAP_ERR_NO_REFERENCED_ROW:
            return HA_ERR_NO_REFERENCED_ROW;
        case STOOLAP_ERR_ROW_IS_REFERENCED:
            return HA_ERR_ROW_IS_REFERENCED;
        case STOOLAP_ERR_DEADLOCK:
            return HA_ERR_LOCK_DEADLOCK;
        case STOOLAP_ERR_TABLE_EXISTS:
            return HA_ERR_TABLE_EXIST;
        case STOOLAP_ERR_NO_SUCH_TABLE:
            return HA_ERR_NO_SUCH_TABLE;
        case STOOLAP_ERR_UNSUPPORTED:
            return HA_ERR_UNSUPPORTED;
        default:
            return HA_ERR_GENERIC;
    }
}

// Brewed mariadbd exports my_error / my_printf_error directly, but the
// plugin's mysql/plugin.h rewrites them through a service-pointer struct
// that isn't shipped. Undef + extern declarations bind to the real symbols.
#undef my_error
#undef my_printf_error
extern "C" void my_error(unsigned int nr, unsigned long MyFlags, ...);
extern "C" void my_printf_error(unsigned int nr, const char* fmt,
                                unsigned long MyFlags, ...);

int report_stoolap_error(const StoolapErrorView& v) {
    const char* msg = v.message;
    const bool have_text = *msg;

    const int32_t cls = stoolap_error_class(msg, nullptr);
    const int rc = ha_error_of(cls);
    if (cls == STOOLAP_ERR_UNKNOWN && have_text) {
        // Wording no pattern covers: stoolap drift, see case_18.
        stoolap_mariadb::g_stats.unmapped_errors.fetch_add(
            1, std::memory_order_relaxed);
    }

    const bool needs_text = (rc == HA_ERR_GENERIC || rc == HA_ERR_UNSUPPORTED);
    if (needs_text && have_text) {
        my_printf_error(ER_GET_ERRMSG, "stoolap: %s", MYF(0), msg);
    }
    return rc;
}

// MariaDB reports any commit-callback failure as ER_ERROR_DURING_COMMIT
// (1180) unless an error is already raised, and stoolap checks UPDATE's
// UNIQUE constraints at commit. Raise the mapped error first so the
// client sees 1062 / 1451 / 1452 / 1213 as it would for the statement.
int raise_commit_error(const char* msg) {
    const StoolapErrorView v{msg ? msg : ""};
    if (!*v.message) return HA_ERR_LOCK_DEADLOCK;
    const int rc = report_stoolap_error(v);
    switch (rc) {
        case HA_ERR_FOUND_DUPP_KEY:
            my_printf_error(ER_DUP_ENTRY, "stoolap: %s", MYF(0), v.message);
            break;
        case HA_ERR_NO_REFERENCED_ROW:
            my_printf_error(ER_NO_REFERENCED_ROW_2, "stoolap: %s", MYF(0),
                            v.message);
            break;
        case HA_ERR_ROW_IS_REFERENCED:
            my_printf_error(ER_ROW_IS_REFERENCED_2, "stoolap: %s", MYF(0),
                            v.message);
            break;
        case HA_ERR_LOCK_DEADLOCK:
            my_error(ER_LOCK_DEADLOCK, MYF(0));
            break;
        default:
            break;  // report_stoolap_error raised ER_GET_ERRMSG
    }
    return rc;
}

namespace {

// MariaDB hands us "./db/tbl"; both db and tbl can contain '_' and '/'.
// Inject `_0` for `_` and `_1` for `/` so distinct (db, tbl) pairs never
// collide (naive `/` -> `__` collides on db=`p`,tbl=`q__r` vs db=`p__q`,tbl=`r`).
// One past stoolap's name limit is shortened (fit_table_name).
std::string stoolap_table_from_path(const char* mariadb_path) {
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
    return stoolap_mariadb::fit_table_name(std::move(out));
}

std::string quote_ident(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// The stoolap type a column is stored in (STOOLAP_COL_*), 0 when none
// fits; the value encodings are in stoolap_types.h.
int32_t stoolap_column_kind(const Field* f) {
    using stoolap_mariadb::Enc;
    switch (stoolap_mariadb::field_enc(f)) {
        case Enc::PACKED_TIME:
        case Enc::UTC_MICROS:
        case Enc::INDEX:
            return STOOLAP_COL_INTEGER;
        case Enc::HEX:
            return STOOLAP_COL_TEXT;
        case Enc::PLAIN:
            break;
    }
    switch (f->real_type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_BIT:  // packed bitmap, val_int() gives the integer
            return STOOLAP_COL_INTEGER;
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            return STOOLAP_COL_FLOAT;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_NEWDECIMAL:  // round-trip via decimal string
        case MYSQL_TYPE_TINY_BLOB:   // TEXT family stored as BLOB type
        case MYSQL_TYPE_BLOB:        // ditto
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            return STOOLAP_COL_TEXT;
        default:
            return 0;
    }
}

// Returns the offending field name when any PK/UNIQUE part is a ci string
// (stoolap's TEXT equality is byte-wise; ci_general would accept 'a'/'A').
std::string ci_string_unique_part(TABLE* form) {
    auto is_ci_string = [](Field* f) -> bool {
        if (!f || !f->has_charset()) return false;
        switch (f->real_type()) {
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB:
                break;
            default:
                // ENUM / SET are stored as their index / mask.
                return false;
        }
        CHARSET_INFO* cs = f->charset();
        return cs && !(cs->state & MY_CS_BINSORT);
    };
    for (uint i = 0; i < form->s->keys; ++i) {
        KEY& k = form->key_info[i];
        if (!(k.flags & HA_NOSAME)) continue;  // PRIMARY and UNIQUE only
        for (uint j = 0; j < k.user_defined_key_parts; ++j) {
            Field* f = k.key_part[j].field;
            if (is_ci_string(f)) {
                return std::string(f->field_name.str, f->field_name.length);
            }
        }
    }
    return std::string();
}

// "key 'K' column 'C'" for the first PRIMARY KEY / UNIQUE part that
// covers only a prefix of its column (a(3)), else "". Stoolap enforces
// uniqueness on whole column values, which is weaker than the declared
// key: 'abc1' and 'abc2' collide on a(3) but not on a.
std::string prefix_unique_part(TABLE* form) {
    for (uint i = 0; i < form->s->keys; ++i) {
        const KEY& k = form->key_info[i];
        if (!(k.flags & HA_NOSAME)) continue;  // PRIMARY and UNIQUE only
        for (uint j = 0; j < k.user_defined_key_parts; ++j) {
            const KEY_PART_INFO& kp = k.key_part[j];
            if (!(kp.key_part_flag & HA_PART_KEY_SEG) || !kp.field) continue;
            std::string out = "key '";
            out.append(k.name.str, k.name.length);
            out += "' column '";
            out.append(kp.field->field_name.str, kp.field->field_name.length);
            out += "'";
            return out;
        }
    }
    return std::string();
}

// Unique keys stoolap cannot enforce as declared. Checked for new
// definitions only (create(), which CREATE TABLE and every ALTER TABLE
// go through): open()'s reconcile must still open a table created before
// a rule existed, so it can be read and fixed with ALTER TABLE.
bool refuse_unenforceable_keys(TABLE* form) {
    if (std::string bad = prefix_unique_part(form); !bad.empty()) {
        my_printf_error(ER_GET_ERRMSG,
                        "stoolap: PRIMARY KEY / UNIQUE %s covers a column "
                        "prefix, but stoolap enforces uniqueness on whole "
                        "column values. Key the whole column, or drop the "
                        "constraint.",
                        MYF(0), bad.c_str());
        return true;
    }
    // Refuse ci PK/UNIQUE up front: silent acceptance of case-different
    // duplicates is worse than failing at CREATE.
    if (std::string bad = ci_string_unique_part(form); !bad.empty()) {
        my_printf_error(ER_GET_ERRMSG,
                        "stoolap: PRIMARY KEY / UNIQUE on column '%s' uses "
                        "a case-insensitive collation, but stoolap compares "
                        "TEXT bytewise. Use a `_bin` collation (e.g. "
                        "utf8mb4_bin) on the column, or drop the "
                        "constraint.",
                        MYF(0), bad.c_str());
        return true;
    }
    return false;
}

// Columns the handler cannot read back faithfully. A COMPRESSED BLOB /
// TEXT stores through its own Field::store, into the field's buffer the
// row MariaDB builds shares: a row read next to it (UPDATE's old row,
// REPLACE's duplicate) would overwrite that row's value. Checked for new
// definitions only, as refuse_unenforceable_keys.
bool refuse_unsupported_columns(TABLE* form) {
    for (uint i = 0; i < form->s->fields; ++i) {
        const Field* f = form->field[i];
        if ((f->flags & BLOB_FLAG) && f->compression_method()) {
            my_printf_error(ER_GET_ERRMSG,
                            "stoolap: COMPRESSED BLOB / TEXT column '%s' is "
                            "not supported. Drop COMPRESSED from the column.",
                            MYF(0), f->field_name.str);
            return true;
        }
    }
    return false;
}

// Creates the stoolap table for `form` with its secondary indexes, and,
// when `with_fks`, the FOREIGN KEYs the statement's parser read (MariaDB
// keeps none in `form` for an engine that does not claim them); 0 or a
// handler error. A secondary index that fails is a warning: queries on
// its key full-scan.
int create_stoolap_table(StoolapDB* db, const std::string& tbl, TABLE* form,
                         bool if_not_exists, bool with_fks, const char* what) {
    std::vector<StoolapColumnDef> columns;
    columns.reserve(form->s->fields);
    for (uint i = 0; i < form->s->fields; ++i) {
        const Field* f = form->field[i];
        const int32_t kind = stoolap_column_kind(f);
        if (!kind) {
            sql_print_error(
                "stoolap: unsupported column type for '%s' (mysql type=%d)",
                f->field_name.str, (int)f->real_type());
            return HA_ERR_UNSUPPORTED;
        }
        columns.push_back({f->field_name.str, kind, f->real_maybe_null(),
                           (f->flags & AUTO_INCREMENT_FLAG) != 0});
    }
    std::vector<std::vector<int32_t>> parts(form->s->keys);
    std::vector<StoolapKeyDef> keys;
    for (uint i = 0; i < form->s->keys; ++i) {
        const KEY& k = form->key_info[i];
        for (uint j = 0; j < k.user_defined_key_parts; ++j)
            parts[i].push_back(
                static_cast<int32_t>(k.key_part[j].field->field_index));
        const int32_t kind = i == form->s->primary_key ? STOOLAP_KEYDEF_PRIMARY
                             : (k.flags & HA_NOSAME)   ? STOOLAP_KEYDEF_UNIQUE
                                                       : STOOLAP_KEYDEF_INDEX;
        keys.push_back({k.name.str, parts[i].data(),
                        static_cast<int32_t>(parts[i].size()), kind});
    }
    std::vector<StoolapThdForeignKey> fks;
    std::vector<std::string> parents;
    std::vector<StoolapForeignKeyDef> fk_defs;
    if (with_fks) {
        // By the child's database name: a TEMPORARY table's path is a file
        // in the tmpdir, not under its database.
        if (!stoolap_thd_foreign_keys(current_thd, form->s->db.str,
                                      form->s->db.length, &fks)) {
            my_printf_error(ER_GET_ERRMSG,
                            "stoolap: composite FOREIGN KEY: stoolap "
                            "enforces single-column foreign keys only.",
                            MYF(0));
            return HA_ERR_UNSUPPORTED;
        }
        for (const auto& fk : fks)
            parents.push_back(stoolap_table_from_path(fk.parent_path.c_str()));
        for (size_t i = 0; i < fks.size(); ++i)
            fk_defs.push_back({fks[i].column.c_str(), parents[i].c_str(),
                               fks[i].parent_column.empty()
                                   ? nullptr
                                   : fks[i].parent_column.c_str(),
                               fks[i].on_delete, fks[i].on_update});
    }
    // A copying ALTER rebuilds the table into this one: it keeps the
    // table's foreign keys, which MariaDB does not hold to hand back.
    std::string altered;
    std::vector<std::pair<std::string, std::string>> renamed;
    std::vector<StoolapColumnRename> renames;
    if (with_fks &&
        stoolap_thd_altered_table(current_thd, &altered, &renamed)) {
        altered = stoolap_table_from_path(altered.c_str());
        for (const auto& r : renamed)
            renames.push_back({r.first.c_str(), r.second.c_str()});
    }
    const int rc = stoolap_create_table(
        db, tbl.c_str(), columns.data(), static_cast<int32_t>(columns.size()),
        keys.data(), static_cast<int32_t>(keys.size()), fk_defs.data(),
        static_cast<int32_t>(fk_defs.size()),
        altered.empty() ? nullptr : altered.c_str(), renames.data(),
        static_cast<int32_t>(renames.size()), if_not_exists);
    if (rc == STOOLAP_INDEX_FAILED) {
        sql_print_warning(
            "stoolap: failed to create indexes of '%s': %s -- queries on "
            "these keys will full-scan",
            tbl.c_str(), stoolap_errmsg(db));
        return 0;
    }
    if (rc != STOOLAP_OK) {
        auto verr = fetch_db_error(db);
        sql_print_error("stoolap: %s of '%s' failed: %s", what, tbl.c_str(),
                        verr.message);
        return report_stoolap_error(verr);
    }
    return 0;
}

int stoolap_init_func(void* p) {
    stoolap_hton = static_cast<handlerton*>(p);
    stoolap_hton->db_type = DB_TYPE_AUTOASSIGN;
    stoolap_hton->create = stoolap_create_handler;
    // HTON_CAN_RECREATE would route TRUNCATE through DROP+CREATE; we expose
    // a real delete_all_rows that runs stoolap's native TRUNCATE instead.
    stoolap_hton->flags = 0;
    stoolap_hton->commit = stoolap_commit_cb;
    stoolap_hton->rollback = stoolap_rollback_cb;
    stoolap_hton->close_connection = stoolap_close_connection_cb;
    stoolap_hton->start_consistent_snapshot =
        stoolap_start_consistent_snapshot_cb;
    // Pushdown factories return NULL for ineligible plans (cross-engine
    // join, SP context, prepare phase, ...) and MariaDB falls through to
    // row-pump. See ha_stoolap_select.cc.
    stoolap_hton->create_select =
        stoolap_pushdown::create_stoolap_select_handler;
    stoolap_hton->create_unit = stoolap_pushdown::create_stoolap_unit_handler;
    stoolap_hton->create_derived =
        stoolap_pushdown::create_stoolap_derived_handler;
    stoolap_hton->savepoint_offset = sizeof(StoolapSavepointSlot);
    stoolap_hton->savepoint_set = stoolap_savepoint_set_cb;
    stoolap_hton->savepoint_release = stoolap_savepoint_release_cb;
    stoolap_hton->savepoint_rollback = stoolap_savepoint_rollback_cb;

    const char* dsn =
        (stoolap_dsn_var && *stoolap_dsn_var) ? stoolap_dsn_var : "memory://";
    int rc = g_engine.open(dsn);
    if (rc != STOOLAP_OK) {
        sql_print_error("stoolap: failed to open DSN '%s': %s", dsn,
                        g_engine.last_error().c_str());
        return HA_ERR_INITIALIZATION;
    }
    sql_print_information(
        "stoolap: storage engine initialized (dsn=%s, version=%s)", dsn,
        stoolap_version());
    return 0;
}

int stoolap_done_func(void*) {
    g_engine.close();
    return 0;
}

}  // namespace

std::string stoolap_table_name(const TABLE_SHARE* share) {
    return stoolap_table_from_path(share->normalized_path.str);
}

// ---------------------------------------------------------------------------
// ha_stoolap implementation
// ---------------------------------------------------------------------------

ha_stoolap::ha_stoolap(handlerton* hton, TABLE_SHARE* table_arg)
    : handler(hton, table_arg) {
    // `ref`: a flag byte and stoolap's row id (see position()).
    ref_length = 1 + 8;
}

ulonglong ha_stoolap::table_flags() const {
    // No HA_HAS_RECORDS on purpose: MariaDB's optimized-away COUNT(*) would
    // turn records() into a user-visible result, and Stoolap's bare
    // table-count metadata isn't MVCC-safe enough for that contract.
    // HA_PRIMARY_KEY_REQUIRED_FOR_{DELETE,POSITION}: our SQL builder
    // identifies rows by PK; without these flags the optimizer may strip
    // PK columns from read_set and UPDATE/DELETE would target the wrong row.
    return HA_REC_NOT_IN_SEQ | HA_NULL_IN_KEY | HA_BINLOG_ROW_CAPABLE |
           HA_PRIMARY_KEY_REQUIRED_FOR_DELETE |
           HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
           HA_CAN_DIRECT_UPDATE_AND_DELETE;
}

ulong ha_stoolap::index_flags(uint inx, uint part, bool /*all_parts*/) const {
    // Numeric/timestamp/boolean parts: full range-scan + ordered iter; their
    // byte-compare matches MariaDB's expected order.
    // ci string parts: drop everything (return 0) so the optimizer falls
    // back to a full scan where MariaDB applies its own collation per row.
    // _bin string parts: full flags (byte compare agrees).
    // DECIMAL/ENUM/SET: drop everything; ref access would build numeric-vs-
    // text predicates against stoolap's TEXT storage.
    // HA_KEY_SCAN_NOT_ROR disables index_merge_intersect (our scans aren't
    // in rowid order; we have no rowid concept).
    if (inx < table_share->keys &&
        part < table_share->key_info[inx].user_defined_key_parts) {
        Field* f = table_share->key_info[inx].key_part[part].field;
        if (f) {
            switch (f->real_type()) {
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_VAR_STRING:
                case MYSQL_TYPE_STRING:
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB: {
                    // A hex-stored multi-byte charset orders by code point
                    // in MariaDB, by byte in stoolap (stoolap_types.h).
                    if (stoolap_mariadb::field_enc(f) ==
                            stoolap_mariadb::Enc::HEX &&
                        !stoolap_mariadb::hex_keeps_order(f))
                        return 0;
                    CHARSET_INFO* cs = f->charset();
                    if (cs && (cs->state & MY_CS_BINSORT)) {
                        return HA_READ_NEXT | HA_READ_RANGE | HA_READ_ORDER |
                               HA_KEY_SCAN_NOT_ROR;
                    }
                    return 0;
                }
                case MYSQL_TYPE_NEWDECIMAL:
                case MYSQL_TYPE_ENUM:
                case MYSQL_TYPE_SET:
                    return 0;
                default:
                    break;
            }
        }
    }
    return HA_READ_NEXT | HA_READ_RANGE | HA_READ_ORDER | HA_KEY_SCAN_NOT_ROR;
}

StoolapDB* ha_stoolap::db_ensure() {
    if (!db_) db_ = g_engine.clone_handle();
    return db_.get();
}

// Forward decl: defined below alongside create(). Used by both create()
// (after a fresh CREATE TABLE) and open()'s reconcile path.

int ha_stoolap::open(const char* name, int /*mode*/, uint /*test_if_locked*/) {
    stoolap_table_ = stoolap_table_from_path(name);

    // Reconcile: MariaDB may have a .frm while stoolap is missing the table
    // (fresh datadir, wipe, DSN switch). CREATE TABLE IF NOT EXISTS no-ops
    // when present. The reconciled-set cache short-circuits the next open()
    // (called per-statement in tight loops); invalidates on DROP/TRUNCATE/RENAME.
    if (!g_engine.is_reconciled(stoolap_table_)) {
        auto* ctx = ha_thd() ? get_thd_ctx(ha_thd()) : nullptr;
        StoolapDB* d = (ctx && ctx->db()) ? ctx->db() : db_ensure();
        if (!d) return HA_ERR_OUT_OF_MEM;

        // A table created before refuse_unenforceable_keys gained a rule
        // still opens (to be read, and fixed with ALTER TABLE), but its
        // key is enforced on whole column values only.
        if (std::string bad = prefix_unique_part(table); !bad.empty()) {
            sql_print_warning(
                "stoolap: table '%s' has PRIMARY KEY / UNIQUE %s over a "
                "column prefix; stoolap enforces it on whole column values. "
                "ALTER TABLE ... DROP INDEX it, or key the whole column.",
                stoolap_table_.c_str(), bad.c_str());
        }
        // Secondary indexes too: a wiped stoolap-side table comes back
        // without them, leaving "index-served" queries on a full scan.
        // Not the statement's FOREIGN KEYs: they belong to the table it
        // creates, if any, and MariaDB keeps none for this one.
        if (int err =
                create_stoolap_table(d, stoolap_table_, table,
                                     /*if_not_exists=*/true,
                                     /*with_fks=*/false, "schema reconcile"))
            return err;
        g_engine.mark_reconciled(stoolap_table_);
    }
    return 0;
}

// After every statement that used the handler: BLOB buffers a large row
// grew go now, not with the handler the table cache keeps.
int ha_stoolap::reset() {
    for (std::string& b : blob_bufs_)
        if (b.capacity() > (size_t(1) << 20)) std::string().swap(b);
    return 0;
}

int ha_stoolap::close() {
    // Release scan state now -- a large fetch_all buffer would otherwise
    // live until the handler is destroyed.
    reset_scan_state();
    db_.reset();
    stoolap_table_.clear();
    return 0;
}

// A single StoolapDB* is unsafe for concurrent use: concurrent DDL
// through g_engine.raw() would race both the executor and the per-handle
// error buffer, leaking error messages across threads. Use the THD clone.
StoolapDB* ha_stoolap::ddl_db() {
    auto* ctx = ha_thd() ? get_thd_ctx(ha_thd()) : nullptr;
    StoolapDB* d = (ctx && ctx->db()) ? ctx->db() : db_ensure();
    return d;
}

int ha_stoolap::create(const char* name, TABLE* form,
                       HA_CREATE_INFO* /*info*/) {
    const std::string tbl = stoolap_table_from_path(name);
    if (refuse_unenforceable_keys(form) || refuse_unsupported_columns(form))
        return HA_ERR_UNSUPPORTED;
    StoolapDB* db = ddl_db();
    if (!db) return HA_ERR_OUT_OF_MEM;
    if (int err = create_stoolap_table(db, tbl, form, /*if_not_exists=*/false,
                                       /*with_fks=*/true, "CREATE TABLE"))
        return err;
    g_engine.mark_reconciled(tbl);
    return 0;
}

int ha_stoolap::delete_table(const char* name) {
    const std::string flat = stoolap_table_from_path(name);
    StoolapDB* db = ddl_db();
    if (!db) return HA_ERR_OUT_OF_MEM;
    if (stoolap_drop_table(db, flat.c_str()) != STOOLAP_OK) {
        auto verr = fetch_db_error(db);
        sql_print_error("stoolap: DROP TABLE failed: %s", verr.message);
        return report_stoolap_error(verr);
    }
    g_engine.drop_reconciled(flat);
    g_engine.records_drop(flat);
    // Re-CREATE TABLE of the same name must start AI fresh, not from the
    // dropped table's leftover counter.
    g_engine.ai_invalidate();
    return 0;
}

int ha_stoolap::rename_table(const char* from, const char* to) {
    const std::string flat_from = stoolap_table_from_path(from);
    const std::string flat_to = stoolap_table_from_path(to);
    StoolapDB* db = ddl_db();
    if (!db) return HA_ERR_OUT_OF_MEM;
    if (stoolap_rename_table(db, flat_from.c_str(), flat_to.c_str()) !=
        STOOLAP_OK) {
        auto verr = fetch_db_error(db);
        sql_print_error("stoolap: RENAME TABLE failed: %s", verr.message);
        return report_stoolap_error(verr);
    }
    g_engine.drop_reconciled(flat_from);
    g_engine.mark_reconciled(flat_to);
    // Move records-cache entry so the renamed table doesn't pay a fresh COUNT.
    uint64_t cnt = 0;
    if (g_engine.records_lookup(flat_from, &cnt)) {
        g_engine.records_set(flat_to, cnt);
    }
    g_engine.records_drop(flat_from);
    g_engine.ai_invalidate();
    return 0;
}

namespace {

// Point Fields at a row buffer that isn't table->record[0].
void move_fields(TABLE* t, my_ptrdiff_t d) {
    if (!d) return;
    for (uint i = 0; i < t->s->fields; ++i)
        t->field[i]->move_field_offset(d);
}

// For TEXT columns the caller owns a std::string (`text_holder`) that
// keeps StoolapValue.text.ptr valid for the duration of the FFI call.
bool extract_field(Field* f, StoolapValue& v, std::string& text_holder) {
    if (f->is_null()) {
        v.value_type = STOOLAP_TYPE_NULL;
        return true;
    }
    if (stoolap_mariadb::field_enc(f) != stoolap_mariadb::Enc::PLAIN)
        return stoolap_mariadb::encode_field(f, v, text_holder);
    switch (f->real_type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_BIT: {
            const longlong raw = f->val_int();
            // BIGINT UNSIGNED > INT64_MAX is the only narrower-than-i64
            // case we can't represent: val_int() hands us a negative
            // longlong that would land as negative i64 in stoolap and
            // compare wrong on range/order. Reject rather than corrupt.
            if (f->real_type() == MYSQL_TYPE_LONGLONG && f->is_unsigned() &&
                raw < 0) {
                return false;
            }
            v.value_type = STOOLAP_TYPE_INTEGER;
            v.v.integer = static_cast<int64_t>(raw);
            return true;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
            v.value_type = STOOLAP_TYPE_FLOAT;
            v.v.float64 = f->val_real();
            return true;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
            // utf8 / ascii text as is. DECIMAL: val_str() formats the
            // canonical decimal string (e.g. "123.450"), keeping precision.
            char tmp[256];
            String tmp_str(tmp, sizeof(tmp), f->charset());
            String* s = f->val_str(&tmp_str);
            text_holder.assign(s->ptr(), s->length());
            v.value_type = STOOLAP_TYPE_TEXT;
            v.v.text.ptr = text_holder.data();
            v.v.text.len = static_cast<int64_t>(text_holder.size());
            return true;
        }
        default:
            return false;
    }
}

// The encode callback of a write, for a value the record path does not
// read itself: extract_field, with the fields moved to the record it reads
// and back to record[0] when the write is done. `holder` keeps the text
// until the next call.
struct RecordSource {
    TABLE* table;
    const uchar* at;
    int err = 0;
    std::string holder;
    explicit RecordSource(TABLE* t) : table(t), at(t->record[0]) {}
    ~RecordSource() {
        move_fields(table, static_cast<my_ptrdiff_t>(table->record[0] - at));
    }
};

int32_t encode_cell(void* ctx, int32_t field, const uint8_t* record,
                    StoolapValue* out) {
    auto* src = static_cast<RecordSource*>(ctx);
    if (record != src->at) {
        move_fields(src->table, static_cast<my_ptrdiff_t>(record - src->at));
        src->at = record;
    }
    if (!extract_field(src->table->field[field], *out, src->holder))
        src->err = HA_ERR_UNSUPPORTED;
    return src->err;
}

}  // namespace

// Bounds memory on huge INSERT...VALUES / SELECT / LOAD DATA (~5MB at
// 8 columns) while keeping per-flush fixed cost a small fraction of
// throughput. Stays well under int32_t row_count for the batch FFI.
static constexpr size_t kBulkFlushRows = 50000;

void ha_stoolap::start_bulk_insert(ha_rows /*rows*/, uint /*flags*/) {
    bulk_active_ = false;
    bulk_owns_tx_ = false;
    bulk_rows_ = 0;

    // INSERT IGNORE / REPLACE / ODKU need per-row HA_ERR_FOUND_DUPP_KEY
    // callbacks for recovery; a batch is all-or-nothing, and one duplicate
    // would drop every row with it.
    if (stoolap_thd_needs_per_row_dup_handling(ha_thd())) return;

    // The rows wait in Rust until the flush.
    if (!row_buffer_) row_buffer_.reset(stoolap_row_buffer_new());
    if (!row_buffer_) return;
    stoolap_row_buffer_clear(row_buffer_.get());
    StoolapDB* db = get_thd_ctx(ha_thd())->db();
    bulk_fk_ =
        db && stoolap_writes_through_sql(db, stoolap_table_.c_str()) == 1;
    bulk_active_ = true;
}

int ha_stoolap::end_bulk_insert() {
    if (!bulk_active_) return 0;
    bulk_active_ = false;
    const int err = flush_bulk_buffer();
    // mysql_insert reports a failed end_bulk_insert by my_errno (with the
    // duplicate key from get_dup_key), not by this return value.
    if (err) my_errno = err;
    return err;
}

int ha_stoolap::flush_bulk_buffer(bool partial) {
    if (bulk_rows_ == 0) return 0;
    bulk_rows_ = 0;
    // A table a foreign key names goes through SQL, a flush's rows in one
    // transaction. Flushed in chunks in autocommit mode, the chunks share
    // one the statement-end commit ends, so the statement stays atomic.
    if (partial && bulk_fk_) {
        auto* ctx = get_thd_ctx(ha_thd());
        if (!ctx->has_tx()) {
            if (ctx->begin() != STOOLAP_OK) {
                sql_print_error("stoolap: bulk-insert tx open failed: %s",
                                stoolap_errmsg(ctx->db()));
                return HA_ERR_GENERIC;
            }
            bulk_owns_tx_ = true;
        }
    }
    StoolapDB* db = direct_write_db();
    if (!db) return HA_ERR_INTERNAL_ERROR;
    int64_t affected = 0;
    const int rc = stoolap_insert_buffer(db, stoolap_table_.c_str(),
                                         static_cast<int32_t>(table->s->fields),
                                         row_buffer_.get(), &affected);
    cur_row_id_valid_ = false;
    if (rc != STOOLAP_OK) {
        invalidate_records_cache();
        return write_error(db, "INSERT");
    }
    apply_count_delta(affected);
    return 0;
}

int ha_stoolap::write_row(const uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;

    // Reset before each write so stale values don't bleed into a later
    // statement's ON DUPLICATE KEY UPDATE / REPLACE recovery.
    last_dup_key_ = MAX_KEY;

    // If the table has an AUTO_INCREMENT column and the user didn't supply
    // a value (or supplied 0), the framework needs to stamp the next value
    // into the field before we serialize. update_auto_increment() routes to
    // our get_auto_increment() implementation.
    if (table->next_number_field && buf == table->record[0]) {
        int err = update_auto_increment();
        if (err) return err;
    }

    // Keep AI allocator in sync with explicit-id INSERTs (cross-connection).
    // Skip non-positive: signed -2 casts to ULLONG_MAX-1 and the next
    // generated insert fails ER_AUTOINC_READ_FAILED.
    if (table->found_next_number_field) {
        Field* aifield = table->found_next_number_field;
        const longlong sv = aifield->val_int();
        const bool unsigned_col = (aifield->flags & UNSIGNED_FLAG) != 0;
        if (unsigned_col || sv > 0) {
            const ulonglong v = static_cast<ulonglong>(sv);
            g_engine.ai_note_explicit(stoolap_table_, static_cast<uint64_t>(v));
        }
    }

    const auto& fields = insert_fields();
    RecordSource src(table);
    if (bulk_active_) {
        const int64_t n = stoolap_row_buffer_add(
            row_buffer_.get(), fields.data(),
            static_cast<int32_t>(fields.size()), buf, &encode_cell, &src);
        if (n < 0) {
            if (src.err) return src.err;
            sql_print_error("stoolap: INSERT failed: %s",
                            stoolap_row_buffer_errmsg(row_buffer_.get()));
            return HA_ERR_GENERIC;
        }
        bulk_rows_ = n;
        if (static_cast<size_t>(n) < kBulkFlushRows) return 0;
        const int err = flush_bulk_buffer(/*partial=*/true);
        if (err && bulk_owns_tx_) {
            auto* ctx = get_thd_ctx(ha_thd());
            if (ctx->has_tx()) ctx->rollback();
            bulk_owns_tx_ = false;
        }
        return err;
    }

    StoolapDB* db = direct_write_db();
    if (!db) return HA_ERR_INTERNAL_ERROR;
    int64_t affected = 0;
    const int rc = stoolap_insert_record(
        db, stoolap_table_.c_str(), static_cast<int32_t>(table->s->fields),
        fields.data(), static_cast<int32_t>(fields.size()), buf, &encode_cell,
        &src, &affected);
    cur_row_id_valid_ = false;
    if (rc != STOOLAP_OK) return src.err ? src.err : write_error(db, "INSERT");
    apply_count_delta(affected);
    return 0;
}

// Every field, as a direct insert reads the record.
const std::vector<StoolapField>& ha_stoolap::insert_fields() {
    if (write_fields_.empty())
        for (uint i = 0; i < table->s->fields; ++i)
            write_fields_.push_back(stoolap_mariadb::record_field(
                table, i, static_cast<int32_t>(i)));
    return write_fields_;
}

// The fields write_set names, as a direct update reads the records.
const std::vector<StoolapField>& ha_stoolap::refresh_update_fields() {
    const uint fields = table->s->fields;
    std::vector<uint32_t>& w = proj_words_scratch_;
    w.assign((fields + 31) / 32, 0);
    for (uint i = 0; i < fields; ++i)
        if (bitmap_is_set(table->write_set, i))
            w[i / 32] |= uint32_t(1) << (i % 32);
    if (w == update_words_ && !update_fields_.empty()) return update_fields_;
    update_fields_.clear();
    for (uint i = 0; i < fields; ++i)
        if (w[i / 32] & (uint32_t(1) << (i % 32)))
            update_fields_.push_back(stoolap_mariadb::record_field(
                table, i, static_cast<int32_t>(i)));
    update_words_ = w;
    return update_fields_;
}

// The PRIMARY KEY parts, which name a row a write has no row id for.
const std::vector<StoolapField>& ha_stoolap::key_fields() {
    if (!key_fields_built_) {
        TABLE_SHARE* share = table->s;
        if (share->primary_key < share->keys) {
            const KEY& pk = share->key_info[share->primary_key];
            for (uint i = 0; i < pk.user_defined_key_parts; ++i) {
                const uint f = pk.key_part[i].fieldnr - 1;
                key_fields_.push_back(stoolap_mariadb::record_field(
                    table, f, static_cast<int32_t>(f)));
            }
        }
        key_fields_built_ = true;
    }
    return key_fields_;
}

// The session handle direct writes go through. A write outside an open
// transaction lands in the statement's own, which the statement-end commit
// callback ends: register for it, as some bulk INSERT paths skip
// external_lock.
StoolapDB* ha_stoolap::direct_write_db() {
    THD* thd = ha_thd();
    trans_register_ha(thd, /*all=*/false, stoolap_hton, /*flags=*/0);
    return get_thd_ctx(thd)->db();
}

// The handler error for a failed direct write, keeping the duplicate key
// for get_dup_key().
int ha_stoolap::write_error(StoolapDB* db, const char* what) {
    auto verr = fetch_db_error(db);
    sql_print_error("stoolap: %s failed: %s", what, verr.message);
    const int err = report_stoolap_error(verr);
    if (err == HA_ERR_FOUND_DUPP_KEY) {
        last_dup_key_ = errkey_from_view(verr, table->s);
        errkey = last_dup_key_;
    }
    return err;
}

// Sets the written columns whose value changed, in the row the last read
// returned (by its row id), or else the row its PRIMARY KEY names.
int ha_stoolap::update_row(const uchar* old_data, const uchar* new_data) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (!cur_row_id_valid_)
        cur_row_id_valid_ = key_row_id(old_data, &cur_row_id_);
    const auto& fields = refresh_update_fields();
    if (fields.empty()) return 0;
    const auto& keys = key_fields();
    StoolapDB* db = direct_write_db();
    if (!db) return HA_ERR_INTERNAL_ERROR;
    RecordSource src(table);
    int64_t affected = 0;
    const int rc = stoolap_update_record(
        db, stoolap_table_.c_str(), static_cast<int32_t>(table->s->fields),
        cur_row_id_, cur_row_id_valid_, fields.data(),
        static_cast<int32_t>(fields.size()), keys.data(),
        static_cast<int32_t>(keys.size()), old_data, new_data, &encode_cell,
        &src, &affected);
    if (rc != STOOLAP_OK) return src.err ? src.err : write_error(db, "UPDATE");
    return affected == 0 ? HA_ERR_RECORD_DELETED : 0;
}

// Deletes the row the last read returned (by its row id), or else the row
// its PRIMARY KEY names.
int ha_stoolap::delete_row(const uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (!cur_row_id_valid_) cur_row_id_valid_ = key_row_id(buf, &cur_row_id_);
    const auto& keys = key_fields();
    StoolapDB* db = direct_write_db();
    if (!db) return HA_ERR_INTERNAL_ERROR;
    RecordSource src(table);
    int64_t affected = 0;
    const int rc = stoolap_delete_record(
        db, stoolap_table_.c_str(), cur_row_id_, cur_row_id_valid_, keys.data(),
        static_cast<int32_t>(keys.size()), buf, &encode_cell, &src, &affected);
    if (rc != STOOLAP_OK) return src.err ? src.err : write_error(db, "DELETE");
    if (affected == 0) return HA_ERR_RECORD_DELETED;
    apply_count_delta(-affected);
    return 0;
}

int ha_stoolap::analyze(THD* /*thd*/, HA_CHECK_OPT* /*opt*/) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    std::string sql = "ANALYZE TABLE ";
    sql += quote_ident(stoolap_table_);
    if (stoolap_exec(db_ensure(), sql.c_str(), nullptr) != STOOLAP_OK) {
        auto verr = fetch_db_error(db_raw());
        sql_print_error("stoolap: ANALYZE failed: %s", verr.message);
        return report_stoolap_error(verr);
    }
    return 0;
}

const COND* ha_stoolap::cond_push(const COND* cond) {
    if (!cond) return nullptr;
    THD* thd = ha_thd();
    if (!thd) return cond;

    // Accept (return null) only when the engine will actually filter:
    // the direct-DML path forwards thd->query() with WHERE intact and
    // stoolap re-evaluates. Returning null on a shape that ends up
    // per-row would silently mass-mutate the table.
    if (stoolap_pushdown::can_direct_modify(thd)) {
        Item* it = const_cast<Item*>(static_cast<const Item*>(cond));
        const bool trust_bin = stoolap_thd_trust_binary_strings(thd);
        if (trust_bin || stoolap_pushdown::item_safe_for_byte_comparison(it)) {
            return nullptr;  // accepted; route to direct path
        }
    }
    // SELECT (row pump) still needs a real cond->SQL translator -- TBD.
    return cond;
}

int ha_stoolap::direct_update_rows_init(List<Item>* /*update_fields*/) {
    // Non-zero falls back to per-row update_row(); never error here.
    return stoolap_pushdown::can_direct_modify(ha_thd()) ? 0
                                                         : HA_ERR_WRONG_COMMAND;
}

int ha_stoolap::direct_update_rows(ha_rows* update_rows, ha_rows* found_rows) {
    // pre_direct_update_rows may have run already and stashed the count.
    if (direct_modify_in_pre_) {
        direct_modify_in_pre_ = false;
        if (update_rows) *update_rows = direct_modify_affected_pre_;
        if (found_rows) *found_rows = direct_modify_affected_pre_;
        return 0;
    }
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    if (update_rows) *update_rows = affected;
    // No found-but-unchanged tracking; report found == affected.
    if (found_rows) *found_rows = affected;
    return 0;
}

int ha_stoolap::direct_delete_rows_init() {
    return stoolap_pushdown::can_direct_modify(ha_thd()) ? 0
                                                         : HA_ERR_WRONG_COMMAND;
}

int ha_stoolap::direct_delete_rows(ha_rows* delete_rows) {
    if (direct_modify_in_pre_) {
        direct_modify_in_pre_ = false;
        if (delete_rows) *delete_rows = direct_modify_affected_pre_;
        return 0;
    }
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    if (affected > static_cast<ha_rows>(INT64_MAX)) {
        invalidate_records_cache();
    } else {
        apply_count_delta(-static_cast<int64_t>(affected));
    }
    if (delete_rows) *delete_rows = affected;
    return 0;
}

// MariaDB 11.4 gates direct DML on these; the base impl returns
// HA_ERR_WRONG_COMMAND. The actual modify runs in pre_direct_*_rows
// (some paths use it as the work site; the short-circuit handles
// follow-up direct_*_rows calls).
int ha_stoolap::pre_direct_update_rows_init(List<Item>* fields) {
    return direct_update_rows_init(fields);
}

int ha_stoolap::pre_direct_update_rows() {
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    direct_modify_in_pre_ = true;
    direct_modify_affected_pre_ = affected;
    return 0;
}

int ha_stoolap::pre_direct_delete_rows_init() {
    return direct_delete_rows_init();
}

int ha_stoolap::pre_direct_delete_rows() {
    ha_rows affected = 0;
    unsigned ek = MAX_KEY;
    int rc = stoolap_pushdown::try_direct_modify(ha_thd(), &affected, &ek);
    if (rc) {
        if (rc == HA_ERR_FOUND_DUPP_KEY) {
            last_dup_key_ = ek;
            errkey = ek;
        }
        return rc;
    }
    direct_modify_in_pre_ = true;
    direct_modify_affected_pre_ = affected;
    if (affected > static_cast<ha_rows>(INT64_MAX)) {
        invalidate_records_cache();
    } else {
        apply_count_delta(-static_cast<int64_t>(affected));
    }
    return 0;
}

int ha_stoolap::delete_all_rows() {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    // Route via autocommit db (stoolap blocks TRUNCATE inside a tx).
    // Cache invalidate AFTER success: a pre-emptive set(0) would leave
    // the optimizer seeing 0 while a rejected TRUNCATE keeps the rows.
    std::string sql = "TRUNCATE TABLE ";
    sql += quote_ident(stoolap_table_);

    if (stoolap_exec(db_ensure(), sql.c_str(), nullptr) != STOOLAP_OK) {
        auto verr = fetch_db_error(db_raw());
        sql_print_error("stoolap: TRUNCATE failed: %s", verr.message);
        return report_stoolap_error(verr);
    }
    set_count_exact(0);
    // Process-wide allocator may still reflect pre-truncate counter.
    g_engine.ai_invalidate();
    return 0;
}

using stoolap_mariadb::pkt_le32;
using stoolap_mariadb::pkt_parse_header;

int ha_stoolap::rnd_init(bool /*scan*/) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    reset_scan_state();
    return open_read(nullptr, 0, nullptr, 0, false);
}

void ha_stoolap::refresh_projection() {
    // read_set | write_set (write_set covers UPDATE's old / new compare),
    // or every field when there are no bitmaps or no bits (a COUNT(*)
    // still needs a row per row).
    const uint fields = table->s->fields;
    std::vector<uint32_t>& w = proj_words_scratch_;
    w.assign((fields + 31) / 32, 0);
    bool any = false;
    if (table->read_set && table->write_set) {
        for (uint i = 0; i < fields; ++i) {
            if (bitmap_is_set(table->read_set, i) ||
                bitmap_is_set(table->write_set, i)) {
                w[i / 32] |= uint32_t(1) << (i % 32);
                any = true;
            }
        }
    } else {
        w.clear();
    }
    if (w == read_cols_words_ && !read_fields_.empty()) return;
    read_fields_.clear();
    for (uint i = 0; i < fields; ++i) {
        if (any && !(w[i / 32] & (uint32_t(1) << (i % 32)))) continue;
        read_fields_.push_back(
            stoolap_mariadb::record_field(table, i, static_cast<int32_t>(i)));
    }
    read_cols_words_ = w;
}

// Opens a direct read of the projected columns.
int ha_stoolap::open_read(const StoolapKeyBound* bounds, int32_t nbounds,
                          const int32_t* order, int32_t norder, bool desc) {
    refresh_projection();
    const uint fields = table->s->fields;

    auto* ctx = get_thd_ctx(ha_thd());
    StoolapDB* db = ctx->db();
    if (!db) return HA_ERR_INTERNAL_ERROR;
    // A statement that sets the primary key moves rows to new ids
    int32_t flags = desc ? STOOLAP_READ_DESC : 0;
    const uint pk = table->s->primary_key;
    if (pk != MAX_KEY &&
        bitmap_is_set(table->write_set,
                      table->key_info[pk].key_part[0].fieldnr - 1))
        flags |= STOOLAP_READ_FIXED;
    StoolapCursor* cursor = read_cursor();
    if (!cursor) return HA_ERR_OUT_OF_MEM;
    stoolap_mariadb::g_stats.buffered_scans.fetch_add(
        1, std::memory_order_relaxed);
    const std::vector<StoolapField>* keys = nbounds ? &index_parts() : nullptr;
    RecordSource src(table);
    const int rc = stoolap_read(
        db, stoolap_table_.c_str(), static_cast<int32_t>(fields),
        read_fields_.data(), static_cast<int32_t>(read_fields_.size()),
        keys ? keys->data() : nullptr,
        keys ? static_cast<int32_t>(keys->size()) : 0, bounds, nbounds, order,
        norder, flags, &encode_cell, &src, cursor);
    // No row can be within the bounds.
    if (rc == STOOLAP_DONE) return HA_ERR_KEY_NOT_FOUND;
    if (rc != STOOLAP_OK) {
        if (src.err) return src.err;
        StoolapErrorView verr{stoolap_cursor_errmsg(cursor)};
        sql_print_error("stoolap: read failed: %s", verr.message);
        return report_stoolap_error(verr);
    }
    reading_ = true;
    return 0;
}

// The active index's parts, as a read's bounds describe them.
const std::vector<StoolapField>& ha_stoolap::index_parts() {
    if (key_parts_index_ != active_index) {
        key_parts_.clear();
        const KEY& ki = table->key_info[active_index];
        for (uint i = 0; i < ki.user_defined_key_parts; ++i) {
            const uint f = ki.key_part[i].fieldnr - 1;
            key_parts_.push_back(stoolap_mariadb::record_field(
                table, f, static_cast<int32_t>(f)));
        }
        key_parts_index_ = active_index;
    }
    return key_parts_;
}

// A record an end bound's key is restored into, the start's being in
// record[1].
uchar* ha_stoolap::bound_record() {
    bound_rec_.resize(table->s->reclength);
    return bound_rec_.data();
}

// The handler's cursor, made on its first read.
StoolapCursor* ha_stoolap::read_cursor() {
    if (!cursor_) cursor_.reset(stoolap_cursor_new());
    return cursor_.get();
}

int ha_stoolap::rnd_end() {
    release_read();
    return 0;
}

int ha_stoolap::rnd_next(uchar* buf) {
    if (!reading_) return HA_ERR_END_OF_FILE;
    // The cursor writes the row into buf; the fields it leaves to the
    // store callback are moved there for it.
    const my_ptrdiff_t row_diff =
        static_cast<my_ptrdiff_t>(buf - table->record[0]);
    if (table->s->null_bytes) std::memset(buf, 0, table->s->null_bytes);
    move_fields(table, row_diff);
    stoolap_mariadb::RecordTarget target{table, 0, &blob_bufs_};
    int64_t id = 0;
    int32_t last = 0;
    const int rc = stoolap_cursor_next(
        cursor_.get(), buf, &stoolap_mariadb::store_cell, &target, &id, &last);
    move_fields(table, -row_diff);
    if (rc == STOOLAP_ROW) {
        cur_row_id_ = id;
        cur_row_id_valid_ = true;
        ++read_rows_;
        // The cursor ended the read with its last row
        if (last) reading_ = false;
        return 0;
    }
    reading_ = false;
    if (rc == STOOLAP_DONE) return HA_ERR_END_OF_FILE;
    if (target.err) return target.err;
    StoolapErrorView verr{stoolap_cursor_errmsg(cursor_.get())};
    sql_print_error("stoolap: read failed: %s", verr.message);
    return report_stoolap_error(verr);
}

int ha_stoolap::rnd_pos(uchar* buf, uchar* pos) {
    if (!pos[0]) {
        // position() had no row id for it (see there).
        my_printf_error(ER_GET_ERRMSG,
                        "stoolap: re-read by position of a row with no row id",
                        MYF(0));
        return HA_ERR_UNSUPPORTED;
    }
    {
        // Re-read the row by its id.
        int64_t id;
        std::memcpy(&id, pos + 1, sizeof(id));
        reset_scan_state();
        refresh_projection();
        auto* ctx = get_thd_ctx(ha_thd());
        StoolapDB* db = ctx->db();
        if (!db) return HA_ERR_INTERNAL_ERROR;
        StoolapCursor* cursor = read_cursor();
        if (!cursor) return HA_ERR_OUT_OF_MEM;
        if (stoolap_read_row(db, stoolap_table_.c_str(),
                             static_cast<int32_t>(table->s->fields),
                             read_fields_.data(),
                             static_cast<int32_t>(read_fields_.size()), id,
                             cursor) != STOOLAP_OK) {
            StoolapErrorView verr{stoolap_cursor_errmsg(cursor)};
            sql_print_error("stoolap: read by row id failed: %s", verr.message);
            return report_stoolap_error(verr);
        }
        reading_ = true;
        const int rc = rnd_next(buf);
        return rc == HA_ERR_END_OF_FILE ? HA_ERR_RECORD_DELETED : rc;
    }
}

// The row id of the row in `record` when the last read gave none: a
// single-column PRIMARY KEY, which stoolap only allows on an INTEGER
// column, is the row id.
bool ha_stoolap::key_row_id(const uchar* record, int64_t* id) {
    TABLE_SHARE* share = table->s;
    if (share->primary_key >= share->keys) return false;
    const KEY& pk = share->key_info[share->primary_key];
    if (pk.user_defined_key_parts != 1) return false;
    Field* f = table->field[pk.key_part[0].fieldnr - 1];
    const my_ptrdiff_t d = static_cast<my_ptrdiff_t>(record - table->record[0]);
    f->move_field_offset(d);
    StoolapValue v{};
    std::string holder;
    const bool ok = !f->is_null() && extract_field(f, v, holder) &&
                    v.value_type == STOOLAP_TYPE_INTEGER;
    f->move_field_offset(-d);
    if (ok) *id = v.v.integer;
    return ok;
}

void ha_stoolap::position(const uchar* record) {
    // A row is named by its row id: the last read's, or else its key's.
    // Every read gives one, so the else is a row MariaDB positions with no
    // read before it, which rnd_pos() refuses (flag 0).
    int64_t id = cur_row_id_;
    const bool known = cur_row_id_valid_ || key_row_id(record, &id);
    ref[0] = known ? 1 : 0;
    std::memcpy(ref + 1, &id, sizeof(id));
}

// Whether an exact-lookup key part must be filtered by MariaDB rather
// than bound as an equality in stoolap's WHERE: stoolap compares bytes
// and whole values, so it misses rows MariaDB's key matches when the part
// covers a column prefix (a(3) binds 'abc' for 'abc1'), uses a ci
// collation (case / accent folding) or a PAD SPACE one ('_bin': 'a' and
// 'a ' are equal). stoolap_trust_binary_strings accepts byte semantics
// for the collation cases. Binary strings compare bytewise in both.
static bool key_part_needs_filter(const KEY_PART_INFO& kp, bool trust_bin) {
    if (kp.key_part_flag & HA_PART_KEY_SEG) return true;
    const Field* f = kp.field;
    if (!f) return true;
    switch (f->real_type()) {
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
            const CHARSET_INFO* cs = f->charset();
            if (!cs || cs == &my_charset_bin || trust_bin) return false;
            return !(cs->state & MY_CS_BINSORT) || !(cs->state & MY_CS_NOPAD);
        }
        default:
            return false;
    }
}

// A filtered part whose collation orders bytes but pads with spaces
// ('_bin'): stoolap looks up the values equal to a key but for trailing
// spaces (StoolapKeyBound::pad), the filter compares them.
static bool key_part_pads(const KEY_PART_INFO& kp, bool trust_bin) {
    return key_part_needs_filter(kp, trust_bin) && kp.field &&
           !(kp.key_part_flag & HA_PART_KEY_SEG) &&
           (kp.field->charset()->state & MY_CS_BINSORT);
}

int ha_stoolap::index_init(uint keynr, bool sorted) {
    active_index = keynr;
    index_sorted_ = sorted;
    key_filter_active_ = false;
    // A read left from a prior rnd_init must not shadow this index stream.
    reset_scan_state();
    return 0;
}

int ha_stoolap::index_end() {
    active_index = MAX_KEY;
    key_filter_active_ = false;
    release_read();
    return 0;
}

int ha_stoolap::index_read_map(uchar* buf, const uchar* key,
                               key_part_map keypart_map,
                               enum ha_rkey_function find_flag) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    reset_scan_state();

    // The comparison on the LAST bound key part and the direction; earlier
    // key parts are always equality.
    int32_t op = STOOLAP_OP_EQ;
    bool desc = false;
    switch (find_flag) {
        case HA_READ_KEY_EXACT:
        case HA_READ_PREFIX:
            op = STOOLAP_OP_EQ;
            break;
        case HA_READ_KEY_OR_NEXT:
            op = STOOLAP_OP_GE;
            break;
        case HA_READ_AFTER_KEY:
            op = STOOLAP_OP_GT;
            break;
        case HA_READ_KEY_OR_PREV:
            op = STOOLAP_OP_LE;
            desc = true;
            break;
        case HA_READ_BEFORE_KEY:
            op = STOOLAP_OP_LT;
            desc = true;
            break;
        default:
            return HA_ERR_UNSUPPORTED;
    }

    KEY& key_info = table->key_info[active_index];

    uint nparts = 0;
    uint klen = 0;
    for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
        if (!(keypart_map & (key_part_map(1) << i))) break;
        nparts = i + 1;
        klen += key_info.key_part[i].store_length;
    }
    if (nparts == 0) return HA_ERR_KEY_NOT_FOUND;

    std::memset(table->record[1], 0, table->s->reclength);
    key_restore(table->record[1], key, &key_info, klen);

    // Exact-lookup parts stoolap cannot test as MariaDB does stay out of
    // the engine conditions; index_next filters them (see
    // key_part_needs_filter).
    const bool trust_bin = stoolap_thd_trust_binary_strings(ha_thd());
    uint32_t filter_mask = 0;
    uint32_t pad_mask = 0;
    if (find_flag == HA_READ_KEY_EXACT) {
        for (uint i = 0; i < nparts; ++i) {
            const KEY_PART_INFO& kp = key_info.key_part[i];
            if (key_part_needs_filter(kp, trust_bin))
                filter_mask |= uint32_t(1) << i;
            if (key_part_pads(kp, trust_bin)) pad_mask |= uint32_t(1) << i;
        }
    }

    // Order when the caller wants sorted rows or reads backward, except
    // for an exact lookup bound by a unique key (one row at most).
    const bool exact_unique_lookup =
        find_flag == HA_READ_KEY_EXACT &&
        nparts == key_info.user_defined_key_parts &&
        (key_info.flags & HA_NOSAME);
    ir_order_.clear();
    if (!exact_unique_lookup && (index_sorted_ || desc)) {
        // A filtered part is an equality MariaDB's collation decides: its
        // matches all sort as one key value, so ordering by their stoolap
        // bytes would break the order of the parts after it. So is a part
        // bound by '=' (or IS NULL): every row has the same value there,
        // and sorting by it only costs a sort.
        const bool last_eq = op == STOOLAP_OP_EQ;
        for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
            if (filter_mask & (uint32_t(1) << i)) continue;
            if (i + 1 < nparts || (i + 1 == nparts && last_eq)) continue;
            ir_order_.push_back(
                static_cast<int32_t>(key_info.key_part[i].field->field_index));
        }
    }

    const StoolapKeyBound bound{table->record[1],
                                static_cast<int32_t>(nparts),
                                op,
                                filter_mask,
                                pad_mask,
                                STOOLAP_BOUND_KEY};
    if (int err = open_read(&bound, 1, ir_order_.data(),
                            static_cast<int32_t>(ir_order_.size()), desc))
        return err;

    if (filter_mask) {
        key_filter_active_ = true;
        key_filter_key_ = &key_info;
        key_filter_buf_.assign(key, key + klen);
    }
    int next_rc = index_next(buf);
    return (next_rc == HA_ERR_END_OF_FILE) ? HA_ERR_KEY_NOT_FOUND : next_rc;
}

int ha_stoolap::index_next(uchar* buf) {
    if (!key_filter_active_ || !key_filter_key_) return rnd_next(buf);
    // key_cmp() reads the fields at record[0]; point them at buf.
    const my_ptrdiff_t off = static_cast<my_ptrdiff_t>(buf - table->record[0]);
    while (true) {
        int r = rnd_next(buf);
        if (r != 0) return r;
        if (off) move_fields(table, off);
        const int c = key_cmp(key_filter_key_->key_part, key_filter_buf_.data(),
                              static_cast<uint>(key_filter_buf_.size()));
        if (off) move_fields(table, -off);
        if (c == 0) return 0;
    }
}

int ha_stoolap::index_first(uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    return index_end_read(buf, false);
}

int ha_stoolap::index_last(uchar* buf) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    return index_end_read(buf, true);
}

// The whole table in the active index's order, from its first row (or
// its last, `desc`). Besides MIN / MAX probes, which read one row,
// MariaDB runs ordered full index scans
// (UPDATE / DELETE ... ORDER BY pk LIMIT n) as index_first then
// index_next, which reads on. (Nothing scans backwards: no HA_READ_PREV.)
int ha_stoolap::index_end_read(uchar* buf, bool desc) {
    reset_scan_state();
    KEY& ki = table->key_info[active_index];
    ir_order_.clear();
    for (uint i = 0; i < ki.user_defined_key_parts; ++i)
        ir_order_.push_back(
            static_cast<int32_t>(ki.key_part[i].field->field_index));
    if (int err = open_read(nullptr, 0, ir_order_.data(),
                            static_cast<int32_t>(ir_order_.size()), desc))
        return err;
    // An empty table is the end of the index: READ_RECORD's index_first
    // (UPDATE / DELETE ... ORDER BY) takes any other code as an error.
    return rnd_next(buf);
}

int ha_stoolap::read_range_first(const key_range* start_key,
                                 const key_range* end_key, bool eq_range,
                                 bool sorted) {
    if (stoolap_table_.empty()) return HA_ERR_INTERNAL_ERROR;
    if (active_index >= table->s->keys) return HA_ERR_INTERNAL_ERROR;
    reset_scan_state();

    KEY& key_info = table->key_info[active_index];

    // Ranges stoolap cannot compare as MariaDB does go to
    // read_range_filtered: a prefix part, a ci or PAD SPACE collation
    // (key_part_needs_filter), a hex encoding out of order; NEWDECIMAL /
    // ENUM / SET too (encoded bound semantics not re-validated).
    const bool trust_bin = stoolap_thd_trust_binary_strings(ha_thd());
    auto contains_unsafe_part = [&](const key_range* range) {
        if (!range) return false;
        for (uint i = 0; i < key_info.user_defined_key_parts; ++i) {
            if (!(range->keypart_map & (key_part_map(1) << i))) break;
            Field* f = key_info.key_part[i].field;
            switch (f->real_type()) {
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_VAR_STRING:
                case MYSQL_TYPE_STRING:
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB: {
                    if (stoolap_mariadb::field_enc(f) ==
                            stoolap_mariadb::Enc::HEX &&
                        !stoolap_mariadb::hex_keeps_order(f))
                        return true;
                    if (key_part_needs_filter(key_info.key_part[i], trust_bin))
                        return true;
                    break;
                }
                case MYSQL_TYPE_NEWDECIMAL:
                case MYSQL_TYPE_ENUM:
                case MYSQL_TYPE_SET:
                    return true;
                default:
                    break;
            }
        }
        return false;
    };
    if (contains_unsafe_part(start_key) || contains_unsafe_part(end_key)) {
        return read_range_filtered(start_key, end_key, sorted);
    }

    // The comparison for the last bound part; earlier parts are equality.
    auto op_for_start = [](ha_rkey_function f) -> int32_t {
        switch (f) {
            case HA_READ_KEY_EXACT:
            case HA_READ_PREFIX:
                return STOOLAP_OP_EQ;
            case HA_READ_KEY_OR_NEXT:
                return STOOLAP_OP_GE;
            case HA_READ_AFTER_KEY:
                return STOOLAP_OP_GT;
            case HA_READ_KEY_OR_PREV:
                return STOOLAP_OP_LE;
            case HA_READ_BEFORE_KEY:
                return STOOLAP_OP_LT;
            default:
                return -1;  // unsupported
        }
    };
    auto op_for_end = [](ha_rkey_function f) -> int32_t {
        // Upper bound convention: HA_READ_AFTER_KEY = "<= K" (inclusive),
        // HA_READ_BEFORE_KEY = "< K" (exclusive).
        switch (f) {
            case HA_READ_AFTER_KEY:
                return STOOLAP_OP_LE;
            case HA_READ_BEFORE_KEY:
                return STOOLAP_OP_LT;
            default:
                return -1;
        }
    };

    auto count_parts = [](const KEY& ki, key_part_map map, uint& nparts,
                          uint& klen) {
        nparts = klen = 0;
        for (uint i = 0; i < ki.user_defined_key_parts; ++i) {
            if (!(map & (key_part_map(1) << i))) break;
            nparts = i + 1;
            klen += ki.key_part[i].store_length;
        }
    };

    // Each bound's key restored into a record of its own.
    StoolapKeyBound bounds[2];
    int32_t nbounds = 0;
    auto add_bound = [&](const key_range* range, int32_t op, uchar* rec) {
        if (op < 0) return HA_ERR_UNSUPPORTED;
        uint nparts = 0, klen = 0;
        count_parts(key_info, range->keypart_map, nparts, klen);
        if (nparts == 0) return 0;
        std::memset(rec, 0, table->s->reclength);
        key_restore(rec, range->key, &key_info, klen);
        bounds[nbounds++] = {rec, static_cast<int32_t>(nparts), op, 0,
                             0,   STOOLAP_BOUND_RANGE};
        return 0;
    };
    if (start_key) {
        if (int rc = add_bound(start_key, op_for_start(start_key->flag),
                               table->record[1]))
            return rc;
    }
    if (end_key) {
        if (int rc =
                add_bound(end_key, op_for_end(end_key->flag), bound_record()))
            return rc;
    }
    ir_order_.clear();

    // Skip the order when an exact unique-index lookup (one row) or when
    // MariaDB's `sorted` flag says the range plan doesn't need it.
    bool exact_unique = false;
    if (start_key && start_key->flag == HA_READ_KEY_EXACT &&
        (key_info.flags & HA_NOSAME)) {
        uint sk_parts = 0, sk_klen = 0;
        count_parts(key_info, start_key->keypart_map, sk_parts, sk_klen);
        exact_unique = (sk_parts == key_info.user_defined_key_parts);
    }
    if (!exact_unique && sorted) {
        for (uint i = 0; i < key_info.user_defined_key_parts; ++i)
            ir_order_.push_back(
                static_cast<int32_t>(key_info.key_part[i].field->field_index));
    }

    // An empty range is the end of it, as handler::read_range_first has
    // it: HA_ERR_KEY_NOT_FOUND here is an error to the caller.
    if (int err = open_read(bounds, nbounds, ir_order_.data(),
                            static_cast<int32_t>(ir_order_.size()), false))
        return err == HA_ERR_KEY_NOT_FOUND ? HA_ERR_END_OF_FILE : err;
    return rnd_next(table->record[0]);
}

int ha_stoolap::read_range_next() {
    if (range_filter_active_) {
        while (true) {
            const int r = rnd_next(table->record[0]);
            if (r) return r;
            if (range_in_bounds()) return 0;
        }
    }
    return index_next(table->record[0]);
}

// The row at record[0] lies inside the range by MariaDB's own key
// comparison (collation, prefix and NULL aware).
bool ha_stoolap::range_in_bounds() {
    KEY& ki = table->key_info[active_index];
    if (range_has_start_) {
        const int c =
            key_cmp(ki.key_part, range_start_buf_.data(), range_start_len_);
        switch (range_start_flag_) {
            case HA_READ_AFTER_KEY:
                if (c <= 0) return false;
                break;
            case HA_READ_KEY_EXACT:
            case HA_READ_PREFIX:
                if (c != 0) return false;
                break;
            default:  // HA_READ_KEY_OR_NEXT
                if (c < 0) return false;
                break;
        }
    }
    return !end_range || compare_key(end_range) <= 0;
}

// A range over a key part stoolap cannot compare as MariaDB does (a ci
// collation, DECIMAL / ENUM / SET, a multi-byte hex charset): stoolap's
// order is not the key's, so neither its WHERE nor the usual stop at the
// first row past the end can be trusted. Stoolap returns candidates
// bounded only by the leading key part when that part is safe, and
// read_range_next keeps the rows inside both bounds by MariaDB's own
// comparison, reading to the end. With `sorted`, rows come in key order
// over the non-constant parts, up to the first that index_flags offers
// no order on (MariaDB relies on no order past it).
int ha_stoolap::read_range_filtered(const key_range* start_key,
                                    const key_range* end_key, bool sorted) {
    KEY& ki = table->key_info[active_index];
    const bool trust_bin = stoolap_thd_trust_binary_strings(ha_thd());
    // Ordered and compared by stoolap as by MariaDB: not a prefix, PAD
    // SPACE or ci string (key_part_needs_filter), nor one index_flags
    // offers no order on.
    auto part_safe = [&](uint i) {
        return ki.key_part[i].field &&
               (index_flags(active_index, i, false) & HA_READ_ORDER) &&
               !key_part_needs_filter(ki.key_part[i], trust_bin);
    };
    // Leading parts every row of the range shares: the bounds agree on
    // them, byte for byte.
    uint const_prefix = 0;
    if (start_key && end_key) {
        uint off = 0;
        while (const_prefix < ki.user_defined_key_parts &&
               (start_key->keypart_map & end_key->keypart_map &
                (key_part_map(1) << const_prefix))) {
            const uint len = ki.key_part[const_prefix].store_length;
            if (std::memcmp(start_key->key + off, end_key->key + off, len))
                break;
            off += len;
            ++const_prefix;
        }
    }

    ir_order_.clear();
    StoolapKeyBound bounds[2];
    int32_t nbounds = 0;
    if (part_safe(0)) {
        // Every row of the range has its first part between the bounds'
        // first parts (inclusive, whatever the flags); NULLs sort first,
        // so an upper bound keeps them.
        for (const key_range* r : {start_key, end_key}) {
            if (!r || !(r->keypart_map & 1)) continue;
            uchar* rec = r == start_key ? table->record[1] : bound_record();
            std::memset(rec, 0, table->s->reclength);
            key_restore(rec, r->key, &ki, ki.key_part[0].store_length);
            bounds[nbounds++] = {
                rec, 1, r == start_key ? STOOLAP_OP_GE : STOOLAP_OP_LE,
                0,   0, STOOLAP_BOUND_LEADING};
        }
    } else if (const_prefix > 0 && key_part_pads(ki.key_part[0], trust_bin)) {
        // Every row of the range equals the bounds' first part there: the
        // values a PAD SPACE part matches, as index_read_map looks them up.
        std::memset(table->record[1], 0, table->s->reclength);
        key_restore(table->record[1], start_key->key, &ki,
                    ki.key_part[0].store_length);
        bounds[nbounds++] = {table->record[1], 1, STOOLAP_OP_EQ, 1, 1,
                             STOOLAP_BOUND_KEY};
    }
    if (sorted) {
        // The order index_flags promised: every part offering it, past
        // one that does not only when that part is constant, by the
        // bounds or by the WHERE (MariaDB's const_key_parts, which its
        // ORDER BY check skips the same way).
        const key_part_map where_const =
            table->const_key_parts ? table->const_key_parts[active_index] : 0;
        for (uint i = 0; i < ki.user_defined_key_parts; ++i) {
            // A constant part is one value to MariaDB: sorting stoolap's
            // bytes there could split values its collation calls equal
            // ('a', 'a ') and break the order of the parts after it.
            if (i < const_prefix || (where_const & (key_part_map(1) << i)))
                continue;
            if (!ki.key_part[i].field ||
                !(index_flags(active_index, i, false) & HA_READ_ORDER))
                break;
            ir_order_.push_back(
                static_cast<int32_t>(ki.key_part[i].field->field_index));
        }
    }

    if (int err = open_read(bounds, nbounds, ir_order_.data(),
                            static_cast<int32_t>(ir_order_.size()), false))
        return err;

    range_has_start_ = start_key != nullptr;
    if (start_key) {
        range_start_buf_.assign(start_key->key,
                                start_key->key + start_key->length);
        range_start_len_ = start_key->length;
        range_start_flag_ = start_key->flag;
    }
    range_key_part = ki.key_part;
    set_end_range(end_key);
    range_filter_active_ = true;
    return read_range_next();
}

void ha_stoolap::get_auto_increment(ulonglong offset, ulonglong increment,
                                    ulonglong nb_desired_values,
                                    ulonglong* first_value,
                                    ulonglong* nb_reserved_values) {
    // Reserve ids process-wide: per-connection caches collide under
    // concurrent sessions + explicit high-id inserts. Reserving with
    // step=increment makes our cursor jump past every id MariaDB
    // logically issues in this batch (offset/increment is the multi-
    // writer replication ladder).
    *first_value = ~ulonglong(0);
    *nb_reserved_values = nb_desired_values ? nb_desired_values : 1;

    Field* auto_field = table ? table->found_next_number_field : nullptr;
    if (!auto_field) return;

    const uint64_t step = increment ? static_cast<uint64_t>(increment) : 1;
    const uint64_t offset_mod =
        (step > 0) ? static_cast<uint64_t>(offset) % step : 0;

    uint64_t first = 0;
    if (g_engine.ai_reserve(stoolap_table_,
                            static_cast<uint64_t>(*nb_reserved_values), step,
                            offset_mod, &first)) {
        *first_value = static_cast<ulonglong>(first);
        return;
    }

    std::string sql = "SELECT MAX(";
    sql += quote_ident(std::string_view(auto_field->field_name.str,
                                        auto_field->field_name.length));
    sql += ") FROM ";
    sql += quote_ident(stoolap_table_);
    auto* ctx = get_thd_ctx(ha_thd());
    StoolapRows* rows = nullptr;
    if (query_via(ctx, db_raw(), sql.c_str(), &rows) != STOOLAP_OK || !rows) {
        return;
    }
    stoolap_mariadb::RowsPtr scoped(rows);
    uint64_t current_max = 0;
    if (stoolap_rows_next(rows) == STOOLAP_ROW &&
        !stoolap_rows_column_is_null(rows, 0)) {
        // Floor at 0: an explicit negative AI value (INSERT VALUES (-2)
        // ...) gives a negative MAX(); uint64 wrap + 1 overflows and the
        // next generated insert fails ER_AUTOINC_READ_FAILED.
        const int64_t raw = stoolap_rows_column_int64(rows, 0);
        current_max = (raw > 0) ? static_cast<uint64_t>(raw) : 0;
    }
    g_engine.ai_seed_and_reserve(stoolap_table_, current_max + 1,
                                 static_cast<uint64_t>(*nb_reserved_values),
                                 step, offset_mod, &first);
    *first_value = static_cast<ulonglong>(first);
}

void ha_stoolap::invalidate_records_cache() {
    cached_records_valid_ = false;
    if (stoolap_table_.empty()) return;
    if (THD* thd = ha_thd()) {
        auto* ctx = get_thd_ctx(thd);
        if (ctx && ctx->has_tx()) {
            ctx->note_records_dirty(stoolap_table_);
            ctx->records_invalidate(stoolap_table_);
            return;
        }
    }
    g_engine.records_invalidate(stoolap_table_);
}

void ha_stoolap::adjust_records_cache(int64_t delta) {
    if (stoolap_table_.empty()) return;

    if (THD* thd = ha_thd()) {
        auto* ctx = get_thd_ctx(thd);
        if (ctx && ctx->has_tx()) {
            ctx->note_records_dirty(stoolap_table_);
            ctx->records_adjust(stoolap_table_, delta);
            if (cached_records_valid_) {
                uint64_t adjusted = cached_records_;
                if (delta < 0) {
                    const uint64_t dec =
                        static_cast<uint64_t>(-(delta + 1)) + 1;
                    if (dec > adjusted) {
                        cached_records_valid_ = false;
                        ctx->records_invalidate(stoolap_table_);
                        return;
                    }
                    adjusted -= dec;
                } else {
                    const uint64_t inc = static_cast<uint64_t>(delta);
                    if (inc > UINT64_MAX - adjusted) {
                        cached_records_valid_ = false;
                        ctx->records_invalidate(stoolap_table_);
                        return;
                    }
                    adjusted += inc;
                }
                cached_records_ = static_cast<ha_rows>(adjusted);
                stats.records = cached_records_;
            }
            return;
        }
    }

    cached_records_valid_ = false;
    g_engine.records_invalidate(stoolap_table_);
}

void ha_stoolap::apply_count_delta(int64_t delta) {
    adjust_records_cache(delta);
}

void ha_stoolap::set_count_exact(uint64_t value) {
    cached_records_ = static_cast<ha_rows>(value);
    cached_records_valid_ = true;
    stats.records = cached_records_;
    if (!stoolap_table_.empty()) {
        g_engine.records_set(stoolap_table_, value);
    }
}

ha_rows ha_stoolap::cached_records() {
    // Lookup order: handler-local -> tx-local (in tx) or engine-global
    // (autocommit) -> live count via FFI. The tx-local layer is
    // per-THD so snapshot-visible counts never leak between connections.
    if (cached_records_valid_) return cached_records_;
    if (stoolap_table_.empty()) return stats.records;

    THD* thd = ha_thd();
    // Optimizer/stat callers can reach here before external_lock has
    // registered the tx. Register now so COUNT observes the same view
    // the later row access path will use; otherwise a pre-lock probe
    // seeds planning with an autocommit-visible count.
    if (thd && thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {
        if (register_trx(thd) != 0) return stats.records;
    }
    auto* ctx = thd ? get_thd_ctx(thd) : nullptr;
    // Skip cross-session cache during an open tx: tx-side counts see this
    // session's uncommitted writes -- right for us, wrong to publish.
    const bool in_tx = ctx && ctx->has_tx();

    if (in_tx) {
        uint64_t cached = 0;
        if (ctx->records_lookup(stoolap_table_, &cached)) {
            cached_records_ = static_cast<ha_rows>(cached);
            cached_records_valid_ = true;
            stats.records = cached_records_;
            return stats.records;
        }
    } else {
        uint64_t cached = 0;
        if (g_engine.records_lookup(stoolap_table_, &cached)) {
            cached_records_ = static_cast<ha_rows>(cached);
            cached_records_valid_ = true;
            stats.records = cached_records_;
            return stats.records;
        }
    }

    // MVCC-visible COUNT(*): inside the tx it sees this session's own
    // uncommitted writes; outside it answers from the autocommit view
    // (stoolap's quiet-epoch row-count fast path).
    stoolap_mariadb::g_stats.records_live_counts.fetch_add(
        1, std::memory_order_relaxed);
    const std::string sql =
        "SELECT COUNT(*) FROM " + quote_ident(stoolap_table_);
    const uint8_t* buf = nullptr;
    int64_t len = 0;
    int rc = stoolap_query_params_all(warm_db(ctx, db_raw()),
                                      in_tx ? ctx->tx() : nullptr, sql.c_str(),
                                      nullptr, 0, &buf, &len);
    if (rc != STOOLAP_OK || !buf) return stats.records;
    // One row, one INTEGER column: header, u32 row_count, tag, i64.
    size_t pos = 0;
    uint32_t cols = 0;
    if (!stoolap_mariadb::pkt_parse_header(buf, static_cast<size_t>(len), &pos,
                                           &cols) ||
        cols != 1 || pos + 4 + 1 + 8 > static_cast<size_t>(len) ||
        stoolap_mariadb::pkt_le32(buf + pos) != 1 ||
        buf[pos + 4] != STOOLAP_TYPE_INTEGER) {
        return stats.records;
    }
    const int64_t n = stoolap_mariadb::pkt_i64(buf + pos + 5);
    const uint64_t count = n > 0 ? static_cast<uint64_t>(n) : 0;

    cached_records_ = static_cast<ha_rows>(count);
    cached_records_valid_ = true;
    stats.records = cached_records_;
    if (in_tx) {
        ctx->records_set(stoolap_table_, count);
    }
    // Never publish here to the process-wide cache: pre-external_lock
    // probes can't reliably distinguish autocommit from in-tx, so a
    // tx-visible count could leak across sessions. DDL (TRUNCATE) seeds
    // exact global values via set_count_exact instead.
    return stats.records;
}

ha_rows ha_stoolap::records() {
    // Keep this inexpensive for optimizer/stat calls. The handler does not
    // advertise HA_HAS_RECORDS because then MariaDB may use this as the
    // user-visible result for SELECT COUNT(*) FROM t, which requires stronger
    // MVCC guarantees than a shared statistics cache can provide.
    return cached_records();
}

ha_rows ha_stoolap::records_in_range(uint inx, const key_range* min_key,
                                     const key_range* max_key,
                                     page_range* /*res*/) {
    // The default handler::records_in_range returns 10, which is the
    // optimizer's signal of "I have no idea." That number drives join
    // ordering and index-vs-scan choices. No stoolap-side histograms,
    // so divide row count by rec_per_key when populated, else estimate
    // by bound shape.
    if (inx >= table->s->keys) return 10;
    const KEY& k = table->key_info[inx];
    // records_in_range() can fire before HA_STATUS_VARIABLE has warmed
    // stats.records, so use cached_records directly.
    const ha_rows cached_total = cached_records();
    const ha_rows total = cached_total ? cached_total : 1;

    // ci-leading ref runs a full scan in index_next (no stoolap-side ci
    // index). Report `total` so the planner treats ci ref as no cheaper
    // than a scan, picking sane join order / hash join. trust_binary_strings
    // opts into byte-equal lookup where ref really is O(1)-ish.
    if (!stoolap_thd_trust_binary_strings(ha_thd())) {
        for (uint i = 0; i < k.user_defined_key_parts; ++i) {
            Field* f = k.key_part[i].field;
            if (!f) continue;
            switch (f->real_type()) {
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_VAR_STRING:
                case MYSQL_TYPE_STRING:
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB: {
                    CHARSET_INFO* cs = f->charset();
                    if (cs && !(cs->state & MY_CS_BINSORT)) {
                        return total;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Equality lookup on a unique key is at most one row.
    if (min_key && max_key && min_key->keypart_map == max_key->keypart_map &&
        min_key->flag == HA_READ_KEY_EXACT &&
        max_key->flag == HA_READ_AFTER_KEY && (k.flags & HA_NOSAME)) {
        return 1;
    }

    // Estimate cardinality via stored rec_per_key if MariaDB has populated
    // it (typically zero for our handler since we don't run ANALYZE-style
    // statistics collection, but cheap to consult).
    if (k.actual_rec_per_key(0) > 0) {
        const double rpk = k.actual_rec_per_key(0);
        return static_cast<ha_rows>(rpk);
    }

    // Coarse estimate by bound shape (HA_KEY_SCAN_NOT_ROR keeps us out
    // of index_merge_intersect plans):
    //   exact eq on non-unique key: ~0.1%
    //   tight range (both bounds):  ~1%
    //   half-open range:            ~10%
    // Floor at 10 so tiny tables don't pick bad plans.
    const bool exact =
        (min_key && max_key && min_key->flag == HA_READ_KEY_EXACT);
    const bool tight = (min_key && max_key && !exact);
    ha_rows est;
    if (exact)
        est = total / 1000;
    else if (tight)
        est = total / 100;
    else
        est = total / 10;
    if (est < 10) est = 10;
    return est;
}

IO_AND_CPU_COST ha_stoolap::scan_time() {
    // We have no file length (stoolap is in-memory / behind FFI); the
    // default scan_time() returns 0. Approximate as stats.records so the
    // ci ref cost gate below has a non-zero quantity to multiply by.
    IO_AND_CPU_COST cost;
    const double n = static_cast<double>(stats.records ? stats.records : 1);
    cost.io = n * 0.5;
    cost.cpu = n * 0.5;
    return cost;
}

IO_AND_CPU_COST ha_stoolap::keyread_time(uint index, ulong ranges,
                                         ha_rows /*rows*/, ulonglong blocks) {
    if (index >= table->s->keys || stoolap_thd_trust_binary_strings(ha_thd())) {
        return handler::keyread_time(index, ranges,
                                     /*rows=*/stats.records, blocks);
    }
    const KEY& k = table->key_info[index];
    if (k.user_defined_key_parts == 0) {
        return handler::keyread_time(index, ranges,
                                     /*rows=*/stats.records, blocks);
    }
    bool ci = false;
    for (uint i = 0; i < k.user_defined_key_parts && !ci; ++i) {
        Field* f = k.key_part[i].field;
        if (!f) continue;
        switch (f->real_type()) {
            case MYSQL_TYPE_VARCHAR:
            case MYSQL_TYPE_VAR_STRING:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB: {
                CHARSET_INFO* cs = f->charset();
                ci = cs && !(cs->state & MY_CS_BINSORT);
                break;
            }
            default:
                break;
        }
    }
    if (!ci) {
        return handler::keyread_time(index, ranges,
                                     /*rows=*/stats.records, blocks);
    }
    // ci fallback walks every row in index_next, so each probe is a
    // full scan. Override caller's `rows` (which is rec_per_key and
    // would leak ci-folded counts across sessions if persisted).
    IO_AND_CPU_COST scan = scan_time();
    const double mult = static_cast<double>(ranges ? ranges : 1);
    IO_AND_CPU_COST cost;
    cost.io = scan.io * mult;
    cost.cpu = scan.cpu * mult;
    return cost;
}

int ha_stoolap::info(uint flag) {
    stats.deleted = 0;
    stats.data_file_length = 0;
    stats.index_file_length = 0;

    if (flag & HA_STATUS_ERRKEY) {
        // get_dup_key() calls us to re-publish the violated key.
        errkey = last_dup_key_;
    }

    // Cache-only refresh on purpose: info() fires several times per
    // statement and a mutation loop invalidates after each row, so
    // calling cached_records() here would re-COUNT N times per stmt.
    // User-visible SELECT COUNT(*) still routes through records().
    if (flag & HA_STATUS_VARIABLE) {
        if (!stoolap_table_.empty()) {
            if (cached_records_valid_) {
                stats.records = cached_records_;
            } else {
                uint64_t cached = 0;
                THD* thd = ha_thd();
                auto* ctx = thd ? static_cast<stoolap_mariadb::ThdContext*>(
                                      thd_get_ha_data(thd, stoolap_hton))
                                : nullptr;
                const bool in_tx = ctx && ctx->has_tx();
                if (in_tx && ctx->records_lookup(stoolap_table_, &cached)) {
                    cached_records_ = static_cast<ha_rows>(cached);
                    cached_records_valid_ = true;
                    stats.records = cached_records_;
                } else if (!in_tx &&
                           g_engine.records_lookup(stoolap_table_, &cached)) {
                    cached_records_ = static_cast<ha_rows>(cached);
                    cached_records_valid_ = true;
                    stats.records = cached_records_;
                }
                // Both caches missed: keep prior stats.records; the 1000
                // placeholder below avoids "Impossible WHERE" on a fresh
                // load.
            }
        }
        if (stats.records == 0) stats.records = 1000;
    }

    // Cardinality steering for ci-leading-key indexes (the handler's
    // ci fallback runs a full scan per ref probe in index_next) lives
    // entirely in per-call hooks: keyread_time and records_in_range.
    // Writing KEY::rec_per_key here would persist across the same
    // session's later trust_binary_strings=1 queries and silently feed
    // ci-folded results to a session that asked for byte-exact ones.
    return 0;
}

THR_LOCK_DATA** ha_stoolap::store_lock(THD* /*thd*/, THR_LOCK_DATA** to,
                                       enum thr_lock_type /*lock_type*/) {
    // Skeleton: no engine-level lock yet. Stoolap manages its own concurrency
    // via MVCC, so we return the buffer unchanged (BLACKHOLE-style).
    return to;
}

int ha_stoolap::external_lock(THD* thd, int lock_type) {
    // Lock acquisition runs at statement start; F_UNLCK at end-of-statement.
    if (lock_type == F_UNLCK) return 0;
    return register_trx(thd);
}

// ---------------------------------------------------------------------------
// Plugin declaration
// ---------------------------------------------------------------------------

static MYSQL_SYSVAR_STR(
    dsn, stoolap_dsn_var, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Stoolap DSN (e.g. memory:// or file:///var/lib/stoolap)",
    /*check=*/nullptr, /*update=*/nullptr,
    /*default=*/"memory://");

// MYSQL_SYSVAR writes here; the update callback mirrors into the atomic
// in stoolap_bridge.cc that the hot paths read without taking THD ctx.
static char stoolap_perf_trace_var = 0;
static void stoolap_perf_trace_update(MYSQL_THD, struct st_mysql_sys_var*,
                                      void* var_ptr, const void* save) {
    const char v = *static_cast<const char*>(save);
    *static_cast<char*>(var_ptr) = v;
    stoolap_mariadb::g_perf_trace_enabled.store(v != 0,
                                                std::memory_order_relaxed);
}

static MYSQL_SYSVAR_BOOL(
    perf_trace, stoolap_perf_trace_var, PLUGIN_VAR_OPCMDARG,
    "Enable per-query / per-row pushdown timing in Stoolap_perf_*. "
    "Off by default -- the timing path costs ~50ns per next_row() which "
    "is visible on microsecond-scale point queries. Flip on temporarily "
    "when investigating pushdown latency.",
    /*check=*/nullptr, stoolap_perf_trace_update,
    /*default=*/false);

static MYSQL_THDVAR_BOOL(
    trust_binary_strings, PLUGIN_VAR_OPCMDARG,
    "Push string predicates / sorts / groups to stoolap even on "
    "non-binary VARCHAR/TEXT columns. Stoolap compares bytes; MariaDB "
    "compares with collation. Caller takes responsibility for the "
    "semantic difference. Off by default.",
    /*check=*/nullptr, /*update=*/nullptr,
    /*default=*/false);

static MYSQL_THDVAR_BOOL(
    explain_pushdown, PLUGIN_VAR_OPCMDARG,
    "Log stoolap's EXPLAIN of every pushed SELECT to the server error "
    "log. Diagnostic only; off by default.",
    /*check=*/nullptr, /*update=*/nullptr,
    /*default=*/false);

static struct st_mysql_sys_var* stoolap_system_variables[] = {
    MYSQL_SYSVAR(dsn), MYSQL_SYSVAR(perf_trace),
    MYSQL_SYSVAR(trust_binary_strings), MYSQL_SYSVAR(explain_pushdown),
    nullptr};

extern "C" int stoolap_thd_trust_binary_strings(MYSQL_THD thd) {
    return THDVAR(thd, trust_binary_strings) ? 1 : 0;
}

extern "C" int stoolap_thd_explain_pushdown(MYSQL_THD thd) {
    return THDVAR(thd, explain_pushdown) ? 1 : 0;
}

struct st_mysql_storage_engine stoolap_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

// SHOW_LONGLONG over std::atomic<uint64_t>: same 8 bytes; relaxed load
// is a plain load on aarch64; approximate counts are fine for SHOW STATUS.
// Single-line _SS_PTR macro to avoid LLVM 18 vs 22 backslash-continuation
// disagreement that would break the format gate.
#define _SS_PTR(member) \
    reinterpret_cast<char*>(&stoolap_mariadb::g_stats.member)

static struct st_mysql_show_var stoolap_status_vars[] = {
    {"Stoolap_pushdown_hits", _SS_PTR(pushdown_hits), SHOW_LONGLONG},
    {"Stoolap_pushdown_misses", _SS_PTR(pushdown_misses), SHOW_LONGLONG},
    {"Stoolap_direct_modify_hits", _SS_PTR(direct_modify_hits), SHOW_LONGLONG},
    {"Stoolap_records_live_counts", _SS_PTR(records_live_counts),
     SHOW_LONGLONG},
    {"Stoolap_buffered_scans", _SS_PTR(buffered_scans), SHOW_LONGLONG},
    {"Stoolap_buffered_rows", _SS_PTR(buffered_rows), SHOW_LONGLONG},
    // Strict drift counter (case_18 pins at 0); see PushdownStats.
    {"Stoolap_unmapped_errors", _SS_PTR(unmapped_errors), SHOW_LONGLONG},
    // Per-phase ns; divide by Stoolap_perf_query_count (or
    // _next_row_count) for averages. Two clock_gettime per phase (~20ns).
    {"Stoolap_perf_factory_setup_ns", _SS_PTR(perf_factory_setup_ns),
     SHOW_LONGLONG},
    {"Stoolap_perf_eager_query_ns", _SS_PTR(perf_eager_query_ns),
     SHOW_LONGLONG},
    {"Stoolap_perf_init_scan_ns", _SS_PTR(perf_init_scan_ns), SHOW_LONGLONG},
    {"Stoolap_perf_next_row_ns", _SS_PTR(perf_next_row_ns), SHOW_LONGLONG},
    {"Stoolap_perf_end_scan_ns", _SS_PTR(perf_end_scan_ns), SHOW_LONGLONG},
    {"Stoolap_perf_query_count", _SS_PTR(perf_query_count), SHOW_LONGLONG},
    {"Stoolap_perf_next_row_count", _SS_PTR(perf_next_row_count),
     SHOW_LONGLONG},
    {nullptr, nullptr, SHOW_UNDEF},
};

#undef _SS_PTR

maria_declare_plugin(stoolap){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &stoolap_storage_engine,
    "STOOLAP",
    "Stoolap Contributors",
    "Stoolap embedded SQL engine bridged as a MariaDB storage engine",
    PLUGIN_LICENSE_BSD, /* see README: revisit Apache vs MariaDB plugin license */
    stoolap_init_func,
    stoolap_done_func,
    0x0001,              /* version 0.1 */
    stoolap_status_vars, /* status variables */
    stoolap_system_variables,
    "0.1.0",
    MariaDB_PLUGIN_MATURITY_ALPHA} maria_declare_plugin_end;
