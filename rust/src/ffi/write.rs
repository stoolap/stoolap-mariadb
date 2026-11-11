// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Write FFI: rows read straight from the plugin's record buffers. Each
//! call writes in the open transaction, or else in the statement's own (see
//! `stoolap_end_statement`). A table a foreign key names is written through
//! SQL instead (`crate::sql_write`), where stoolap's executor checks the
//! keys, as is a row named by its primary key rather than its row id.

use std::cell::Cell;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::panic;

use stoolap::core::{Error, Result, Value};
use stoolap::storage::traits::Table;

use super::read::table_name;
use super::types::StoolapDB;
use super::{STOOLAP_ERROR, STOOLAP_OK};
use crate::record::{pad_unit, read_value, read_values, same_in, EncodeFn, RecordIn, StoolapField};
use crate::session::Session;
use crate::sql_write;
use crate::write::{self, ReadCheck};

/// Runs `direct` on `table`'s write handle, or `sql` when a foreign key
/// names the table or the row is named `by_key`, storing the count in
/// `affected`. `unchecked` runs `direct` whatever keys name the table:
/// rows the caller checks against them after (a copying ALTER's)
#[allow(clippy::too_many_arguments)]
unsafe fn write_through(
    db: *mut StoolapDB,
    table: *const c_char,
    affected: *mut i64,
    what: &str,
    by_key: bool,
    unchecked: bool,
    direct: impl FnOnce(&mut dyn Table) -> Result<i64>,
    sql: impl FnOnce(&Session, &str) -> Result<i64>,
) -> i32 {
    let Some(handle) = db.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| -> Result<i64> {
        let name = table_name(table)?;
        let session = &handle.session;
        if by_key {
            return sql(session, name);
        }
        if unchecked {
            return session.with_any_write_table(name, direct);
        }
        match session.with_write_table(name, direct)? {
            Some(n) => Ok(n),
            None => sql(session, name),
        }
    }));
    match result {
        Ok(Ok(n)) => {
            if !affected.is_null() {
                *affected = n;
            }
            STOOLAP_OK
        }
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

unsafe fn field_slice<'a>(fields: *const StoolapField, n: i32) -> Result<&'a [StoolapField]> {
    if n <= 0 {
        return Ok(&[]);
    }
    if fields.is_null() {
        return Err(Error::internal("NULL fields with a non-zero count"));
    }
    Ok(std::slice::from_raw_parts(fields, n as usize))
}

/// The values of a whole row: `fields` must name columns 0.. in order
unsafe fn row_values(fields: &[StoolapField], src: &RecordIn) -> Result<Vec<Value>> {
    if fields
        .iter()
        .enumerate()
        .any(|(i, f)| f.column as usize != i)
    {
        return Err(Error::internal(
            "a row's fields must name every column in order",
        ));
    }
    read_values(fields, src)
}

/// `fields`' column names and their values in `src`
unsafe fn named(
    session: &Session,
    table: &str,
    fields: &[StoolapField],
    src: &RecordIn,
) -> Result<Vec<(String, Value)>> {
    let names = session.column_names(table)?;
    fields
        .iter()
        .map(|f| {
            let name = names
                .get(f.column as usize)
                .ok_or_else(|| Error::internal("column out of range"))?;
            Ok((name.clone(), read_value(f, src)?))
        })
        .collect()
}

fn columns(table_columns: i32) -> Result<usize> {
    usize::try_from(table_columns).map_err(|_| Error::internal("bad column count"))
}

/// Holds row `row_id` for a write through SQL as `write::hold_unchanged`
/// does, failing it when the row no longer holds what was read; false when
/// the row is gone, which the write must not go on to (a row inserted since
/// under its key is another one). Through the storage handle, in the
/// statement's transaction: an UPDATE would run the actions of the foreign
/// keys that name the column it writes, and a DELETE finds its rows before
/// it deletes them, with no second look.
fn hold_read(
    session: &Session,
    table: &str,
    row_id: i64,
    columns: &[usize],
    values: &[Value],
    pad: &[Option<&'static str>],
) -> Result<bool> {
    let read = ReadCheck {
        columns,
        values,
        pad,
    };
    Ok(session
        .with_any_write_table(table, |t| write::hold_unchanged(t, row_id, &read))?
        .is_some())
}

/// 1 when a foreign key names `table` (as child or parent), so its writes
/// go through SQL; 0 when they go direct; `STOOLAP_ERROR` on error.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `table` a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn stoolap_writes_through_sql(
    db: *mut StoolapDB,
    table: *const c_char,
) -> i32 {
    let Some(handle) = db.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        handle.session.writes_through_sql(table_name(table)?)
    }));
    match result {
        Ok(Ok(fk)) => fk as i32,
        Ok(Err(e)) => {
            handle.set_error(&e.to_string());
            super::error::note_error(&e);
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.set_error("panic during stoolap_writes_through_sql");
            STOOLAP_ERROR
        }
    }
}

