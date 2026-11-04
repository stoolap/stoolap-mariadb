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

use std::os::raw::c_char;
use std::panic;

use stoolap::core::{Error, Result, Value};
use stoolap::storage::traits::Table;

use super::read::table_name;
use super::types::{StoolapDB, StoolapValue};
use super::{value, STOOLAP_ERROR, STOOLAP_OK};
use crate::write;

pub const STOOLAP_FALLBACK: i32 = 2;

unsafe fn values(ptr: *const StoolapValue, n: usize) -> Result<Vec<Value>> {
    if n == 0 {
        return Ok(Vec::new());
    }
    if ptr.is_null() {
        return Err(Error::internal("NULL values with a non-zero count"));
    }
    std::slice::from_raw_parts(ptr, n)
        .iter()
        .map(|v| value::ffi_value_to_rust(v))
        .collect()
}

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

/// Inserts `nrows` rows of `table_columns` values each, row after row.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `table` a NUL-terminated UTF-8
/// string, `values` hold `nrows * table_columns` values. `affected` may be
/// NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_insert(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    values_ptr: *const StoolapValue,
    nrows: i32,
    affected: *mut i64,
) -> i32 {
    on_write_table(db, table, affected, "stoolap_insert", |t| {
        let cols =
            usize::try_from(table_columns).map_err(|_| Error::internal("bad column count"))?;
        let nrows = usize::try_from(nrows).map_err(|_| Error::internal("bad row count"))?;
        let all = values(values_ptr, cols * nrows)?;
        let mut rows = Vec::with_capacity(nrows);
        let mut it = all.into_iter();
        for _ in 0..nrows {
            rows.push(it.by_ref().take(cols).collect());
        }
        write::insert(t, cols, rows)
    })
}

/// Sets `ncolumns` columns of row `row_id` to `values`; `affected` gets 0
/// when the row is gone.
///
/// # Safety
///
/// As `stoolap_insert`; `columns` and `values` hold `ncolumns` entries.
#[no_mangle]
pub unsafe extern "C" fn stoolap_update_row(
    db: *mut StoolapDB,
    table: *const c_char,
    table_columns: i32,
    row_id: i64,
    columns: *const i32,
    values_ptr: *const StoolapValue,
    ncolumns: i32,
    affected: *mut i64,
) -> i32 {
    on_write_table(db, table, affected, "stoolap_update_row", |t| {
        let cols =
            usize::try_from(table_columns).map_err(|_| Error::internal("bad column count"))?;
        let n = usize::try_from(ncolumns).map_err(|_| Error::internal("bad column count"))?;
        let positions: Vec<usize> = if n == 0 {
            Vec::new()
        } else if columns.is_null() {
            return Err(Error::internal("NULL columns"));
        } else {
            std::slice::from_raw_parts(columns, n)
                .iter()
                .map(|&c| usize::try_from(c).map_err(|_| Error::internal("negative column")))
                .collect::<Result<_>>()?
        };
        write::update(t, cols, row_id, &positions, values(values_ptr, n)?)
    })
}

/// Deletes row `row_id`; `affected` gets 0 when the row is gone.
///
/// # Safety
///
/// As `stoolap_insert`.
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
