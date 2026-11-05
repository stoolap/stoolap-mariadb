/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

// How each MariaDB column type is stored in stoolap, and the conversions
// both ways. One place, so the writer (extract_field), both readers (the
// packed buffer and the streaming cursor), CREATE TABLE and the SQL
// generator agree.
//
// The aim is that stoolap's own comparisons and ordering match MariaDB's:
// - DATE, DATETIME and TIME are INTEGERs in MariaDB's pack_time() form,
//   the value MariaDB itself compares (zero and partial dates, 0000-9999,
//   negative and >99 h times included). A DATE is the DATETIME at
//   midnight, so DATE and DATETIME columns compare with each other.
// - TIMESTAMP is an INTEGER: UTC microseconds since the epoch, the value
//   MariaDB stores; 0 is the zero timestamp.
// - ENUM is its index and SET its bit mask, the values MariaDB sorts by.
// - Binary strings, and text in a character set other than utf8 /
//   ascii, are TEXT holding the bytes in uppercase hex: stoolap TEXT must
//   be UTF-8, and hex keeps byte order and equality (so a latin1_bin key
//   orders as MariaDB's, which converting to utf8 would not).
// Everything else (integers, doubles, utf8 / ascii text, DECIMAL as its
// decimal string) is stored as is.
//
// Requires field.h (MYSQL_SERVER) before inclusion.

#pragma once

#include "stoolap_mariadb.h"

#include "sql_time.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace stoolap_mariadb {

enum class Enc : uint8_t {
    PLAIN,
    PACKED_TIME,  // DATE, DATETIME, TIME
    UTC_MICROS,   // TIMESTAMP
    INDEX,        // ENUM, SET
    HEX,          // binary strings, text in non-utf8 character sets
};

// Text whose bytes are already valid UTF-8 in the same order.
// Called per cell: a flag test for the usual utf8mb3 / utf8mb4.
inline bool text_is_utf8(const CHARSET_INFO* cs) {
    if (!cs) return false;
    if (cs->state & MY_CS_UNICODE) return cs->mbminlen == 1;
    return cs->cs_name.str && std::strcmp(cs->cs_name.str, "ascii") == 0;
}

inline Enc field_enc(const Field* f) {
    switch (f->real_type()) {
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
            return Enc::PACKED_TIME;
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
            return Enc::UTC_MICROS;
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
            return Enc::INDEX;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            if (text_is_utf8(f->charset())) return Enc::PLAIN;
            // Plugin data types (INET6, UUID, ...) report a binary STRING
            // but read and write their text form, stored as is.
            if (!f->type_handler()->is_traditional_scalar_type())
                return Enc::PLAIN;
            return Enc::HEX;
        default:
            return Enc::PLAIN;
    }
}

// Whether a hex-stored column's stoolap order (its bytes) is MariaDB's:
// true for binary strings and single-byte character sets, whose _bin
// collations compare bytes; a multi-byte one (utf16le_bin, ...) compares
// code points. Equality holds either way.
inline bool hex_keeps_order(const Field* f) {
    const CHARSET_INFO* cs = f->charset();
    return !cs || cs == &my_charset_bin || cs->mbmaxlen == 1;
}

inline enum_mysql_timestamp_type packed_time_type(const Field* f) {
    switch (f->real_type()) {
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
            return MYSQL_TIMESTAMP_TIME;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
            return MYSQL_TIMESTAMP_DATE;
        default:
            return MYSQL_TIMESTAMP_DATETIME;
    }
}

// The pack_time() value of a DATE / DATETIME / TIME field.
inline int64_t pack_field_time(Field* f) {
    MYSQL_TIME mt;
    std::memset(&mt, 0, sizeof(mt));
    if (packed_time_type(f) == MYSQL_TIMESTAMP_TIME)
        f->get_date(&mt, date_mode_t(date_mode_t::TIME_ONLY));
    else
        f->get_date(&mt, date_mode_t(date_mode_t::FUZZY_DATES |
                                     date_mode_t::INVALID_DATES));
    return static_cast<int64_t>(pack_time(&mt));
}

