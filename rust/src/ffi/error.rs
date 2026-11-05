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

//! Error storage for FFI: global error for `stoolap_open` failures.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::c_char;

// Thread-local error for stoolap_open() failures (before a handle exists)
// and for stoolap_tx_commit/stoolap_tx_rollback failures (handle consumed).
// Thread-local storage ensures the returned pointer remains valid until the
// next API call on the same thread, without cross-thread races.
thread_local! {
    static THREAD_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

pub(crate) fn set_global_error(msg: &str) {
    THREAD_ERROR.with(|cell| {
        *cell.borrow_mut() = CString::new(msg.replace('\0', "\\0")).ok();
    });
}

pub(crate) fn global_error_ptr() -> *const c_char {
    THREAD_ERROR.with(|cell| {
        let borrow = cell.borrow();
        match &*borrow {
            Some(cs) => cs.as_ptr(),
            None => empty_cstr(),
        }
    })
}

/// Returns a pointer to a static empty C string.
pub(crate) fn empty_cstr() -> *const c_char {
    // SAFETY: b"\0" is a valid null-terminated C string.
    static EMPTY: &[u8] = b"\0";
    EMPTY.as_ptr() as *const c_char
}

// =========================================================================
// Error classes
// =========================================================================

/// A wording no class covers: the plugin counts these (stoolap drift)
pub const STOOLAP_ERR_UNKNOWN: i32 = 0;
/// A known error with no MariaDB counterpart: reported with its text
pub const STOOLAP_ERR_GENERIC: i32 = 1;
pub const STOOLAP_ERR_DUP_KEY: i32 = 2;
pub const STOOLAP_ERR_NO_REFERENCED_ROW: i32 = 3;
pub const STOOLAP_ERR_ROW_IS_REFERENCED: i32 = 4;
/// Retry the statement: a write conflict, a lock, a schema change
pub const STOOLAP_ERR_DEADLOCK: i32 = 5;
pub const STOOLAP_ERR_TABLE_EXISTS: i32 = 6;
pub const STOOLAP_ERR_NO_SUCH_TABLE: i32 = 7;
pub const STOOLAP_ERR_UNSUPPORTED: i32 = 8;

/// The key a duplicate names: none, the PRIMARY KEY, or the n-th
/// table-level unique constraint of the CREATE TABLE
pub const STOOLAP_KEY_NONE: i32 = -1;
pub const STOOLAP_KEY_PRIMARY: i32 = -2;

use stoolap::core::Error;

thread_local! {
    /// The last error recorded with its value, by its message: its class
    /// comes from the value, not the wording
    static LAST: RefCell<Option<(String, (i32, i32))>> = const { RefCell::new(None) };
}

/// Notes `e`, which a handle has just recorded as its message
pub(crate) fn note_error(e: &Error) {
    let class = match e {
        Error::PrimaryKeyConstraint { .. } => (STOOLAP_ERR_DUP_KEY, STOOLAP_KEY_PRIMARY),
        Error::UniqueConstraint { index, .. } => (STOOLAP_ERR_DUP_KEY, unique_ordinal(index)),
        Error::ForeignKeyViolation { detail, .. } => {
            if detail.contains("still referenced by table") {
                (STOOLAP_ERR_ROW_IS_REFERENCED, STOOLAP_KEY_NONE)
            } else {
                (STOOLAP_ERR_NO_REFERENCED_ROW, STOOLAP_KEY_NONE)
            }
        }
        Error::TableNotFound(_) | Error::ViewNotFound(_) | Error::TableOrViewNotFound(_) => {
            (STOOLAP_ERR_NO_SUCH_TABLE, STOOLAP_KEY_NONE)
        }
        Error::TableAlreadyExists(_)
        | Error::IndexAlreadyExists(_)
        | Error::ViewAlreadyExists(_) => (STOOLAP_ERR_TABLE_EXISTS, STOOLAP_KEY_NONE),
        // The rest by their wording, as the plugin always read them
        _ => {
            LAST.with(|c| *c.borrow_mut() = None);
            return;
        }
    };
    LAST.with(|c| *c.borrow_mut() = Some((e.to_string(), class)));
}

/// The n of stoolap's "unique_<table>_<n>" index
fn unique_ordinal(index: &str) -> i32 {
    index
        .rsplit_once('_')
        .filter(|_| index.starts_with("unique_"))
        .and_then(|(_, n)| n.parse::<u16>().ok())
        .map_or(STOOLAP_KEY_NONE, i32::from)
}

/// The class of a stoolap error message, most specific first. PK / UNIQUE /
/// FK anchor at the start, since the FK message contains "does not exist"
fn class_of_text(m: &str) -> (i32, i32) {
    let has = |n: &str| m.contains(n);
    let starts = |n: &str| m.starts_with(n);
    let class = if starts("primary key constraint failed") {
        return (STOOLAP_ERR_DUP_KEY, STOOLAP_KEY_PRIMARY);
    } else if let Some(rest) = m.strip_prefix("unique constraint failed for index ") {
        let index = rest.split(" on column ").next().unwrap_or("");
        return (STOOLAP_ERR_DUP_KEY, unique_ordinal(index));
    } else if starts("unique constraint failed") {
        STOOLAP_ERR_DUP_KEY
    } else if has("still referenced by table") {
        // The parent side; the child side's "referenced row ... does not
        // exist" is the violation below
        STOOLAP_ERR_ROW_IS_REFERENCED
    } else if starts("foreign key constraint violation") {
        STOOLAP_ERR_NO_REFERENCED_ROW
    } else if starts("not null constraint failed") || starts("CHECK constraint failed") {
        STOOLAP_ERR_GENERIC
    } else if has("; retry the statement") {
        // A schema change, or a truncate or drop overtaking the view
        STOOLAP_ERR_DEADLOCK
    } else if has("' already exists") {
        STOOLAP_ERR_TABLE_EXISTS
    } else if starts("column '") || starts("index '") {
        // Their lookups end in "' not found" too
        STOOLAP_ERR_GENERIC
    } else if has("' not found") {
        STOOLAP_ERR_NO_SUCH_TABLE
    } else if has("uncommitted changes")
        || has("write conflict")
        || starts("failed to acquire lock")
        || starts("transaction aborted")
        || starts("database is locked")
    {
        STOOLAP_ERR_DEADLOCK
    } else if has("not supported") || has("unsupported") {
        STOOLAP_ERR_UNSUPPORTED
    } else if [
        "parse error",
        "division by zero",
        "type conversion error",
        "type error",
        "invalid argument",
        "value for column ",
        "IO error",
        "query cancelled",
        "transaction already",
        "transaction not started",
    ]
    .iter()
    .any(|p| starts(p))
    {
        STOOLAP_ERR_GENERIC
    } else {
        STOOLAP_ERR_UNKNOWN
    };
    (class, STOOLAP_KEY_NONE)
}

/// The class of the error `msg` (`STOOLAP_ERR_*`) and, for a duplicate,
/// the key it names in `key` (`STOOLAP_KEY_*` or an ordinal)
///
/// # Safety
///
/// `msg` must be a NUL-terminated string or NULL; `key` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn stoolap_error_class(msg: *const c_char, key: *mut i32) -> i32 {
    let text = if msg.is_null() {
        ""
    } else {
        std::ffi::CStr::from_ptr(msg).to_str().unwrap_or("")
    };
    let (class, k) = LAST
        .with(|c| {
            c.borrow()
                .as_ref()
                .filter(|(m, _)| m.as_str() == text)
                .map(|(_, class)| *class)
        })
        .unwrap_or_else(|| class_of_text(text));
    if !key.is_null() {
        *key = k;
    }
    class
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_classes() {
        let c = |m: &str| class_of_text(m);
        assert_eq!(
            c("primary key constraint failed with 3 already exists in this table"),
            (STOOLAP_ERR_DUP_KEY, STOOLAP_KEY_PRIMARY)
        );
        assert_eq!(c("unique constraint failed for index unique_t_on_x_2 on column a, b with value (1, 2)"), (STOOLAP_ERR_DUP_KEY, 2));
        assert_eq!(c("foreign key constraint violation: column 'p' ... — cannot delete row with id = 1 — still referenced by table 'c'"), (STOOLAP_ERR_ROW_IS_REFERENCED, STOOLAP_KEY_NONE));
        assert_eq!(c("foreign key constraint violation: column 'p' in table 'c' references 'p(id)' — referenced row does not exist").0, STOOLAP_ERR_NO_REFERENCED_ROW);
        assert_eq!(
            c("write conflict: row 3 was modified by another transaction").0,
            STOOLAP_ERR_DEADLOCK
        );
        assert_eq!(c("column 'x' not found").0, STOOLAP_ERR_GENERIC);
        assert_eq!(c("table 'x' not found").0, STOOLAP_ERR_NO_SUCH_TABLE);
        assert_eq!(
            c("invalid argument: row-pump UPDATE/DELETE ... is unsupported.").0,
            STOOLAP_ERR_UNSUPPORTED
        );
        assert_eq!(c("something new").0, STOOLAP_ERR_UNKNOWN);
        assert_eq!(unique_ordinal("unique_a_1b_12"), 12);
        assert_eq!(unique_ordinal("idx_x"), STOOLAP_KEY_NONE);
    }
}
