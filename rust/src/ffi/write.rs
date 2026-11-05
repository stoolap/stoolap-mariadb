// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Direct write FFI. Each call writes in the open transaction, or else in
//! the statement's own (see `stoolap_end_statement`), and returns
//! `STOOLAP_FALLBACK` for a table a foreign key names: the caller writes
//! those through SQL, where stoolap's executor checks the keys.

use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::panic;

use stoolap::core::{Error, Result, Value};
use stoolap::storage::traits::Table;

use super::read::table_name;
use super::types::StoolapDB;
use super::{STOOLAP_ERROR, STOOLAP_OK};
use crate::record::{read_value, read_values, same_in, EncodeFn, RecordIn, StoolapField};
use crate::write;

pub const STOOLAP_FALLBACK: i32 = 2;

/// Runs `f` on `table`'s write handle, storing the count in `affected`
unsafe fn on_write_table(
    db: *mut StoolapDB,
    table: *const c_char,
    affected: *mut i64,
    what: &str,
    f: impl FnOnce(&mut dyn Table) -> Result<i64>,
) -> i32 {
    let Some(handle) = db.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        let name = table_name(table)?;
        handle.session.with_write_table(name, f)
    }));
    match result {
        Ok(Ok(Some(n))) => {
            if !affected.is_null() {
                *affected = n;
            }
            STOOLAP_OK
        }
        Ok(Ok(None)) => STOOLAP_FALLBACK,
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

/// Deletes row `row_id`; `affected` gets 0 when the row is gone.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `table` a NUL-terminated UTF-8
/// string. `affected` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_delete_row(
    db: *mut StoolapDB,
    table: *const c_char,
    row_id: i64,
    affected: *mut i64,
) -> i32 {
    on_write_table(db, table, affected, "stoolap_delete_row", |t| {
        write::delete(t, row_id)
    })
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

/// 1 when a foreign key names `table`, whose writes return
/// `STOOLAP_FALLBACK`; 0 when they go direct; `STOOLAP_ERROR` on error.
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
            STOOLAP_ERROR
        }
        Err(_) => {
            handle.set_error("panic during stoolap_writes_through_sql");
            STOOLAP_ERROR
        }
    }
}

/// Inserts the row in `record`: `fields` describe every column, in order,
/// and `encode(ctx, ...)` gives the values they do not read themselves.
///
/// # Safety
///
/// As `stoolap_delete_row`; `fields` holds `nfields` entries and `record`
/// must be a record buffer of the layout they describe.
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
    affected: *mut i64,
) -> i32 {
    on_write_table(db, table, affected, "stoolap_insert_record", |t| {
        let cols =
            usize::try_from(table_columns).map_err(|_| Error::internal("bad column count"))?;
        let src = RecordIn {
            record,
            encode,
            ctx,
        };
        let row = row_values(field_slice(fields, nfields)?, &src)?;
        write::insert(t, cols, vec![row])
    })
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
        Ok(Err(e)) => e.to_string(),
        Err(_) => "panic during stoolap_row_buffer_add".to_string(),
    };
    buf.last_error = CString::new(msg.replace('\0', "\\0")).ok();
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
/// empty whatever the outcome).
///
/// # Safety
///
/// As `stoolap_delete_row`; `buffer` must be a valid `StoolapRowBuffer`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_insert_buffer(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    buffer: *mut StoolapRowBuffer,
    affected: *mut i64,
) -> i32 {
    let rows = match buffer.as_mut() {
        Some(buf) => std::mem::take(&mut buf.rows),
        None => return STOOLAP_ERROR,
    };
    on_write_table(db, table, affected, "stoolap_insert_buffer", |t| {
        let cols =
            usize::try_from(table_columns).map_err(|_| Error::internal("bad column count"))?;
        write::insert(t, cols, rows)
    })
}

/// Sets row `row_id` from `new_record` where `fields` (the columns the
/// statement writes) differ from `old_record`; `affected` gets 1 when
/// nothing differs, 0 when the row is gone.
///
/// # Safety
///
/// As `stoolap_insert_record`, for both records.
#[no_mangle]
pub unsafe extern "C" fn stoolap_update_record(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    row_id: i64,
    fields: *const StoolapField,
    nfields: i32,
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
    on_write_table(db, table, affected, "stoolap_update_record", |t| {
        let cols =
            usize::try_from(table_columns).map_err(|_| Error::internal("bad column count"))?;
        let src = RecordIn {
            record: new_record,
            encode,
            ctx,
        };
        let mut columns = Vec::with_capacity(changed.len());
        let mut values = Vec::with_capacity(changed.len());
        for f in &changed {
            columns.push(f.column as usize);
            values.push(read_value(f, &src)?);
        }
        write::update(t, cols, row_id, &columns, values)
    })
}
