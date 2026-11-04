// Copyright 2025 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Result set iteration FFI functions.

use std::os::raw::c_char;
use std::panic;

use crate::session::Rows;
use stoolap::core::types::DataType;
use stoolap::core::{Result, Value};

use super::types::StoolapRows;
use super::{
    STOOLAP_DONE, STOOLAP_ERROR, STOOLAP_OK, STOOLAP_ROW,
};

/// Advance to the next row.
///
/// Returns `STOOLAP_ROW` if a row is available, `STOOLAP_DONE` when exhausted,
/// or `STOOLAP_ERROR` on error.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_next(rows: *mut StoolapRows) -> i32 {
    let handle = match rows.as_mut() {
        Some(h) => h,
        None => return STOOLAP_ERROR,
    };

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        let rows_inner = match &mut handle.rows {
            Some(r) => r,
            None => return STOOLAP_DONE,
        };

        handle.row_gen += 1;
        if rows_inner.advance() {
            // A TIMESTAMP the C API cannot carry as nanoseconds is an error,
            // not the epoch a column read would give
            let unrepresentable = rows_inner
                .current_row()
                .as_slice()
                .iter()
                .find_map(|value| match value {
                    Value::Timestamp(t) => stoolap::core::value::timestamp_nanos(t).err(),
                    _ => None,
                });
            if let Some(err) = unrepresentable {
                handle.has_row = false;
                handle.set_error(&err.to_string());
                return STOOLAP_ERROR;
            }
            handle.has_row = true;
            STOOLAP_ROW
        } else {
            handle.has_row = false;
            // Check for runtime filter errors (e.g., invalid parameterized REGEXP)
            if let Some(err) = rows_inner.error() {
                handle.set_error(&err.to_string());
                return STOOLAP_ERROR;
            }
            STOOLAP_DONE
        }
    }));

    result.unwrap_or_else(|_| {
        handle.set_error("panic during stoolap_rows_next");
        STOOLAP_ERROR
    })
}

/// Get the current Value at a column index, if the row is valid.
unsafe fn get_current_value(handle: &StoolapRows, index: i32) -> Option<&Value> {
    if !handle.has_row || index < 0 {
        return None;
    }
    let rows_inner = handle.rows.as_ref()?;
    let row = rows_inner.current_row();
    row.get(index as usize)
}

/// Get an integer value from the current row. Returns 0 if NULL or not convertible.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_column_int64(rows: *const StoolapRows, index: i32) -> i64 {
    let handle = match rows.as_ref() {
        Some(h) => h,
        None => return 0,
    };

    match get_current_value(handle, index) {
        Some(val) => val.as_int64().unwrap_or(0),
        None => 0,
    }
}

/// Get a float value from the current row. Returns 0.0 if NULL or not convertible.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_column_double(rows: *const StoolapRows, index: i32) -> f64 {
    let handle = match rows.as_ref() {
        Some(h) => h,
        None => return 0.0,
    };

    match get_current_value(handle, index) {
        Some(val) => val.as_float64().unwrap_or(0.0),
        None => 0.0,
    }
}

/// Get a text value from the current row.
///
/// Returns a pointer valid until the next `stoolap_rows_next()` call.
/// `out_len`: if non-NULL, receives the byte length (excluding null terminator).
/// Returns NULL if the column is NULL. Must NOT be freed.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows` pointer. `out_len` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_column_text(
    rows: *mut StoolapRows,
    index: i32,
    out_len: *mut i64,
) -> *const c_char {
    let handle = match rows.as_mut() {
        Some(h) => h,
        None => return std::ptr::null(),
    };

    if !handle.has_row || index < 0 {
        return std::ptr::null();
    }

    let idx = index as usize;
    let row_gen = handle.row_gen;
    let fresh = match handle.text_cache.get(idx) {
        Some((read_at, _)) => *read_at != row_gen,
        None => true,
    };
    if fresh {
        let Some(rows_inner) = &handle.rows else {
            return std::ptr::null();
        };
        let Some(val) = rows_inner.current_row().get(idx) else {
            return std::ptr::null();
        };
        if handle.text_cache.len() <= idx {
            let len = handle.columns.len().max(idx + 1);
            handle
                .text_cache
                .resize_with(len, || (u64::MAX, Vec::new()));
        }
        let (read_at, buf) = &mut handle.text_cache[idx];
        // Interior NULs are kept: out_len gives the full text, a C string
        // reader stops at the first one
        let mut fill = |s: &str| {
            buf.clear();
            buf.reserve(s.len() + 1);
            buf.extend_from_slice(s.as_bytes());
            buf.push(0);
        };
        if let Some(s) = val.as_str() {
            fill(s);
        } else if let Some(s) = val.as_string() {
            fill(&s);
        } else {
            return std::ptr::null();
        }
        *read_at = row_gen;
    }

    let (_, cached) = &handle.text_cache[idx];
    if !out_len.is_null() {
        // Length excludes the trailing NUL terminator
        *out_len = (cached.len() - 1) as i64;
    }
    cached.as_ptr() as *const c_char
}

