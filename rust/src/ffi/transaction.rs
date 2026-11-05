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

//! Transaction FFI functions. A transaction is installed in its database
//! handle's session; the `StoolapTx` handle names it until commit or
//! rollback consumes the handle.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::panic;
use std::sync::Arc;

use stoolap::core::types::IsolationLevel;
use stoolap::core::{Error, Result};

use super::types::{StoolapDB, StoolapRows, StoolapTx, StoolapValue};
use super::value;
use super::{
    STOOLAP_ERROR, STOOLAP_ISOLATION_READ_COMMITTED, STOOLAP_ISOLATION_SNAPSHOT, STOOLAP_OK,
};
use crate::session::{Rows, Session};

/// Begin a transaction with a specific isolation level.
///
/// # Safety
///
/// - `db` must be a valid `StoolapDB` pointer.
/// - `isolation` must be `STOOLAP_ISOLATION_READ_COMMITTED` or `STOOLAP_ISOLATION_SNAPSHOT`.
/// - `out_tx` must be a valid pointer to a `*mut StoolapTx`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_begin_with_isolation(
    db: *mut StoolapDB,
    isolation: i32,
    out_tx: *mut *mut StoolapTx,
) -> i32 {
    if out_tx.is_null() {
        super::error::set_global_error("out_tx pointer is NULL");
        return STOOLAP_ERROR;
    }
    *out_tx = std::ptr::null_mut();

    let handle = match db.as_mut() {
        Some(h) => h,
        None => {
            super::error::set_global_error("db handle is NULL");
            return STOOLAP_ERROR;
        }
    };
    handle.last_error = None;

    let level = match isolation {
        STOOLAP_ISOLATION_READ_COMMITTED => IsolationLevel::ReadCommitted,
        STOOLAP_ISOLATION_SNAPSHOT => IsolationLevel::SnapshotIsolation,
        _ => {
            handle.set_error("invalid isolation level");
            return STOOLAP_ERROR;
        }
    };

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        match handle.session.begin(level) {
            Ok(()) => {
                *out_tx = Box::into_raw(Box::new(StoolapTx {
                    session: Arc::clone(&handle.session),
                    last_error: None,
                }));
                STOOLAP_OK
            }
            Err(e) => {
                handle.set_error(&e.to_string());
                super::error::note_error(&e);
                STOOLAP_ERROR
            }
        }
    }));

    result.unwrap_or_else(|_| {
        handle.set_error("panic during stoolap_begin_with_isolation");
        STOOLAP_ERROR
    })
}

/// Runs `f` on the transaction's session, recording an error or a panic
/// on the handle.
unsafe fn on_tx(tx: *mut StoolapTx, what: &str, f: impl FnOnce(&Session) -> Result<()>) -> i32 {
    let handle = match tx.as_mut() {
        Some(h) => h,
        None => return STOOLAP_ERROR,
    };
    handle.last_error = None;

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        if !handle.session.in_transaction() {
            return Err(Error::TransactionEnded);
        }
        f(&handle.session)
    }));

    match result {
        Ok(Ok(())) => STOOLAP_OK,
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

unsafe fn sql_str<'a>(sql: *const c_char) -> Result<&'a str> {
    if sql.is_null() {
        return Err(Error::internal("SQL string is NULL"));
    }
    CStr::from_ptr(sql)
        .to_str()
        .map_err(|e| Error::internal(format!("invalid UTF-8 in SQL: {}", e)))
}

/// Execute a SQL statement within a transaction (no parameters).
///
/// # Safety
///
/// - `tx` must be a valid `StoolapTx` pointer.
/// - `sql` must be a valid null-terminated UTF-8 string.
/// - `rows_affected` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_tx_exec(
    tx: *mut StoolapTx,
    sql: *const c_char,
    rows_affected: *mut i64,
) -> i32 {
    stoolap_tx_exec_params(tx, sql, std::ptr::null(), 0, rows_affected)
}

