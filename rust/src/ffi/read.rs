// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Direct read FFI: a cursor over a table of the open transaction, its
//! rows handed out in packed chunks.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic;

use stoolap::core::{Error, Result};

use super::rows::{reset_packed, write_header, write_value};
use super::types::{StoolapDB, StoolapValue};
use super::{value, STOOLAP_DONE, STOOLAP_ERROR, STOOLAP_OK, STOOLAP_ROW};
use crate::read::{Cond, CondOp, Cursor, Node, ReadSpec};

pub const STOOLAP_READ_DESC: i32 = 1;
pub const STOOLAP_READ_FIXED: i32 = 2;
pub const STOOLAP_OP_EQ: i32 = 0;
pub const STOOLAP_OP_GT: i32 = 1;
pub const STOOLAP_OP_GE: i32 = 2;
pub const STOOLAP_OP_LT: i32 = 3;
pub const STOOLAP_OP_LE: i32 = 4;
pub const STOOLAP_OP_IS_NULL: i32 = 5;
pub const STOOLAP_OP_IS_NOT_NULL: i32 = 6;
/// Joins the `column` (a count) conditions before it
pub const STOOLAP_OP_AND: i32 = 7;
pub const STOOLAP_OP_OR: i32 = 8;

/// `column op value`, the column by position
#[repr(C)]
pub struct StoolapCond {
    pub column: i32,
    pub op: i32,
    pub value: StoolapValue,
}

pub struct StoolapCursor {
    cursor: Cursor,
    packed: Vec<u8>,
    last_error: Option<CString>,
}

unsafe fn slice<'a, T>(ptr: *const T, len: i32) -> Result<&'a [T]> {
    if len <= 0 {
        return Ok(&[]);
    }
    if ptr.is_null() {
        return Err(Error::internal("NULL array with a non-zero length"));
    }
    Ok(std::slice::from_raw_parts(ptr, len as usize))
}

unsafe fn positions(ptr: *const i32, len: i32) -> Result<Vec<usize>> {
    slice(ptr, len)?
        .iter()
        .map(|&c| usize::try_from(c).map_err(|_| Error::internal("negative column")))
        .collect()
}

pub(crate) unsafe fn table_name<'a>(table: *const c_char) -> Result<&'a str> {
    if table.is_null() {
        return Err(Error::internal("table name is NULL"));
    }
    CStr::from_ptr(table)
        .to_str()
        .map_err(|e| Error::internal(format!("invalid UTF-8 in table name: {}", e)))
}

/// Hands out the cursor `result` opened, or records its error on `handle`
unsafe fn finish_open(
    handle: &mut StoolapDB,
    result: std::thread::Result<Result<Cursor>>,
    out: *mut *mut StoolapCursor,
    what: &str,
) -> i32 {
    match result {
        Ok(Ok(cursor)) => {
            *out = Box::into_raw(Box::new(StoolapCursor {
                cursor,
                packed: Vec::new(),
                last_error: None,
            }));
            STOOLAP_OK
        }
        Ok(Err(e)) => {
            handle.set_error(&e.to_string());
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.set_error(&format!("panic during {}", what));
            STOOLAP_ERROR
        }
    }
}

/// Opens a read of `table`, in the open transaction or else in a read
/// transaction of the statement's own, which `stoolap_end_statement` ends:
/// the rows meeting every
/// condition, each carrying `columns`, ordered by `order` (descending with
/// `STOOLAP_READ_DESC` in `flags`) or in no set order when `norder` is 0.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `table` a NUL-terminated UTF-8 string,
/// and the arrays must hold their stated lengths. `out` must not be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_read(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    columns: *const i32,
    ncolumns: i32,
    conds: *const StoolapCond,
    nconds: i32,
    order: *const i32,
    norder: i32,
    flags: i32,
    out: *mut *mut StoolapCursor,
) -> i32 {
    if out.is_null() {
        return STOOLAP_ERROR;
    }
    *out = std::ptr::null_mut();
    let Some(handle) = db.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<Cursor> {
        let name = table_name(table)?;
        let columns = positions(columns, ncolumns)?;
        let order = positions(order, norder)?;
        let mut cond_list = Vec::with_capacity(nconds.max(0) as usize);
        for c in slice(conds, nconds)? {
            let column =
                usize::try_from(c.column).map_err(|_| Error::internal("negative column"))?;
            let op = match c.op {
                STOOLAP_OP_AND => {
                    cond_list.push(Node::And(column));
                    continue;
                }
                STOOLAP_OP_OR => {
                    cond_list.push(Node::Or(column));
                    continue;
                }
                STOOLAP_OP_EQ => CondOp::Eq,
                STOOLAP_OP_GT => CondOp::Gt,
                STOOLAP_OP_GE => CondOp::Ge,
                STOOLAP_OP_LT => CondOp::Lt,
                STOOLAP_OP_LE => CondOp::Le,
                STOOLAP_OP_IS_NULL => CondOp::IsNull,
                STOOLAP_OP_IS_NOT_NULL => CondOp::IsNotNull,
                _ => return Err(Error::internal("invalid condition operator")),
            };
            let value = match op {
                CondOp::IsNull | CondOp::IsNotNull => stoolap::core::Value::null_unknown(),
                _ => value::ffi_value_to_rust(&c.value)?,
            };
            cond_list.push(Node::Cond(Cond { column, op, value }));
        }
        let spec = ReadSpec {
            table_columns: usize::try_from(table_columns).unwrap_or(usize::MAX),
            columns: &columns,
            conds: cond_list,
            order: &order,
            desc: flags & STOOLAP_READ_DESC != 0,
            fixed: flags & STOOLAP_READ_FIXED != 0,
        };
        Cursor::ordered(&handle.session, name, spec)
    }));
    finish_open(handle, result, out, "stoolap_read")
}

