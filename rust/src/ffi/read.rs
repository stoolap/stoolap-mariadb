// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Direct read FFI: a cursor over a table of the open transaction that
//! writes each row straight into the caller's record buffer (see
//! `crate::record`). The caller keeps one cursor per handler and reads into
//! it again and again.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::panic;

use stoolap::core::{Error, Result};

use super::types::{StoolapDB, StoolapValue};
use super::{value, STOOLAP_DONE, STOOLAP_ERROR, STOOLAP_OK, STOOLAP_ROW};
use crate::read::{Cond, CondOp, Cursor, Node, ReadSpec};
use crate::record::{write_record, Sink, StoolapField, StoreFn};

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
    /// The fields each row fills
    fields: Vec<StoolapField>,
    /// The values the store callback takes, one at a time
    scratch: Vec<u8>,
    last_error: Option<CString>,
}

impl StoolapCursor {
    fn set_error(&mut self, msg: &str) {
        self.last_error = CString::new(msg.replace('\0', "\\0")).ok();
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

/// The columns `fields` read
unsafe fn field_list(fields: *const StoolapField, n: i32) -> Result<Vec<StoolapField>> {
    let fields = slice(fields, n)?.to_vec();
    if fields.iter().any(|f| f.column < 0) {
        return Err(Error::internal("negative column"));
    }
    Ok(fields)
}

/// Puts the read `open` makes into `cursor`, reading `fields`
unsafe fn read_into(
    cursor: *mut StoolapCursor,
    fields: *const StoolapField,
    nfields: i32,
    what: &str,
    open: impl FnOnce(&[usize]) -> Result<Cursor>,
) -> i32 {
    let Some(handle) = cursor.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    handle.cursor = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<()> {
        handle.fields = field_list(fields, nfields)?;
        let columns: Vec<usize> = handle.fields.iter().map(|f| f.column as usize).collect();
        handle.cursor = Some(open(&columns)?);
        Ok(())
    }));
    match result {
        Ok(Ok(())) => STOOLAP_OK,
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

/// A cursor with no read open, for `stoolap_read` / `stoolap_read_row`
#[no_mangle]
pub extern "C" fn stoolap_cursor_new() -> *mut StoolapCursor {
    Box::into_raw(Box::new(StoolapCursor {
        cursor: None,
        fields: Vec::new(),
        scratch: Vec::new(),
        last_error: None,
    }))
}

/// Opens a read of `table` into `cursor`, in the open transaction or else
/// in a read transaction of the statement's own, which
/// `stoolap_end_statement` ends: the rows meeting every condition, each
/// filling `fields`, ordered by `order` (descending with
/// `STOOLAP_READ_DESC` in `flags`) or in no set order when `norder` is 0.
/// `stoolap_cursor_next` gives the rows; errors go to
/// `stoolap_cursor_errmsg(cursor)`.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `cursor` a valid `StoolapCursor`,
/// `table` a NUL-terminated UTF-8 string, and the arrays must hold their
/// stated lengths.
#[no_mangle]
pub unsafe extern "C" fn stoolap_read(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    fields: *const StoolapField,
    nfields: i32,
    conds: *const StoolapCond,
    nconds: i32,
    order: *const i32,
    norder: i32,
    flags: i32,
    cursor: *mut StoolapCursor,
) -> i32 {
    read_into(cursor, fields, nfields, "stoolap_read", |columns| {
        let db = db.as_ref().ok_or_else(|| Error::internal("NULL db"))?;
        let name = table_name(table)?;
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
            columns,
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

/// Opens a read of row `row_id` of `table` into `cursor`, filling
/// `fields`: no row when it is not visible. As `stoolap_read` otherwise.
///
/// # Safety
///
/// As `stoolap_read`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_read_row(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    fields: *const StoolapField,
    nfields: i32,
    row_id: i64,
    cursor: *mut StoolapCursor,
) -> i32 {
    read_into(cursor, fields, nfields, "stoolap_read_row", |columns| {
        let db = db.as_ref().ok_or_else(|| Error::internal("NULL db"))?;
        let name = table_name(table)?;
        let table_columns = usize::try_from(table_columns).unwrap_or(usize::MAX);
        db.session
            .with_table(name, |t| Cursor::by_id(t, table_columns, columns, row_id))
    })
}

/// Writes the read's next row into `record`: its fields as the read
/// describes them, the rest through `store(ctx, ...)`. Gives the row's id
/// in `row_id`, and 1 in `last` when no row follows it (the read has
/// ended then). Returns `STOOLAP_ROW`, `STOOLAP_DONE` at the end (the read
/// ends), or `STOOLAP_ERROR` (the read ends; see `stoolap_cursor_errmsg`).
///
/// # Safety
///
/// `cursor` must be a valid `StoolapCursor`, `record` a record buffer of
/// the layout its fields describe; `row_id` and `last` must not be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_cursor_next(
    cursor: *mut StoolapCursor,
    record: *mut u8,
    store: Option<StoreFn>,
    ctx: *mut c_void,
    row_id: *mut i64,
    last: *mut i32,
) -> i32 {
    let Some(handle) = cursor.as_mut() else {
        return STOOLAP_ERROR;
    };
    if record.is_null() || row_id.is_null() || last.is_null() {
        return STOOLAP_ERROR;
    }
    handle.last_error = None;
    *last = 0;
    let sink = Sink { record, store, ctx };
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<bool> {
        let StoolapCursor {
            cursor,
            fields,
            scratch,
            ..
        } = handle;
        let Some(read) = cursor.as_mut() else {
            return Ok(false);
        };
        let Some((id, row)) = read.next_row()? else {
            return Ok(false);
        };
        write_record(fields, row, &sink, scratch)?;
        *row_id = id;
        Ok(true)
    }));
    match result {
        Ok(Ok(true)) => {
            if handle.cursor.as_ref().is_some_and(Cursor::exhausted) {
                handle.cursor = None;
                *last = 1;
            }
            STOOLAP_ROW
        }
        Ok(Ok(false)) => {
            handle.cursor = None;
            STOOLAP_DONE
        }
        Ok(Err(e)) => {
            handle.cursor = None;
            handle.set_error(&e.to_string());
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.cursor = None;
            handle.set_error("panic during stoolap_cursor_next");
            STOOLAP_ERROR
        }
    }
}

/// Ends the cursor's read, if one is open, keeping the cursor for the
/// next
///
/// # Safety
///
/// `cursor` must be a valid `StoolapCursor` pointer or NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_cursor_end(cursor: *mut StoolapCursor) {
    if let Some(c) = cursor.as_mut() {
        c.cursor = None;
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