/// Execute a SQL statement within a transaction (with parameters).
///
/// # Safety
///
/// - `tx` must be a valid `StoolapTx` pointer.
/// - `sql` must be a valid null-terminated UTF-8 string.
/// - `params` must point to `params_len` valid `StoolapValue` structs (or be NULL).
/// - `rows_affected` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_tx_exec_params(
    tx: *mut StoolapTx,
    sql: *const c_char,
    params: *const StoolapValue,
    params_len: i32,
    rows_affected: *mut i64,
) -> i32 {
    on_tx(tx, "stoolap_tx_exec_params", |session| {
        let sql = sql_str(sql)?;
        let p = value::params_to_vec(params, params_len)?;
        let affected = session.run(sql, p)?.rows_affected();
        if !rows_affected.is_null() {
            *rows_affected = affected;
        }
        Ok(())
    })
}

/// Query within a transaction (no parameters).
///
/// # Safety
///
/// - `tx` must be a valid `StoolapTx` pointer.
/// - `sql` must be a valid null-terminated UTF-8 string.
/// - `out_rows` must be a valid pointer to a `*mut StoolapRows`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_tx_query(
    tx: *mut StoolapTx,
    sql: *const c_char,
    out_rows: *mut *mut StoolapRows,
) -> i32 {
    tx_query_params(tx, sql, std::ptr::null(), 0, out_rows)
}

/// Query within a transaction (with parameters).
///
/// # Safety
///
/// - `tx` must be a valid `StoolapTx` pointer.
/// - `sql` must be a valid null-terminated UTF-8 string.
/// - `params` must point to `params_len` valid `StoolapValue` structs (or be NULL).
/// - `out_rows` must be a valid pointer to a `*mut StoolapRows`.
unsafe fn tx_query_params(
    tx: *mut StoolapTx,
    sql: *const c_char,
    params: *const StoolapValue,
    params_len: i32,
    out_rows: *mut *mut StoolapRows,
) -> i32 {
    if out_rows.is_null() {
        return STOOLAP_ERROR;
    }
    *out_rows = std::ptr::null_mut();
    on_tx(tx, "stoolap_tx_query_params", |session| {
        let sql = sql_str(sql)?;
        let p = value::params_to_vec(params, params_len)?;
        *out_rows = StoolapRows::boxed(Rows::new(session.run(sql, p)?));
        Ok(())
    })
}

/// Ends a transaction with `end`; the handle is consumed (freed), so an
/// error goes to the thread's global error.
unsafe fn end_tx(tx: *mut StoolapTx, end: impl FnOnce(&Session) -> Result<()>) -> i32 {
    if tx.is_null() {
        return STOOLAP_ERROR;
    }
    let handle = Box::from_raw(tx);
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| end(&handle.session)));
    match result {
        Ok(Ok(())) => STOOLAP_OK,
        Ok(Err(e)) => {
            super::error::set_global_error(&e.to_string());
            super::error::note_error(&e);
            STOOLAP_ERROR
        }
        Err(_) => STOOLAP_ERROR,
    }
}

/// Commit a transaction. The tx handle is consumed (freed).
///
/// # Safety
///
/// `tx` must be a valid `StoolapTx` pointer.
/// After this call (success or failure), the pointer is invalid.
#[no_mangle]
pub unsafe extern "C" fn stoolap_tx_commit(tx: *mut StoolapTx) -> i32 {
    end_tx(tx, Session::commit)
}

/// Rollback a transaction. The tx handle is consumed (freed).
///
/// # Safety
///
/// `tx` must be a valid `StoolapTx` pointer.
/// After this call (success or failure), the pointer is invalid.
#[no_mangle]
pub unsafe extern "C" fn stoolap_tx_rollback(tx: *mut StoolapTx) -> i32 {
    end_tx(tx, Session::rollback)
}

/// Get the last error message for a transaction handle.
///
/// # Safety
///
/// `tx` must be a valid `StoolapTx` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_tx_errmsg(tx: *const StoolapTx) -> *const c_char {
    match tx.as_ref() {
        Some(handle) => handle.error_ptr(),
        None => super::error::global_error_ptr(),
    }
}