// Writer side. For text, `holder` keeps the bytes alive for the FFI call.
// False: the value has no stoolap representation.
inline bool encode_field(Field* f, StoolapValue& v, std::string& holder) {
    switch (field_enc(f)) {
        case Enc::PACKED_TIME:
            v.value_type = STOOLAP_TYPE_INTEGER;
            v.v.integer = pack_field_time(f);
            return true;
        case Enc::UTC_MICROS: {
            ulong usec = 0;
            const my_time_t secs = f->get_timestamp(&usec);
            v.value_type = STOOLAP_TYPE_INTEGER;
            v.v.integer = static_cast<int64_t>(secs) * 1000000 +
                          static_cast<int64_t>(usec);
            return true;
        }
        case Enc::INDEX:
            v.value_type = STOOLAP_TYPE_INTEGER;
            v.v.integer = static_cast<int64_t>(f->val_int());
            return true;
        case Enc::HEX: {
            char tmp[256];
            String tmp_str(tmp, sizeof(tmp), f->charset());
            const String* s = f->val_str(&tmp_str);
            static const char digits[] = "0123456789ABCDEF";
            holder.resize(s->length() * 2);
            const auto* p = reinterpret_cast<const uchar*>(s->ptr());
            for (size_t i = 0; i < s->length(); ++i) {
                holder[2 * i] = digits[p[i] >> 4];
                holder[2 * i + 1] = digits[p[i] & 15];
            }
            v.value_type = STOOLAP_TYPE_TEXT;
            v.v.text.ptr = holder.data();
            v.v.text.len = static_cast<int64_t>(holder.size());
            return true;
        }
        case Enc::PLAIN:
            break;
    }
    return false;  // PLAIN types are written by the caller
}

// Reader side: an INTEGER cell into `f`.
inline void store_int(Field* f, int64_t v) {
    switch (field_enc(f)) {
        case Enc::PACKED_TIME: {
            MYSQL_TIME mt;
            unpack_time(static_cast<longlong>(v), &mt, packed_time_type(f));
            f->store_time_dec(&mt, f->decimals());
            return;
        }
        case Enc::UTC_MICROS:
            static_cast<Field_timestamp*>(f)->store_TIME(
                static_cast<my_time_t>(v / 1000000),
                static_cast<ulong>(v % 1000000));
            return;
        default:
            // Preserve UNSIGNED_FLAG: stoolap's INTEGER is i64, and the
            // writer refuses BIGINT UNSIGNED above INT64_MAX, so the bit
            // pattern round-trips. ENUM / SET take their index / mask.
            f->store(static_cast<longlong>(v), f->is_unsigned());
            return;
    }
}

inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Reader side: a TEXT cell into `f`. Stoolap text is UTF-8; a binary
// field's text is the hex of its bytes.
inline void store_text(Field* f, const char* s, size_t n) {
    const CHARSET_INFO* cs = f->charset();
    if (text_is_utf8(cs)) {  // the usual case, one flag test
        // A utf8mb4 VARCHAR takes valid UTF-8 as it is: no more bytes
        // than the field has characters always fits, so the bytes go in
        // without the copy that checks and counts each character.
        if (f->real_type() == MYSQL_TYPE_VARCHAR &&
            cs->cset == my_charset_utf8mb4_bin.cset && n <= f->char_length()) {
            auto* v = static_cast<Field_varstring*>(f);
            if (v->length_bytes == 1)
                *f->ptr = static_cast<uchar>(n);
            else
                int2store(f->ptr, static_cast<uint16>(n));
            std::memcpy(f->ptr + v->length_bytes, s, n);
            return;
        }
        f->store(s, n, cs);
        return;
    }
    const Enc enc = field_enc(f);
    if (enc == Enc::HEX && n % 2 == 0) {
        std::string bytes(n / 2, '\0');
        bool ok = true;
        for (size_t i = 0; i < n / 2 && ok; ++i) {
            const int hi = hex_nibble(s[2 * i]), lo = hex_nibble(s[2 * i + 1]);
            ok = hi >= 0 && lo >= 0;
            bytes[i] = static_cast<char>((hi << 4) | lo);
        }
        if (ok) {
            // The bytes are in the field's own character set.
            f->store(bytes.data(), bytes.size(), cs);
            return;
        }
    }
    // Anything else (a DECIMAL's string) reads as before.
    f->store(s, n, cs);
}

}  // namespace stoolap_mariadb
