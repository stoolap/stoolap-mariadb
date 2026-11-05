// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Direct read FFI: a cursor over a table of the open transaction, its
//! rows handed out in packed chunks. The caller keeps one cursor per
//! handler and reads into it again and again: a key lookup is one call,
//! with no allocation once the buffer has grown.

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
    /// The open read, if any
    cursor: Option<Cursor>,
    packed: Vec<u8>,
    last_error: Option<CString>,
}

impl StoolapCursor {
    fn set_error(&mut self, msg: &str) {
        self.last_error = CString::new(msg.replace('\0', "\\0")).ok();
    }

    /// Packs up to `max_rows` rows of the open read; ends the read when
    /// they are its last
    fn fetch(&mut self, max_rows: i32) -> Result<usize> {
        let StoolapCursor { cursor, packed, .. } = self;
        reset_packed(packed);
        let Some(read) = cursor.as_mut() else {
            return Ok(0);
        };
        let n = read.column_count();
        let count_at = write_header(std::iter::repeat(&b""[..]).take(n + 1), packed);
        let max = usize::try_from(max_rows).unwrap_or(0).max(1);
        let rows = read.next_rows(max, |id, row, columns| {
            for &c in columns {
                write_value(row.get(c), packed)?;
            }
            packed.push(1); // INTEGER
            packed.extend_from_slice(&id.to_le_bytes());
            Ok(())
        })?;
        packed[count_at..count_at + 4].copy_from_slice(&(rows as u32).to_le_bytes());
        if rows < max {
            // Short only at the end: let go of the table now
            *cursor = None;
        }
        Ok(rows)
    }
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

/// Puts the read `open` makes into `cursor` and packs its first
/// `first_rows` rows, as `stoolap_cursor_fetch`
unsafe fn read_into(
    cursor: *mut StoolapCursor,
    first_rows: i32,
    out_buf: *mut *const u8,
    out_len: *mut i64,
    what: &str,
    open: impl FnOnce() -> Result<Cursor>,
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
    handle.cursor = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<usize> {
        handle.cursor = Some(open()?);
        handle.fetch(first_rows)
    }));
    finish_fetch(handle, result, out_buf, out_len, what)
}

unsafe fn finish_fetch(
    handle: &mut StoolapCursor,
    result: std::thread::Result<Result<usize>>,
    out_buf: *mut *const u8,
    out_len: *mut i64,
    what: &str,
) -> i32 {
    match result {
        Ok(Ok(0)) => STOOLAP_DONE,
        Ok(Ok(_)) => {
            *out_buf = handle.packed.as_ptr();
            *out_len = handle.packed.len() as i64;
            STOOLAP_ROW
        }
        Ok(Err(e)) => {
            handle.cursor = None;
            handle.set_error(&e.to_string());
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.cursor = None;
            handle.set_error(&format!("panic during {}", what));
            STOOLAP_ERROR
        }
    }
}

/// A cursor with no read open, for `stoolap_read` / `stoolap_read_row`
#[no_mangle]
pub extern "C" fn stoolap_cursor_new() -> *mut StoolapCursor {
    Box::into_raw(Box::new(StoolapCursor {
        cursor: None,
        packed: Vec::new(),
        last_error: None,
    }))
}

/// Opens a read of `table` into `cursor`, in the open transaction or else
/// in a read transaction of the statement's own, which
/// `stoolap_end_statement` ends: the rows meeting every condition, each
/// carrying `columns`, ordered by `order` (descending with
/// `STOOLAP_READ_DESC` in `flags`) or in no set order when `norder` is 0.
/// Packs its first `first_rows` rows as `stoolap_cursor_fetch`; errors go
/// to `stoolap_cursor_errmsg(cursor)`.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `cursor` a valid `StoolapCursor`,
/// `table` a NUL-terminated UTF-8 string, and the arrays must hold their
/// stated lengths. `out_buf` and `out_len` must not be NULL.
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
    cursor: *mut StoolapCursor,
    first_rows: i32,
    out_buf: *mut *const u8,
    out_len: *mut i64,
) -> i32 {
    read_into(cursor, first_rows, out_buf, out_len, "stoolap_read", || {
        let db = db.as_ref().ok_or_else(|| Error::internal("NULL db"))?;
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
        Cursor::ordered(&db.session, name, spec)
    })
}

/// Ends the statement's direct access, and the transaction it opened
/// outside an open one: committed when `commit`, else rolled back. Called
/// at the end of each MariaDB statement, with its cursors' reads ended. A failed
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

/// Opens a read of row `row_id` of `table` into `cursor`, carrying
/// `columns`: no row when it is not visible. As `stoolap_read` otherwise.
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
    cursor: *mut StoolapCursor,
    out_buf: *mut *const u8,
    out_len: *mut i64,
) -> i32 {
    read_into(cursor, 1, out_buf, out_len, "stoolap_read_row", || {
        let db = db.as_ref().ok_or_else(|| Error::internal("NULL db"))?;
        let name = table_name(table)?;
        let columns = positions(columns, ncolumns)?;
        let table_columns = usize::try_from(table_columns).unwrap_or(usize::MAX);
        db.session
            .with_table(name, |t| Cursor::by_id(t, table_columns, &columns, row_id))
    })
}

/// Packs the cursor's next rows, at most `max_rows`, in the
/// `stoolap_rows_fetch_all` format (column names empty), each row's id
/// as a last INTEGER cell. The buffer stays the cursor's, valid until its
/// next read, fetch or close. Returns `STOOLAP_ROW` with at least one row,
/// `STOOLAP_DONE` at the end; a short chunk is the last, and ends the read.
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
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| handle.fetch(max_rows)));
    finish_fetch(handle, result, out_buf, out_len, "stoolap_cursor_fetch")
}

/// Ends the cursor's read, if one is open, keeping the cursor for the
/// next; the last chunk is gone, and its buffer too when it grew large
///
/// # Safety
///
/// `cursor` must be a valid `StoolapCursor` pointer or NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_cursor_end(cursor: *mut StoolapCursor) {
    if let Some(c) = cursor.as_mut() {
        c.cursor = None;
        reset_packed(&mut c.packed);
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
