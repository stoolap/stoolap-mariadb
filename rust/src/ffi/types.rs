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

//! FFI type definitions: opaque wrapper structs and `#[repr(C)]` value type.

use std::ffi::CString;
use std::os::raw::c_char;

use std::sync::Arc;

use crate::session::{Rows, Session};

/// FFI-safe tagged union for passing parameter values across the C boundary.
#[repr(C)]
pub struct StoolapValue {
    /// One of `STOOLAP_TYPE_*` constants.
    pub value_type: i32,
    pub _padding: i32,
    pub v: StoolapValueData,
}

/// Union payload for [`StoolapValue`].
#[repr(C)]
pub union StoolapValueData {
    pub integer: i64,
    pub float64: f64,
    pub boolean: i32,
    pub text: StoolapTextData,
    pub blob: StoolapBlobData,
    pub timestamp_nanos: i64,
}

/// Text pointer + length (not necessarily null-terminated on input).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct StoolapTextData {
    pub ptr: *const c_char,
    pub len: i64,
}

/// Blob pointer + length.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct StoolapBlobData {
    pub ptr: *const u8,
    pub len: i64,
}

/// Opaque handle wrapping a [`Database`] connection.
pub struct StoolapDB {
    pub(crate) session: Arc<Session>,
    pub(crate) last_error: Option<CString>,
    /// Buffer behind `stoolap_query_params_all()`, reused across calls
    pub(crate) packed: Vec<u8>,
}

/// Opaque handle wrapping a [`Transaction`].
/// The transaction open in `session`, until commit or rollback consumes
/// the handle
pub struct StoolapTx {
    pub(crate) session: Arc<Session>,
    pub(crate) last_error: Option<CString>,
}

/// Opaque handle wrapping a [`Rows`] result set.
pub struct StoolapRows {
    pub(crate) rows: Option<Rows>,
    pub(crate) has_row: bool,
    /// Column names of the result, kept after the rows are consumed
    pub(crate) columns: Vec<String>,
    /// Per column, the row it was read for and its text with a trailing NUL.
    /// Buffers are reused from row to row; an entry is current when its row
    /// matches `row_gen`
    pub(crate) text_cache: Vec<(u64, Vec<u8>)>,
    /// Counts the rows `stoolap_rows_next()` advanced to
    pub(crate) row_gen: u64,
}

impl StoolapDB {
    pub(crate) fn set_error(&mut self, msg: &str) {
        let sanitized = msg.replace('\0', "\\0");
        self.last_error = CString::new(sanitized).ok();
    }

    pub(crate) fn error_ptr(&self) -> *const c_char {
        match &self.last_error {
            Some(cs) => cs.as_ptr(),
            None => super::error::empty_cstr(),
        }
    }
}

impl StoolapTx {
    pub(crate) fn set_error(&mut self, msg: &str) {
        let sanitized = msg.replace('\0', "\\0");
        self.last_error = CString::new(sanitized).ok();
    }

    pub(crate) fn error_ptr(&self) -> *const c_char {
        match &self.last_error {
            Some(cs) => cs.as_ptr(),
            None => super::error::empty_cstr(),
        }
    }
}

impl Drop for StoolapTx {
    fn drop(&mut self) {
        // A handle dropped without commit or rollback rolls back, as
        // stoolap's own Transaction does
        if self.session.in_transaction() {
            let _ = self.session.rollback();
        }
    }
}

impl StoolapRows {
    /// A handle on `rows`
    pub(crate) fn boxed(rows: Rows) -> *mut Self {
        let handle = Box::new(StoolapRows {
            columns: rows.columns().to_vec(),
            rows: Some(rows),
            has_row: false,
            text_cache: Vec::new(),
            row_gen: 0,
        });
        Box::into_raw(handle)
    }
}