/// Check if the current row's column is NULL.
/// Returns 1 if NULL, 0 otherwise.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_column_is_null(rows: *const StoolapRows, index: i32) -> i32 {
    let handle = match rows.as_ref() {
        Some(h) => h,
        None => return 1,
    };

    match get_current_value(handle, index) {
        Some(val) => {
            if val.is_null() {
                1
            } else {
                0
            }
        }
        None => 1,
    }
}

/// Largest packed buffer a handle keeps for its next call
const PACKED_KEEP_MAX: usize = 1 << 20;

/// Empties a handle's packed buffer for reuse, releasing it instead when
/// an earlier large result left it above `PACKED_KEEP_MAX`
pub(crate) fn reset_packed(buf: &mut Vec<u8>) {
    if buf.capacity() > PACKED_KEEP_MAX {
        *buf = Vec::new();
    } else {
        buf.clear();
    }
}

/// Writes the column names and a zero row count, returning where the
/// row count sits so `write_rows` can patch it
pub(crate) fn write_header<'a>(names: impl ExactSizeIterator<Item = &'a [u8]>, buf: &mut Vec<u8>) -> usize {
    buf.extend_from_slice(&(names.len() as u32).to_le_bytes());
    for name in names {
        buf.extend_from_slice(&(name.len() as u16).to_le_bytes());
        buf.extend_from_slice(name);
    }
    let count_at = buf.len();
    buf.extend_from_slice(&0u32.to_le_bytes());
    count_at
}

/// Packs one cell: a type tag and its payload (see `stoolap_rows_fetch_all`)
pub(crate) fn write_value(val: Option<&Value>, buf: &mut Vec<u8>) -> Result<()> {
    match val {
        None => {
            buf.push(0); // TYPE_NULL
        }
        Some(val) if val.is_null() => {
            buf.push(0); // TYPE_NULL
        }
        Some(val) => match val.data_type() {
            DataType::Null => {
                buf.push(0);
            }
            DataType::Integer => {
                buf.push(1);
                let v = val.as_int64().unwrap_or(0);
                buf.extend_from_slice(&v.to_le_bytes());
            }
            DataType::Float => {
                buf.push(2);
                let v = val.as_float64().unwrap_or(0.0);
                buf.extend_from_slice(&v.to_le_bytes());
            }
            DataType::Text => {
                buf.push(3);
                let s = val.as_str().unwrap_or("");
                buf.extend_from_slice(&(s.len() as u32).to_le_bytes());
                buf.extend_from_slice(s.as_bytes());
            }
            DataType::Boolean => {
                buf.push(4);
                buf.push(if val.as_boolean().unwrap_or(false) {
                    1
                } else {
                    0
                });
            }
            DataType::Timestamp => {
                buf.push(5);
                let nanos = match val.as_timestamp() {
                    Some(ts) => stoolap::core::value::timestamp_nanos(&ts)?,
                    None => 0,
                };
                buf.extend_from_slice(&nanos.to_le_bytes());
            }
            DataType::Json => {
                buf.push(6);
                let s = val.as_str().unwrap_or("");
                buf.extend_from_slice(&(s.len() as u32).to_le_bytes());
                buf.extend_from_slice(s.as_bytes());
            }
            DataType::Vector => {
                buf.push(7);
                if let Value::Extension(data) = val {
                    if data.first() == Some(&(DataType::Vector as u8)) {
                        let payload = &data[1..];
                        buf.extend_from_slice(&(payload.len() as u32).to_le_bytes());
                        buf.extend_from_slice(payload);
                    } else {
                        buf.extend_from_slice(&0u32.to_le_bytes());
                    }
                } else {
                    buf.extend_from_slice(&0u32.to_le_bytes());
                }
            }
        },
    }
    Ok(())
}

/// Packs up to `max_rows` rows and patches the count at `count_at`. A
/// runtime error that ends the result early is returned, not packed.
fn write_rows(
    rows: &mut Rows,
    col_count: usize,
    max_rows: usize,
    count_at: usize,
    buf: &mut Vec<u8>,
) -> Result<u32> {
    let mut row_count: u32 = 0;
    while (row_count as usize) < max_rows && rows.advance() {
        let row = rows.current_row();
        for i in 0..col_count {
            write_value(row.get(i), buf)?;
        }
        row_count += 1;
    }
    if (row_count as usize) < max_rows {
        if let Some(err) = rows.error() {
            return Err(err);
        }
    }
    buf[count_at..count_at + 4].copy_from_slice(&row_count.to_le_bytes());
    Ok(row_count)
}

