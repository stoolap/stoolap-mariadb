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

use super::types::{StoolapDB, StoolapRows};
use super::{STOOLAP_DONE, STOOLAP_ERROR, STOOLAP_OK, STOOLAP_ROW};
use crate::key::{bound_nodes, StoolapKeyBound};
use crate::read::{Cursor, ReadSpec};
use crate::record::{write_record, EncodeFn, RecordIn, Sink, StoolapField, StoreFn};

pub const STOOLAP_READ_DESC: i32 = 1;
pub const STOOLAP_READ_FIXED: i32 = 2;

pub struct StoolapCursor {
    /// The open read, if any
    cursor: Option<Cursor>,
    /// The fields each row fills
    fields: Vec<StoolapField>,
    /// The column count of the query result it holds, if any
    width: Option<usize>,
    /// The values the store callback takes, one at a time
    scratch: Vec<u8>,
    last_error: Option<CString>,
}

impl StoolapCursor {
    fn set_error(&mut self, msg: &str) {
        self.last_error = CString::new(super::error::error_text(msg)).ok();
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
    open: impl FnOnce(&[usize]) -> Result<Option<Cursor>>,
) -> i32 {
    let Some(handle) = cursor.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    handle.cursor = None;
    handle.width = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<bool> {
        handle.fields = field_list(fields, nfields)?;
        let columns: Vec<usize> = handle.fields.iter().map(|f| f.column as usize).collect();
        handle.cursor = open(&columns)?;
        Ok(handle.cursor.is_some())
    }));
    match result {
        Ok(Ok(true)) => STOOLAP_OK,
        Ok(Ok(false)) => STOOLAP_DONE,
        Ok(Err(e)) => {
            handle.set_error(&e.to_string());
            super::error::note_error(&e);
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
        width: None,
        scratch: Vec::new(),
        last_error: None,
    }))
}

/// Opens a read of `table` into `cursor`, in the open transaction or else
/// in a read transaction of the statement's own, which
/// `stoolap_end_statement` ends: the rows within every bound on the key
/// `keys` describes (none: every row), each filling `fields`, ordered by
/// `order` (descending with `STOOLAP_READ_DESC` in `flags`) or in no set
/// order when `norder` is 0. `encode(ctx, ...)` gives the bound values the
/// key's fields do not read themselves. `STOOLAP_DONE` when no row can be
/// within the bounds (no read opens); `stoolap_cursor_next` gives the rows;
/// errors go to `stoolap_cursor_errmsg(cursor)`.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `cursor` a valid `StoolapCursor`,
/// `table` a NUL-terminated UTF-8 string, the arrays must hold their stated
/// lengths, and each bound's record be a record buffer of the layout
/// `keys` describe.
#[no_mangle]
pub unsafe extern "C" fn stoolap_read(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    fields: *const StoolapField,
    nfields: i32,
    keys: *const StoolapField,
    nkeys: i32,
    bounds: *const StoolapKeyBound,
    nbounds: i32,
    order: *const i32,
    norder: i32,
    flags: i32,
    encode: Option<EncodeFn>,
    ctx: *mut c_void,
    cursor: *mut StoolapCursor,
) -> i32 {
    read_into(cursor, fields, nfields, "stoolap_read", |columns| {
        let db = db.as_ref().ok_or_else(|| Error::internal("NULL db"))?;
        let name = table_name(table)?;
        let order = positions(order, norder)?;
        let parts = slice(keys, nkeys)?;
        let mut conds = Vec::new();
        for bound in slice(bounds, nbounds)? {
            let src = RecordIn {
                record: bound.record,
                encode,
                ctx,
            };
            if !bound_nodes(parts, bound, &src, &mut conds)? {
                return Ok(None);
            }
        }
        let spec = ReadSpec {
            table_columns: usize::try_from(table_columns).unwrap_or(usize::MAX),
            columns,
            conds,
            order: &order,
            desc: flags & STOOLAP_READ_DESC != 0,
            fixed: flags & STOOLAP_READ_FIXED != 0,
        };
        Cursor::ordered(&db.session, name, spec).map(Some)
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
            super::error::note_error(&e);
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.set_error("panic during stoolap_end_statement");
            STOOLAP_ERROR
        }
    }
}

/// Takes every row of `rows` into `cursor`, ending the read it had, so
/// that a query's runtime error shows here rather than halfway through
/// its rows; `stoolap_cursor_set_fields` then says what they fill. `rows`
/// is left empty.
///
/// # Safety
///
/// `rows` must be a valid `StoolapRows`, `cursor` a valid `StoolapCursor`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rows_into_cursor(
    rows: *mut StoolapRows,
    cursor: *mut StoolapCursor,
) -> i32 {
    let (Some(rows), Some(handle)) = (rows.as_mut(), cursor.as_mut()) else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    handle.cursor = None;
    handle.width = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<()> {
        let mut taken = Vec::new();
        if let Some(r) = rows.rows.as_mut() {
            while r.advance() {
                taken.push((0, r.take_row()));
            }
            if let Some(e) = r.error() {
                return Err(e);
            }
        }
        rows.rows = None;
        rows.has_row = false;
        handle.width = Some(rows.columns.len());
        handle.cursor = Some(Cursor::from_rows(taken));
        Ok(())
    }));
    match result {
        Ok(Ok(())) => STOOLAP_OK,
        Ok(Err(e)) => {
            handle.set_error(&e.to_string());
            super::error::note_error(&e);
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.set_error("panic during stoolap_rows_into_cursor");
            STOOLAP_ERROR
        }
    }
}

/// Sets the fields the rows `stoolap_rows_into_cursor` took fill, each
/// naming a column of the result; refused when one is past its columns.
///
/// # Safety
///
/// `cursor` must be a valid `StoolapCursor`, `fields` hold `nfields`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_cursor_set_fields(
    cursor: *mut StoolapCursor,
    fields: *const StoolapField,
    nfields: i32,
) -> i32 {
    let Some(handle) = cursor.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    let result = field_list(fields, nfields).and_then(|fields| {
        let width = handle.width.unwrap_or(usize::MAX);
        match fields.iter().find(|f| f.column as usize >= width) {
            Some(f) => Err(Error::internal(format!(
                "the result has {} columns, field {} reads column {}",
                width, f.field, f.column
            ))),
            None => Ok(fields),
        }
    });
    match result {
        Ok(fields) => {
            handle.fields = fields;
            STOOLAP_OK
        }
        Err(e) => {
            handle.cursor = None;
            handle.set_error(&e.to_string());
            super::error::note_error(&e);
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
            .map(Some)
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
            super::error::note_error(&e);
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
