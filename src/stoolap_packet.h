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

// The packed binary format of stoolap_query_params_all(), and the reads'
// record helpers: the field descriptions a read writes rows by
// (record_field) and the store callback for the values it leaves to
// Field::store (store_cell, one cell of this format at a time).
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

#include "stoolap_mariadb.h"

#include "field.h"
#include "table.h"
#include "my_global.h"
#include "stoolap_types.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

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
            const int64_t v = pkt_i64(buf + *pos);
            *pos += 8;
            store_int(f, v);  // decodes by f's type (stoolap_types.h)
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
            store_text(f, s, bl);
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

// What a read into `table` writes for its field `index`, from the read's
// column `column` (see StoolapField).
inline StoolapField record_field(TABLE* table, uint index, int32_t column) {
    Field* f = table->field[index];
    const uchar* rec = table->record[0];
    StoolapField d{};
    d.column = column;
    d.field = static_cast<int32_t>(index);
    d.offset = static_cast<uint32_t>(f->ptr - rec);
    d.pack_length = f->pack_length();
    if (f->null_ptr) {
        d.null_offset = static_cast<uint32_t>(f->null_ptr - rec);
        d.null_mask = f->null_bit;
    }
    describe_field(f, &d);
    return d;
}

// The store callback of a read into `table`, for a value
// stoolap_cursor_next leaves to Field::store; the fields point into the
// record the read writes. Keeps the error for the read's failure.
//
// Field_blob::store keeps its bytes in the field's own buffer, which the
// next store into that field, MariaDB's included, overwrites: a row read
// into record[1] would share it with the row MariaDB builds in record[0]
// (UPDATE's old and new rows, REPLACE and ON DUPLICATE KEY UPDATE's
// duplicate). With `blobs`, a read moves each BLOB's bytes into its own
// buffer (one per field, valid until the next row), as InnoDB keeps them
// in its own heap.
struct RecordTarget {
    TABLE* table = nullptr;
    int err = 0;
    std::vector<std::string>* blobs = nullptr;
};

// A TEXT / BLOB cell's bytes into `own`, the field pointing at them, with
// no Field::store (which writes the field's shared buffer first). False
// for a cell this does not take, which goes through Field::store.
inline bool store_blob_owned(Field_blob* b, const uint8_t* cell, size_t len,
                             std::string& own) {
    // Its store compresses (create() refuses such columns)
    if (b->compression_method()) return false;
    if (len < 5) return false;
    const uint8_t tag = cell[0];
    if (tag != STOOLAP_TYPE_TEXT && tag != STOOLAP_TYPE_BLOB &&
        tag != STOOLAP_TYPE_JSON)
        return false;
    const uint32_t n = pkt_le32(cell + 1);
    if (5 + size_t(n) > len) return false;
    const char* s = reinterpret_cast<const char*>(cell + 5);
    if (tag == STOOLAP_TYPE_TEXT && field_enc(b) == Enc::HEX) {
        // A binary or non-utf8 field's text is the hex of its bytes.
        if (n % 2) return false;
        own.resize(n / 2);
        for (uint32_t i = 0; i < n / 2; ++i) {
            const int hi = hex_nibble(s[2 * i]), lo = hex_nibble(s[2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            own[i] = static_cast<char>((hi << 4) | lo);
        }
    } else {
        own.assign(s, n);
    }
    if (own.size() > b->max_data_length()) return false;
    b->set_notnull();
    b->set_ptr(static_cast<uint32>(own.size()),
               reinterpret_cast<uchar*>(own.data()));
    return true;
}

inline int32_t store_cell(void* ctx, int32_t field, const uint8_t* cell,
                          size_t len) {
    auto* t = static_cast<RecordTarget*>(ctx);
    Field* f = t->table->field[field];
    if (t->blobs && (f->flags & BLOB_FLAG)) {
        if (t->blobs->size() <= static_cast<size_t>(field))
            t->blobs->resize(t->table->s->fields);
        if (store_blob_owned(static_cast<Field_blob*>(f), cell, len,
                             (*t->blobs)[field]))
            return 0;
    }
    size_t pos = 0;
    const int err = pkt_store_value(cell, len, &pos, f);
    if (err) t->err = err;
    return err;
}

}  // namespace stoolap_mariadb
