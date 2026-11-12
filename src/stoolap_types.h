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
#include <vector>

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

// Reader side: how stoolap_cursor_next writes `f` into the record. The
// kinds it writes itself are the ones whose record bytes the value gives
// as they are; the rest (dates, ENUM, DECIMAL, BLOB, ...) go through
// Field::store, from the store callback.
inline void describe_field(Field* f, StoolapField* d) {
    d->kind = STOOLAP_FIELD_STORE;
    switch (f->real_type()) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
            d->kind = STOOLAP_FIELD_INT;
            d->length = f->pack_length();
            d->is_unsigned = f->is_unsigned();
            return;
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_FLOAT:
            // Fixed decimals round and UNSIGNED clamps, in Field::store.
            if (f->decimals() >= NOT_FIXED_DEC && !f->is_unsigned())
                d->kind = f->real_type() == MYSQL_TYPE_DOUBLE
                              ? STOOLAP_FIELD_DOUBLE
                              : STOOLAP_FIELD_FLOAT;
            return;
        case MYSQL_TYPE_VARCHAR:
            // A COMPRESSED one reports VARCHAR too but stores its own
            // header.
            if (!f->compression_method() &&
                f->charset()->cset == my_charset_utf8mb4_bin.cset) {
                d->kind = STOOLAP_FIELD_VARCHAR;
                d->length = static_cast<Field_varstring*>(f)->length_bytes;
                d->max_chars = f->char_length();
            }
            return;
        default:
            return;
    }
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
        // A COMPRESSED one reports VARCHAR too but stores its own header.
        if (f->real_type() == MYSQL_TYPE_VARCHAR && !f->compression_method() &&
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

// A string column whose collation does not compare its bytes (a `_ci`
// one): stoolap compares TEXT bytewise, so a UNIQUE key over it is kept
// on a column of its collation keys instead (collation_key).
inline bool collation_keyed_type(const Field* f) {
    if (!f || !f->has_charset()) return false;
    switch (f->real_type()) {
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
            break;
        default:
            return false;
    }
    const CHARSET_INFO* cs = f->charset();
    // The Czech collations' keys take no weight count, which the padded
    // key needs, and big5's give values MariaDB holds apart the same
    // weights. A CHAR under NO PAD has InnoDB's UNIQUE and MariaDB's `=`
    // disagree (on ignorable characters). Their UNIQUE keys stay refused
    return cs && cs != &my_charset_bin && !(cs->state & MY_CS_BINSORT) &&
           !(cs->state & MY_CS_STRNXFRM_BAD_NWEIGHTS) &&
           std::strcmp(cs->cs_name.str, "big5") != 0 &&
           !(f->real_type() == MYSQL_TYPE_STRING && (cs->state & MY_CS_NOPAD));
}

// The fields of `share` stoolap keeps a collation key column for, in field
// order: those of collation_keyed_type a UNIQUE key (not the primary key)
// names. The key column of the n-th is stoolap column fields + n.
inline std::vector<uint> collation_keyed_fields(const TABLE_SHARE* share) {
    std::vector<bool> keyed(share->fields, false);
    for (uint i = 0; i < share->keys; ++i) {
        const KEY& k = share->key_info[i];
        if (i == share->primary_key || !(k.flags & HA_NOSAME)) continue;
        for (uint j = 0; j < k.user_defined_key_parts; ++j) {
            const Field* f = k.key_part[j].field;
            if (f && collation_keyed_type(f)) keyed[f->field_index] = true;
        }
    }
    std::vector<uint> out;
    for (uint i = 0; i < share->fields; ++i)
        if (keyed[i]) out.push_back(i);
    return out;
}

// `f`'s collation key: values the collation holds equal have equal keys,
// others differ. It is WEIGHT_STRING(f AS CHAR(n)) for n past the weights
// any value takes (a character takes at most cs->strxfrm_multiply, the
// UCA tables' limit), every key padded to the same length, less the tail
// it shares with the empty string's key (`pad`): the keys still differ
// where the padded ones did. With one level of weights the padded key is
// the value's weights then the space's, so it is computed for the value
// alone, its trailing space weights cut under PAD SPACE (`pad` holds the
// space's). In uppercase hex, as stoolap TEXT. False when the value may
// take more weights than the key holds.
inline bool collation_key(Field* f, std::string& out, std::string& buf,
                          std::string& pad) {
    const CHARSET_INFO* cs = f->charset();
    const uint per_char = cs->strxfrm_multiply ? cs->strxfrm_multiply : 1;
    // The value, a CHAR without its padding whatever the session's
    // PAD_CHAR_TO_FULL_LENGTH (val_str keeps it then); its collation pads
    // with spaces (collation_keyed_type)
    const uchar* src;
    size_t srclen;
    char tmp[MAX_FIELD_WIDTH];
    String value(tmp, sizeof(tmp), cs);
    if (f->real_type() == MYSQL_TYPE_STRING) {
        src = f->ptr;
        srclen = cs->lengthsp(reinterpret_cast<const char*>(f->ptr),
                              f->field_length);
    } else {
        const String* v = f->val_str(&value);
        src = reinterpret_cast<const uchar*>(v->ptr());
        srclen = v->length();
    }
    const bool one_level = cs->levels_for_order <= 1;
    // A character takes a byte at least: the value's bytes bound its
    // characters, the column's characters every value's
    const uint chars = one_level ? static_cast<uint>(srclen)
                                 : static_cast<uint>(f->char_length());
    const uint nweights = chars * per_char + 1;
    const size_t len = cs->strnxfrmlen(size_t{nweights} * cs->mbmaxlen);
    const uint flags = one_level ? 0 : MY_STRXFRM_PAD_WITH_SPACE;
    if (pad.empty()) {
        // The space's weights (the space in the column's character set),
        // or the empty string's padded key
        uchar space[8];
        const int sn = cs->wc_mb(' ', space, space + sizeof(space));
        const size_t plen = one_level ? cs->strnxfrmlen(cs->mbmaxlen) : len;
        pad.resize(plen);
        pad.resize(cs->strnxfrm(
            reinterpret_cast<uchar*>(pad.data()), plen,
            one_level ? 1 : nweights, space,
            one_level && sn > 0 ? static_cast<size_t>(sn) : 0, flags));
    }
    if (buf.size() < len) buf.resize(len);
    const size_t n = cs->strnxfrm(reinterpret_cast<uchar*>(buf.data()), len,
                                  nweights, src, srclen, flags);
    // A value whose weights reach the end of the key may have lost some
    size_t keep = n;
    if (!one_level) {
        if (n != pad.size()) return false;
        while (keep && buf[keep - 1] == pad[keep - 1])
            --keep;
        if (keep == n) return false;
    } else if (n >= len) {
        return false;
    } else if (!(cs->state & MY_CS_NOPAD) && !pad.empty()) {
        const size_t u = pad.size();
        while (keep >= u &&
               std::memcmp(buf.data() + keep - u, pad.data(), u) == 0)
            keep -= u;
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    out.resize(keep * 2);
    for (size_t i = 0; i < keep; ++i) {
        const auto b = static_cast<uchar>(buf[i]);
        out[2 * i] = kHex[b >> 4];
        out[2 * i + 1] = kHex[b & 15];
    }
    return true;
}

}  // namespace stoolap_mariadb