/// Checks every row of `table` against its foreign keys: the rows a
/// copying ALTER wrote unchecked. `STOOLAP_OK`, or the first row's
/// violation.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `table` a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn stoolap_check_foreign_keys(
    db: *mut StoolapDB,
    table: *const c_char,
) -> i32 {
    let Some(handle) = db.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        sql_write::check_foreign_keys(&handle.session, table_name(table)?)
    }));
    match result {
        Ok(Ok(())) => STOOLAP_OK,
        Ok(Err(e)) => {
            handle.set_error(&e.to_string());
            super::error::note_error(&e);
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.set_error("panic during stoolap_check_foreign_keys");
            STOOLAP_ERROR
        }
    }
}

/// Inserts the row in `record`: `fields` describe every column, in order,
/// and `encode(ctx, ...)` gives the values they do not read themselves.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `table` a NUL-terminated UTF-8
/// string, `fields` hold `nfields` entries and `record` be a record buffer
/// of the layout they describe. `affected` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_insert_record(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    fields: *const StoolapField,
    nfields: i32,
    record: *const u8,
    encode: Option<EncodeFn>,
    ctx: *mut c_void,
    unchecked: i32,
    affected: *mut i64,
) -> i32 {
    let src = RecordIn {
        record,
        encode,
        ctx,
    };
    write_through(
        db,
        table,
        affected,
        "stoolap_insert_record",
        false,
        unchecked != 0,
        |t| {
            let row = row_values(field_slice(fields, nfields)?, &src)?;
            write::insert(t, columns(table_columns)?, vec![row])
        },
        |session, name| {
            let row = row_values(field_slice(fields, nfields)?, &src)?;
            sql_write::insert(session, name, vec![row])
        },
    )
}

/// Rows a bulk insert holds until it writes them
pub struct StoolapRowBuffer {
    rows: Vec<Vec<Value>>,
    last_error: Option<CString>,
}

#[no_mangle]
pub extern "C" fn stoolap_row_buffer_new() -> *mut StoolapRowBuffer {
    Box::into_raw(Box::new(StoolapRowBuffer {
        rows: Vec::new(),
        last_error: None,
    }))
}

/// # Safety
///
/// `buffer` must be a valid `StoolapRowBuffer` pointer or NULL; it is freed.
#[no_mangle]
pub unsafe extern "C" fn stoolap_row_buffer_free(buffer: *mut StoolapRowBuffer) {
    if !buffer.is_null() {
        drop(Box::from_raw(buffer));
    }
}

/// Adds the row in `record` (as `stoolap_insert_record` reads it); returns
/// the buffer's row count, or -1 (see `stoolap_row_buffer_errmsg`).
///
/// # Safety
///
/// As `stoolap_insert_record`; `buffer` must be a valid `StoolapRowBuffer`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_row_buffer_add(
    buffer: *mut StoolapRowBuffer,
    fields: *const StoolapField,
    nfields: i32,
    record: *const u8,
    encode: Option<EncodeFn>,
    ctx: *mut c_void,
) -> i64 {
    let Some(buf) = buffer.as_mut() else {
        return -1;
    };
    buf.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        let src = RecordIn {
            record,
            encode,
            ctx,
        };
        row_values(field_slice(fields, nfields)?, &src)
    }));
    let msg = match result {
        Ok(Ok(row)) => {
            buf.rows.push(row);
            return buf.rows.len() as i64;
        }
        Ok(Err(e)) => {
            super::error::note_error(&e);
            e.to_string()
        }
        Err(_) => "panic during stoolap_row_buffer_add".to_string(),
    };
    buf.last_error = CString::new(super::error::error_text(&msg)).ok();
    -1
}

/// Drops the rows the buffer holds, and their memory
///
/// # Safety
///
/// `buffer` must be a valid `StoolapRowBuffer` pointer or NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_row_buffer_clear(buffer: *mut StoolapRowBuffer) {
    if let Some(buf) = buffer.as_mut() {
        buf.rows = Vec::new();
    }
}

/// # Safety
///
/// `buffer` must be a valid `StoolapRowBuffer` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_row_buffer_errmsg(
    buffer: *const StoolapRowBuffer,
) -> *const c_char {
    match buffer.as_ref().and_then(|b| b.last_error.as_ref()) {
        Some(e) => e.as_ptr(),
        None => super::error::empty_cstr(),
    }
}

/// Inserts the rows `buffer` holds, which it gives up (the buffer is left
/// empty whatever the outcome). Through SQL, rows outside an open
/// transaction go in one of their own.
///
/// # Safety
///
/// As `stoolap_insert_record`; `buffer` must be a valid `StoolapRowBuffer`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_insert_buffer(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    buffer: *mut StoolapRowBuffer,
    unchecked: i32,
    affected: *mut i64,
) -> i32 {
    let rows = match buffer.as_mut() {
        Some(buf) => Cell::new(std::mem::take(&mut buf.rows)),
        None => return STOOLAP_ERROR,
    };
    write_through(
        db,
        table,
        affected,
        "stoolap_insert_buffer",
        false,
        unchecked != 0,
        |t| write::insert(t, columns(table_columns)?, rows.take()),
        |session, name| sql_write::insert(session, name, rows.take()),
    )
}