/// Packs every row of `rows` into `buf`, which is reset first
pub(crate) fn pack_all(rows: &mut Rows, buf: &mut Vec<u8>) -> Result<u32> {
    reset_packed(buf);
    let count_at = write_header(rows.columns().iter().map(|n| n.as_bytes()), buf);
    let col_count = rows.columns().len();
    write_rows(rows, col_count, usize::MAX, count_at, buf)
}

/// Fetch all remaining rows into a packed binary buffer.
///
/// Binary format:
/// ```text
/// [column_count: u32 LE]
/// [for each column: name_len:u16 LE, name_bytes:u8[name_len]]
/// [row_count: u32 LE]
/// [for each row, for each column:
///   type_tag: u8
///   payload (varies by type):
///     NULL(0): empty
///     INTEGER(1): i64 LE (8 bytes)
///     FLOAT(2): f64 LE (8 bytes)
///     TEXT(3): len:u32 LE + bytes:u8[len]
///     BOOLEAN(4): u8 (0 or 1)
///     TIMESTAMP(5): i64 LE (8 bytes, nanos since epoch)
///     JSON(6): len:u32 LE + bytes:u8[len]
///     BLOB(7): len:u32 LE + bytes:u8[len]
/// ]
/// ```
///
/// On success, `*out_buf` receives a heap-allocated buffer and `*out_len`
/// its byte length. The caller MUST free it with `stoolap_buffer_free()`.
/// The rows handle is consumed and closed by this call. A runtime error
/// that ends the result early returns `STOOLAP_ERROR` and no buffer.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows` pointer. `out_buf` and `out_len` must not be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_fetch_all(
    rows: *mut StoolapRows,
    out_buf: *mut *mut u8,
    out_len: *mut i64,
) -> i32 {
    if out_buf.is_null() || out_len.is_null() {
        return STOOLAP_ERROR;
    }
    *out_buf = std::ptr::null_mut();
    *out_len = 0;

    let handle = match rows.as_mut() {
        Some(h) => h,
        None => return STOOLAP_ERROR,
    };

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        let mut buf = Vec::with_capacity(4096);
        let count_at = write_header(handle.columns.iter().map(|n| n.as_bytes()), &mut buf);
        let col_count = handle.columns.len();
        let packed = match &mut handle.rows {
            Some(r) => write_rows(r, col_count, usize::MAX, count_at, &mut buf),
            None => Ok(0),
        };
        if let Err(e) = packed {
            handle.set_error(&e.to_string());
            return STOOLAP_ERROR;
        }

        // Transfer ownership to caller.
        // into_boxed_slice guarantees len == capacity (unlike shrink_to_fit
        // which is only a hint). This makes stoolap_buffer_free safe.
        let boxed = buf.into_boxed_slice();
        let len = boxed.len();
        let ptr = Box::into_raw(boxed) as *mut u8;

        *out_buf = ptr;
        *out_len = len as i64;
        STOOLAP_OK
    }));

    // Close rows handle after fetching
    handle.rows = None;
    handle.has_row = false;

    result.unwrap_or_else(|_| {
        handle.set_error("panic during stoolap_rows_fetch_all");
        STOOLAP_ERROR
    })
}

/// Free a buffer allocated by `stoolap_rows_fetch_all()`.
///
/// # Safety
///
/// `buf` must be a pointer returned by `stoolap_rows_fetch_all`, or NULL.
/// `len` must be the exact length returned alongside the buffer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_buffer_free(buf: *mut u8, len: i64) {
    if !buf.is_null() && len > 0 {
        // The buffer was created via Box::into_raw(Vec::into_boxed_slice()),
        // so len == capacity is guaranteed. Reconstruct and drop.
        let _ = Vec::from_raw_parts(buf, len as usize, len as usize);
    }
}

/// Close the result set and free resources.
///
/// Safe to call with NULL (no-op).
/// Must be called even if iteration completes (`STOOLAP_DONE`).
///
/// # Safety
///
/// `rows` must be a pointer returned by a stoolap query function, or NULL.
/// After this call, the pointer is invalid.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_close(rows: *mut StoolapRows) {
    if !rows.is_null() {
        let _ = Box::from_raw(rows);
    }
}

/// Get the last error message for a rows handle.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_errmsg(rows: *const StoolapRows) -> *const c_char {
    match rows.as_ref() {
        Some(handle) => handle.error_ptr(),
        None => super::error::empty_cstr(),
    }
}
