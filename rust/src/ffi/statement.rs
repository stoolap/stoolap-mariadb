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

//! Prepared statement FFI functions.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::panic;

use std::sync::Arc;

use stoolap::api::ParamVec;

use super::types::{StoolapDB, StoolapStmt, StoolapValue};
use super::value;
use super::{STOOLAP_ERROR, STOOLAP_OK};

/// Prepare a SQL statement for repeated execution.
///
/// # Safety
///
/// - `db` must be a valid `StoolapDB` pointer.
/// - `sql` must be a valid null-terminated UTF-8 string.
/// - `out_stmt` must be a valid pointer to a `*mut StoolapStmt`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_prepare(
    db: *mut StoolapDB,
    sql: *const c_char,
    out_stmt: *mut *mut StoolapStmt,
) -> i32 {
    if out_stmt.is_null() {
        return STOOLAP_ERROR;
    }
    *out_stmt = std::ptr::null_mut();

    let handle = match db.as_mut() {
        Some(h) => h,
        None => return STOOLAP_ERROR,
    };
    handle.last_error = None;

    if sql.is_null() {
        handle.set_error("SQL string is NULL");
        return STOOLAP_ERROR;
    }

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        let sql_str = match CStr::from_ptr(sql).to_str() {
            Ok(s) => s,
            Err(e) => {
                handle.set_error(&format!("invalid UTF-8 in SQL: {}", e));
                return STOOLAP_ERROR;
            }
        };

        match handle.session.prepare(sql_str) {
            Ok(plan) => {
                let stmt_handle = Box::new(StoolapStmt {
                    plan,
                    last_error: None,
                    _session: Arc::clone(&handle.session),
                });
                *out_stmt = Box::into_raw(stmt_handle);
                STOOLAP_OK
            }
            Err(e) => {
                handle.set_error(&e.to_string());
                STOOLAP_ERROR
            }
        }
    }));

    result.unwrap_or_else(|_| {
        handle.set_error("panic during stoolap_prepare");
        STOOLAP_ERROR
    })
}

/// Execute a prepared statement as a batch: opens a transaction, executes
/// the statement once per parameter row, then commits. One FFI call replaces
/// `2 + N` calls (begin + N × exec + commit).
///
/// `params` is a flat array of `row_count * params_per_row` StoolapValue
/// structs laid out row-major: row0_p0, row0_p1, ..., rowN_pK.
///
/// On error the transaction is rolled back and the error is stored on `db`.
///
/// # Safety
///
/// - `db` must be a valid `StoolapDB` pointer.
/// - `stmt` must be a valid `StoolapStmt` pointer prepared from `db`.
/// - `params` must point to `row_count * params_per_row` valid `StoolapValue`
///   structs (or be NULL when `row_count == 0`).
/// - `total_affected` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_stmt_exec_batch(
    db: *mut StoolapDB,
    stmt: *const StoolapStmt,
    params: *const StoolapValue,
    params_per_row: i32,
    row_count: i32,
    total_affected: *mut i64,
) -> i32 {
    let db_handle = match db.as_mut() {
        Some(h) => h,
        None => {
            super::error::set_global_error("db handle is NULL");
            return STOOLAP_ERROR;
        }
    };
    db_handle.last_error = None;

    let stmt_handle = match stmt.as_ref() {
        Some(h) => h,
        None => {
            db_handle.set_error("statement handle is NULL");
            return STOOLAP_ERROR;
        }
    };

    if row_count <= 0 {
        if !total_affected.is_null() {
            *total_affected = 0;
        }
        return STOOLAP_OK;
    }
    if params_per_row < 0 || (params_per_row > 0 && params.is_null()) {
        db_handle.set_error("params must hold row_count * params_per_row values");
        return STOOLAP_ERROR;
    }

    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| {
        let session = &db_handle.session;
        let level = session.isolation_level();
        if let Err(e) = session.begin(level) {
            db_handle.set_error(&e.to_string());
            return STOOLAP_ERROR;
        }

        let ppr = params_per_row as usize;
        let mut total: i64 = 0;

        for r in 0..(row_count as usize) {
            let param_vec = if ppr == 0 {
                Ok(ParamVec::new())
            } else {
                value::params_to_vec(params.add(r * ppr), params_per_row)
            };

            match param_vec.and_then(|p| session.run_plan(&stmt_handle.plan, p)) {
                Ok(result) => total += result.rows_affected(),
                Err(e) => {
                    let _ = session.rollback();
                    db_handle.set_error(&e.to_string());
                    return STOOLAP_ERROR;
                }
            }
        }

        match session.commit() {
            Ok(_) => {
                if !total_affected.is_null() {
                    *total_affected = total;
                }
                STOOLAP_OK
            }
            Err(e) => {
                db_handle.set_error(&e.to_string());
                STOOLAP_ERROR
            }
        }
    }));

    result.unwrap_or_else(|_| {
        db_handle.set_error("panic during stoolap_stmt_exec_batch");
        STOOLAP_ERROR
    })
}

#[no_mangle]
pub unsafe extern "C" fn stoolap_stmt_finalize(stmt: *mut StoolapStmt) {
    if stmt.is_null() {
        return;
    }
    drop(Box::from_raw(stmt));
}

/// Get the last error message for a statement handle.
///
/// # Safety
///
/// `stmt` must be a valid `StoolapStmt` pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_stmt_errmsg(stmt: *const StoolapStmt) -> *const c_char {
    match stmt.as_ref() {
        Some(handle) => handle.error_ptr(),
        None => super::error::empty_cstr(),
    }
}
