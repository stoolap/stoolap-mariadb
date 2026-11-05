// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! DDL FFI: tables from the plugin's descriptions (see `crate::ddl`).

use std::ffi::CStr;
use std::os::raw::c_char;
use std::panic;

use stoolap::core::{Error, Result};

use super::types::StoolapDB;
use super::{STOOLAP_ERROR, STOOLAP_OK};
use crate::ddl::{self, Column, Key};
use crate::session::Session;

/// The table was created, but some indexes were not (see
/// `stoolap_errmsg`): their queries full-scan
pub const STOOLAP_INDEX_FAILED: i32 = 3;

#[repr(C)]
pub struct StoolapColumnDef {
    pub name: *const c_char,
    /// `STOOLAP_COL_*`
    pub kind: i32,
    pub nullable: i32,
    pub auto_increment: i32,
}

#[repr(C)]
pub struct StoolapKeyDef {
    pub name: *const c_char,
    /// The key's columns, by position
    pub columns: *const i32,
    pub ncolumns: i32,
    /// `STOOLAP_KEYDEF_*`
    pub kind: i32,
}

unsafe fn text<'a>(p: *const c_char, what: &str) -> Result<&'a str> {
    if p.is_null() {
        return Err(Error::internal(format!("{} is NULL", what)));
    }
    CStr::from_ptr(p)
        .to_str()
        .map_err(|e| Error::internal(format!("invalid UTF-8 in {}: {}", what, e)))
}

unsafe fn slice<'a, T>(p: *const T, n: i32) -> Result<&'a [T]> {
    if n <= 0 {
        return Ok(&[]);
    }
    if p.is_null() {
        return Err(Error::internal("NULL array with a non-zero length"));
    }
    Ok(std::slice::from_raw_parts(p, n as usize))
}

/// Runs `f` on the handle's session, recording its error
unsafe fn on_session(
    db: *mut StoolapDB,
    what: &str,
    f: impl FnOnce(&Session) -> Result<i32>,
) -> i32 {
    let Some(handle) = db.as_mut() else {
        return STOOLAP_ERROR;
    };
    handle.last_error = None;
    let result = panic::catch_unwind(panic::AssertUnwindSafe(|| f(&handle.session)));
    match result {
        Ok(Ok(rc)) => rc,
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

/// Creates `table` (stoolap's name) from `columns` and `keys`, in
/// MariaDB's database `db_name`; `create_sql`, the statement's
/// `create_sql_len` bytes as the client sent them, may be NULL and is read
/// for FOREIGN KEY clauses. `STOOLAP_INDEX_FAILED`
/// when some secondary index was not created (listed in `stoolap_errmsg`).
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`; the strings NUL-terminated UTF-8; the
/// arrays hold their stated lengths.
#[no_mangle]
pub unsafe extern "C" fn stoolap_create_table(
    db: *mut StoolapDB,
    table: *const c_char,
    db_name: *const c_char,
    columns: *const StoolapColumnDef,
    ncolumns: i32,
    keys: *const StoolapKeyDef,
    nkeys: i32,
    create_sql: *const u8,
    create_sql_len: i64,
    if_not_exists: i32,
) -> i32 {
    let mut failed = Vec::new();
    let rc = on_session(db, "stoolap_create_table", |session| {
        let cols = slice(columns, ncolumns)?
            .iter()
            .map(|c| {
                Ok(Column {
                    name: text(c.name, "column name")?.to_string(),
                    kind: c.kind,
                    nullable: c.nullable != 0,
                    auto_increment: c.auto_increment != 0,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let key_list = slice(keys, nkeys)?
            .iter()
            .map(|k| {
                let columns = slice(k.columns, k.ncolumns)?
                    .iter()
                    .map(|&c| usize::try_from(c).map_err(|_| Error::internal("negative column")))
                    .collect::<Result<Vec<_>>>()?;
                Ok(Key {
                    name: text(k.name, "key name")?.to_string(),
                    columns,
                    kind: k.kind,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        let sql = if create_sql.is_null() {
            None
        } else {
            let len = usize::try_from(create_sql_len).map_err(|_| Error::internal("bad length"))?;
            Some(std::slice::from_raw_parts(create_sql, len))
        };
        failed = ddl::create_table(
            session,
            text(table, "table name")?,
            text(db_name, "database name")?,
            &cols,
            &key_list,
            sql,
            if_not_exists != 0,
        )?;
        Ok(STOOLAP_OK)
    });
    if rc == STOOLAP_OK && !failed.is_empty() {
        if let Some(handle) = db.as_mut() {
            handle.set_error(&failed.join("; "));
        }
        return STOOLAP_INDEX_FAILED;
    }
    rc
}

/// Writes `name` (`len` bytes) as a stoolap table name, shortened as
/// `ddl::fit_name` does, to `out` (STOOLAP_MAX_TABLE_NAME bytes); returns
/// its length, 0 when `name` fits as it is or is not UTF-8.
///
/// # Safety
///
/// `name` must hold `len` bytes and `out` STOOLAP_MAX_TABLE_NAME.
#[no_mangle]
pub unsafe extern "C" fn stoolap_fit_table_name(
    name: *const u8,
    len: usize,
    out: *mut u8,
) -> usize {
    if name.is_null() || out.is_null() {
        return 0;
    }
    let Ok(s) = std::str::from_utf8(std::slice::from_raw_parts(name, len)) else {
        return 0;
    };
    let fit = ddl::fit_name(s.to_string());
    if fit.len() == len {
        return 0;
    }
    std::ptr::copy_nonoverlapping(fit.as_ptr(), out, fit.len());
    fit.len()
}

/// Drops `table` if it is there.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, `table` NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn stoolap_drop_table(db: *mut StoolapDB, table: *const c_char) -> i32 {
    on_session(db, "stoolap_drop_table", |session| {
        ddl::drop_table(session, text(table, "table name")?)?;
        Ok(STOOLAP_OK)
    })
}

/// Renames table `from` to `to`.
///
/// # Safety
///
/// As `stoolap_drop_table`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rename_table(
    db: *mut StoolapDB,
    from: *const c_char,
    to: *const c_char,
) -> i32 {
    on_session(db, "stoolap_rename_table", |session| {
        ddl::rename_table(session, text(from, "table name")?, text(to, "table name")?)?;
        Ok(STOOLAP_OK)
    })
}
