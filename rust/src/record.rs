// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! A row written straight into a MariaDB record buffer. The plugin
//! describes each field it reads once (where it sits in the record, its
//! NULL bit, how it is stored); the common kinds are written here and the
//! rest are handed back, one value at a time, to the plugin's own store.

use std::os::raw::c_void;

use stoolap::core::{Error, Result, Row, Value};

use crate::ffi::rows::write_value;

/// Handed to the plugin's store callback
pub const STOOLAP_FIELD_STORE: i32 = 0;
/// A little-endian integer of `length` bytes (1, 2, 3, 4 or 8)
pub const STOOLAP_FIELD_INT: i32 = 1;
/// An 8-byte DOUBLE, with no fixed decimals
pub const STOOLAP_FIELD_DOUBLE: i32 = 2;
/// A 4-byte FLOAT, with no fixed decimals
pub const STOOLAP_FIELD_FLOAT: i32 = 3;
/// A utf8mb4 VARCHAR: a `length`-byte (1 or 2) length prefix, then the
/// bytes; a value of at most `max_chars` bytes always fits
pub const STOOLAP_FIELD_VARCHAR: i32 = 4;

/// One field a read fills
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StoolapField {
    /// The stoolap column it reads
    pub column: i32,
    /// `STOOLAP_FIELD_*`
    pub kind: i32,
    /// Where it sits in the record
    pub offset: u32,
    /// Its NULL bit: the byte's offset and the mask, 0 when NOT NULL
    pub null_offset: u32,
    pub null_mask: u32,
    pub length: u32,
    pub max_chars: u32,
    pub is_unsigned: u32,
    /// The field's index, for the store callback
    pub field: i32,
}

/// Stores the cell `cell` (`stoolap_rows_fetch_all` format, one value) in
/// field `field`; 0 on success
pub type StoreFn =
    unsafe extern "C" fn(ctx: *mut c_void, field: i32, cell: *const u8, len: usize) -> i32;

/// Where a read writes its rows
pub struct Sink {
    pub record: *mut u8,
    pub store: Option<StoreFn>,
    pub ctx: *mut c_void,
}

/// Whether `v` fits a `bytes`-byte integer field
fn int_fits(v: i64, bytes: u32, unsigned: bool) -> bool {
    match (bytes, unsigned) {
        (8, _) => true, // BIGINT UNSIGNED keeps its bit pattern in an i64
        (1..=4, false) => {
            let bits = bytes * 8 - 1;
            (-(1i64 << bits)..(1i64 << bits)).contains(&v)
        }
        (1..=4, true) => (0..(1i64 << (bytes * 8))).contains(&v),
        _ => false,
    }
}

/// Writes `v` into `f` at `at` when its kind takes it as it is
///
/// # Safety
///
/// `at` must point at the field in a record of the described layout.
unsafe fn write_direct(f: &StoolapField, v: &Value, at: *mut u8) -> bool {
    match (f.kind, v) {
        (STOOLAP_FIELD_INT, Value::Integer(_) | Value::Boolean(_)) => {
            let i = match v {
                Value::Integer(i) => *i,
                _ => matches!(v, Value::Boolean(true)) as i64,
            };
            if !int_fits(i, f.length, f.is_unsigned != 0) {
                return false;
            }
            std::ptr::copy_nonoverlapping(i.to_le_bytes().as_ptr(), at, f.length as usize);
            true
        }
        (STOOLAP_FIELD_DOUBLE, Value::Float(_) | Value::Integer(_)) => {
            let d = match v {
                Value::Float(d) => *d,
                Value::Integer(i) => *i as f64,
                _ => unreachable!(),
            };
            if !d.is_finite() {
                return false;
            }
            std::ptr::copy_nonoverlapping(d.to_le_bytes().as_ptr(), at, 8);
            true
        }
        (STOOLAP_FIELD_FLOAT, Value::Float(d)) => {
            if !d.is_finite() || d.abs() > f32::MAX as f64 {
                return false;
            }
            std::ptr::copy_nonoverlapping((*d as f32).to_le_bytes().as_ptr(), at, 4);
            true
        }
        (STOOLAP_FIELD_VARCHAR, Value::Text(s)) => {
            let bytes = s.as_bytes();
            if bytes.len() > f.max_chars as usize {
                return false;
            }
            let n = bytes.len() as u16;
            let prefix = f.length as usize;
            std::ptr::copy_nonoverlapping(n.to_le_bytes().as_ptr(), at, prefix);
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), at.add(prefix), bytes.len());
            true
        }
        _ => false,
    }
}

/// Writes `row`'s `fields` into `sink`'s record; `scratch` packs the
/// values the store callback takes
///
/// # Safety
///
/// `sink.record` must be a record buffer of the layout `fields` describe.
pub unsafe fn write_record(
    fields: &[StoolapField],
    row: &Row,
    sink: &Sink,
    scratch: &mut Vec<u8>,
) -> Result<()> {
    for f in fields {
        let v = row.get(f.column as usize).filter(|v| !v.is_null());
        if f.null_mask != 0 {
            let nb = sink.record.add(f.null_offset as usize);
            match v {
                None => {
                    *nb |= f.null_mask as u8;
                    continue;
                }
                Some(_) => *nb &= !(f.null_mask as u8),
            }
        }
        if let Some(v) = v {
            if write_direct(f, v, sink.record.add(f.offset as usize)) {
                continue;
            }
        }
        // Everything else (and NULL in a NOT NULL field) as the plugin
        // stores it
        let store = sink
            .store
            .ok_or_else(|| Error::internal("a field needs a store callback"))?;
        scratch.clear();
        write_value(v, scratch)?;
        let rc = store(sink.ctx, f.field, scratch.as_ptr(), scratch.len());
        if rc != 0 {
            return Err(Error::internal(format!(
                "storing field {} failed ({})",
                f.field, rc
            )));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::int_fits;

    #[test]
    fn int_ranges() {
        assert!(int_fits(127, 1, false) && !int_fits(128, 1, false));
        assert!(int_fits(-128, 1, false) && !int_fits(-129, 1, false));
        assert!(int_fits(255, 1, true) && !int_fits(256, 1, true) && !int_fits(-1, 1, true));
        assert!(int_fits(8388607, 3, false) && !int_fits(8388608, 3, false));
        assert!(int_fits(16777215, 3, true) && !int_fits(16777216, 3, true));
        assert!(int_fits(i32::MIN as i64, 4, false) && !int_fits(i32::MAX as i64 + 1, 4, false));
        assert!(int_fits(u32::MAX as i64, 4, true) && !int_fits(u32::MAX as i64 + 1, 4, true));
        assert!(int_fits(i64::MIN, 8, false) && int_fits(-1, 8, true));
        assert!(!int_fits(0, 5, false));
    }
}
