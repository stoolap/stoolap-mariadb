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

// Shared parser for the packed binary format produced by
// stoolap_rows_fetch_all(). Used by both the row-pump full-scan path
// (rnd_init in ha_stoolap.cc) and the pushdown handlers (select_handler,
// derived_handler, unit_handler in ha_stoolap_select.cc) to consume one
// packed buffer per result set instead of issuing per-row /
// per-cell FFI accessors.
//
// Format (little-endian):
//   u32 col_count
//   for each col:    u16 name_len, u8[name_len] name
//   u32 row_count
//   for each row, for each col:
//     u8 type_tag
//     payload:
//       NULL(0)      empty
//       INTEGER(1)   i64
//       FLOAT(2)     f64
//       TEXT(3)      u32 len, u8[len] bytes
//       BOOLEAN(4)   u8
//       TIMESTAMP(5) i64 nanos since epoch
//       JSON(6)      u32 len, u8[len] bytes
//       BLOB(7)      u32 len, u8[len] bytes

#include <stoolap.h>

#include "field.h"
#include "my_global.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace stoolap_mariadb {

inline uint16_t pkt_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t pkt_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t pkt_le64(const uint8_t* p) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i)
        r |= static_cast<uint64_t>(p[i]) << (i * 8);
    return r;
}
inline int64_t pkt_i64(const uint8_t* p) {
    return static_cast<int64_t>(pkt_le64(p));
}
inline double pkt_f64(const uint8_t* p) {
    uint64_t u = pkt_le64(p);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

/// Parse the column-metadata header. On success leaves *pos at the start
/// of the row_count u32. Returns false if the buffer is truncated.
inline bool pkt_parse_header(const uint8_t* buf, size_t len, size_t* pos,
                             uint32_t* cols_out) {
    if (*pos + 4 > len) return false;
    uint32_t cols = pkt_le32(buf + *pos);
    *pos += 4;
    for (uint32_t i = 0; i < cols; ++i) {
        if (*pos + 2 > len) return false;
        uint16_t nlen = pkt_le16(buf + *pos);
        *pos += 2;
        if (*pos + nlen > len) return false;
        *pos += nlen;
    }
    *cols_out = cols;
    return true;
}

/// Skip past one column payload. Used when stoolap returns more columns
/// than the destination wants (schema drift, projection skip).
inline bool pkt_skip_value(const uint8_t* buf, size_t len, size_t* pos) {
    if (*pos + 1 > len) return false;
    uint8_t tag = buf[(*pos)++];
    switch (tag) {
        case STOOLAP_TYPE_NULL:
            return true;
        case STOOLAP_TYPE_INTEGER:
        case STOOLAP_TYPE_FLOAT:
        case STOOLAP_TYPE_TIMESTAMP:
            if (*pos + 8 > len) return false;
            *pos += 8;
            return true;
        case STOOLAP_TYPE_BOOLEAN:
            if (*pos + 1 > len) return false;
            *pos += 1;
            return true;
        case STOOLAP_TYPE_TEXT:
        case STOOLAP_TYPE_JSON:
        case STOOLAP_TYPE_BLOB: {
            if (*pos + 4 > len) return false;
            uint32_t bl = pkt_le32(buf + *pos);
            *pos += 4;
            if (*pos + bl > len) return false;
            *pos += bl;
            return true;
        }
        default:
            return false;
    }
}

/// Read one column's payload and Field::store() it. Returns 0 on success
/// or HA_ERR_* on malformed-buffer / unsupported type.
inline int pkt_store_value(const uint8_t* buf, size_t len, size_t* pos,
                           Field* f) {
    if (*pos + 1 > len) return HA_ERR_GENERIC;
    uint8_t tag = buf[(*pos)++];
    const bool nullable = f->maybe_null();
    if (tag == STOOLAP_TYPE_NULL) {
        if (nullable) {
            f->set_null();
            return 0;
        }
        f->store(static_cast<longlong>(0), false);
        return 0;
    }
    if (nullable) f->set_notnull();

    switch (tag) {
        case STOOLAP_TYPE_INTEGER: {
            if (*pos + 8 > len) return HA_ERR_GENERIC;
            int64_t v = pkt_i64(buf + *pos);
            *pos += 8;
            // Preserve UNSIGNED_FLAG through Field::store. Stoolap's
            // INTEGER is i64, so for non-BIGINT unsigned types (TINYINT
            // / SMALLINT / MEDIUMINT / INT UNSIGNED) the bit pattern
            // fits and Field::store will write the correct unsigned
            // value. For BIGINT UNSIGNED with stored values > INT64_MAX
            // the value comes back here as a negative i64 (the bit
            // pattern), and Field::store(..., unsigned=true) reinterprets
            // it as the original unsigned value -- so round-trip via
            // equality is preserved. (Range/order operations *inside*
            // stoolap on those out-of-range bigint-unsigned values are
            // not faithful; the write path rejects them up front.)
            f->store(v, /*unsigned=*/f->is_unsigned());
            return 0;
        }
        case STOOLAP_TYPE_FLOAT: {
            if (*pos + 8 > len) return HA_ERR_GENERIC;
            double d = pkt_f64(buf + *pos);
            *pos += 8;
            f->store(d);
            return 0;
        }
        case STOOLAP_TYPE_BOOLEAN: {
            if (*pos + 1 > len) return HA_ERR_GENERIC;
            uint8_t b = buf[(*pos)++];
            f->store(static_cast<longlong>(b ? 1 : 0), false);
            return 0;
        }
        case STOOLAP_TYPE_TEXT:
        case STOOLAP_TYPE_JSON: {
            if (*pos + 4 > len) return HA_ERR_GENERIC;
            uint32_t bl = pkt_le32(buf + *pos);
            *pos += 4;
            if (*pos + bl > len) return HA_ERR_GENERIC;
            const char* s = reinterpret_cast<const char*>(buf + *pos);
            *pos += bl;
            f->store(s, bl, f->charset());
            return 0;
        }
        case STOOLAP_TYPE_BLOB: {
            if (*pos + 4 > len) return HA_ERR_GENERIC;
            uint32_t bl = pkt_le32(buf + *pos);
            *pos += 4;
            if (*pos + bl > len) return HA_ERR_GENERIC;
            const char* s = reinterpret_cast<const char*>(buf + *pos);
            *pos += bl;
            f->store(s, bl, &my_charset_bin);
            return 0;
        }
        case STOOLAP_TYPE_TIMESTAMP: {
            if (*pos + 8 > len) return HA_ERR_GENERIC;
            int64_t nanos = pkt_i64(buf + *pos);
            *pos += 8;
            int64_t secs = nanos / 1000000000LL;
            int64_t us = (nanos % 1000000000LL) / 1000LL;
            if (us < 0) {
                us += 1000000;
                secs--;
            }
            struct tm tm_;
            time_t t = static_cast<time_t>(secs);
            gmtime_r(&t, &tm_);
            char tbuf[40];
            int n = std::snprintf(
                tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d.%06lld",
                tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday, tm_.tm_hour,
                tm_.tm_min, tm_.tm_sec, static_cast<long long>(us));
            f->store(tbuf, static_cast<uint>(n), &my_charset_bin);
            return 0;
        }
        default:
            return HA_ERR_UNSUPPORTED;
    }
}

}  // namespace stoolap_mariadb