/// Sets the row from `new_record` where `fields` (the columns the
/// statement writes) differ from `old_record`: row `row_id` when
/// `has_row_id`, else the row the primary key `keys` holds in `old_record`
/// names. `affected` gets 1 when nothing differs, 0 when the row is gone.
///
/// # Safety
///
/// As `stoolap_insert_record`, for both records; `keys` holds `nkeys`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_update_record(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    row_id: i64,
    has_row_id: i32,
    fields: *const StoolapField,
    nfields: i32,
    keys: *const StoolapField,
    nkeys: i32,
    read: *const StoolapField,
    nread: i32,
    old_record: *const u8,
    new_record: *const u8,
    encode: Option<EncodeFn>,
    ctx: *mut c_void,
    affected: *mut i64,
) -> i32 {
    let changed: Vec<StoolapField> = match field_slice(fields, nfields) {
        Ok(fields) => fields
            .iter()
            .filter(|f| !same_in(f, old_record, new_record))
            .copied()
            .collect(),
        Err(_) => return STOOLAP_ERROR,
    };
    if changed.is_empty() {
        if !affected.is_null() {
            *affected = 1;
        }
        return STOOLAP_OK;
    }
    let new = RecordIn {
        record: new_record,
        encode,
        ctx,
    };
    let old = RecordIn {
        record: old_record,
        encode,
        ctx,
    };
    let read = match field_slice(read, nread) {
        Ok(fields) => fields,
        Err(_) => return STOOLAP_ERROR,
    };
    let read_columns: Vec<usize> = read.iter().map(|f| f.column as usize).collect();
    let pad: Vec<_> = read.iter().map(pad_unit).collect();
    write_through(
        db,
        table,
        affected,
        "stoolap_update_record",
        has_row_id == 0,
        false,
        |t| {
            let set: Vec<usize> = changed.iter().map(|f| f.column as usize).collect();
            let values = read_values(&changed, &new)?;
            let was = read_values(read, &old)?;
            write::update(
                t,
                columns(table_columns)?,
                row_id,
                &set,
                values,
                &ReadCheck {
                    columns: &read_columns,
                    values: &was,
                    pad: &pad,
                },
            )
        },
        |session, name| {
            if has_row_id != 0 {
                let was = read_values(read, &old)?;
                if !hold_read(session, name, row_id, &read_columns, &was, &pad)? {
                    return Ok(0);
                }
            }
            let keys = field_slice(keys, nkeys)?;
            let set = named(session, name, &changed, &new)?;
            let key = named(session, name, keys, &old)?;
            let columns: Vec<usize> = changed.iter().map(|f| f.column as usize).collect();
            let check = (has_row_id != 0).then_some((row_id, columns.as_slice()));
            sql_write::update(session, name, set, key, check)
        },
    )
}

/// Deletes row `row_id` when `has_row_id`, else the row the primary key
/// `keys` holds in `record` names; `affected` gets 0 when it is gone.
///
/// # Safety
///
/// As `stoolap_insert_record`; `keys` holds `nkeys`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_delete_record(
    db: *mut StoolapDB,
    table: *const c_char,
    row_id: i64,
    has_row_id: i32,
    keys: *const StoolapField,
    nkeys: i32,
    read: *const StoolapField,
    nread: i32,
    record: *const u8,
    encode: Option<EncodeFn>,
    ctx: *mut c_void,
    affected: *mut i64,
) -> i32 {
    let src = RecordIn {
        record,
        encode,
        ctx,
    };
    let read = match field_slice(read, nread) {
        Ok(fields) => fields,
        Err(_) => return STOOLAP_ERROR,
    };
    let read_columns: Vec<usize> = read.iter().map(|f| f.column as usize).collect();
    let pad: Vec<_> = read.iter().map(pad_unit).collect();
    write_through(
        db,
        table,
        affected,
        "stoolap_delete_record",
        has_row_id == 0,
        false,
        |t| {
            let was = read_values(read, &src)?;
            let check = ReadCheck {
                columns: &read_columns,
                values: &was,
                pad: &pad,
            };
            write::delete(t, row_id, &check)
        },
        |session, name| {
            if has_row_id != 0 {
                let was = read_values(read, &src)?;
                if !hold_read(session, name, row_id, &read_columns, &was, &pad)? {
                    return Ok(0);
                }
            }
            let key = named(session, name, field_slice(keys, nkeys)?, &src)?;
            sql_write::delete(session, name, key)
        },
    )
}