/// Ends the statement's direct access, and the transaction it opened
/// outside an open one: committed when `commit`, else rolled back. Called
/// at the end of each MariaDB statement, with its cursors closed. A failed
/// commit rolls back and reports to `stoolap_errmsg(db)`.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB` pointer or NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_end_statement(db: *mut StoolapDB, commit: i32) -> i32 {
    let Some(handle) = db.as_mut() else {
        return STOOLAP_OK;
    };
    handle.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        handle.session.end_statement(commit != 0)
    }));
    match result {
        Ok(Ok(())) => STOOLAP_OK,
        Ok(Err(e)) => {
            handle.set_error(&e.to_string());
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.set_error("panic during stoolap_end_statement");
            STOOLAP_ERROR
        }
    }
}

/// Opens a read of row `row_id` of `table`, carrying `columns`: no row
/// when it is not visible. As `stoolap_read` otherwise.
///
/// # Safety
///
/// As `stoolap_read`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_read_row(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    columns: *const i32,
    ncolumns: i32,
    row_id: i64,
    out: *mut *mut StoolapCursor,
) -> i32 {
    if out.is_null() {
        return STOOLAP_ERROR;
    }
    *out = std::ptr::null_mut();
    let Some(handle) = db.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<Cursor> {
        let name = table_name(table)?;
        let columns = positions(columns, ncolumns)?;
        let table_columns = usize::try_from(table_columns).unwrap_or(usize::MAX);
        handle
            .session
            .with_table(name, |t| Cursor::by_id(t, table_columns, &columns, row_id))
    }));
    finish_open(handle, result, out, "stoolap_read_row")
}

/// Packs the cursor's next rows, at most `max_rows`, in the
/// `stoolap_rows_fetch_all` format (column names empty), each row's id
/// as a last INTEGER cell. The buffer stays
/// the cursor's, valid until the next fetch or close. Returns
/// `STOOLAP_ROW` with at least one row, `STOOLAP_DONE` at the end.
///
/// # Safety
///
/// `cursor` must be a valid `StoolapCursor`; `out_buf` and `out_len` must
/// not be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_cursor_fetch(
    cursor: *mut StoolapCursor,
    max_rows: i32,
    out_buf: *mut *const u8,
    out_len: *mut i64,
) -> i32 {
    if out_buf.is_null() || out_len.is_null() {
        return STOOLAP_ERROR;
    }
    *out_buf = std::ptr::null();
    *out_len = 0;
    let Some(handle) = cursor.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<usize> {
        let StoolapCursor { cursor, packed, .. } = handle;
        reset_packed(packed);
        let n = cursor.column_count();
        let count_at = write_header(std::iter::repeat(&b""[..]).take(n + 1), packed);
        let max = usize::try_from(max_rows).unwrap_or(0).max(1);
        let rows = cursor.next_rows(max, |id, row, columns| {
            for &c in columns {
                write_value(row.get(c), packed)?;
            }
            packed.push(1); // INTEGER
            packed.extend_from_slice(&id.to_le_bytes());
            Ok(())
        })?;
        packed[count_at..count_at + 4].copy_from_slice(&(rows as u32).to_le_bytes());
        Ok(rows)
    }));

    match result {
        Ok(Ok(0)) => STOOLAP_DONE,
        Ok(Ok(_)) => {
            *out_buf = handle.packed.as_ptr();
            *out_len = handle.packed.len() as i64;
            STOOLAP_ROW
        }
        Ok(Err(e)) => {
            handle.last_error = CString::new(e.to_string().replace('\0', "\\0")).ok();
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.last_error = CString::new("panic during stoolap_cursor_fetch").ok();
            STOOLAP_ERROR
        }
    }
}

/// # Safety
///
/// `cursor` must be a valid `StoolapCursor` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_cursor_errmsg(cursor: *const StoolapCursor) -> *const c_char {
    match cursor.as_ref().and_then(|c| c.last_error.as_ref()) {
        Some(e) => e.as_ptr(),
        None => super::error::empty_cstr(),
    }
}

/// # Safety
///
/// `cursor` must be a valid `StoolapCursor` pointer or NULL; it is freed.
#[no_mangle]
pub unsafe extern "C" fn stoolap_cursor_close(cursor: *mut StoolapCursor) {
    if !cursor.is_null() {
        drop(Box::from_raw(cursor));
    }
}
