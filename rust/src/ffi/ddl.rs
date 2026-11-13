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

use super::types::{StoolapDB, StoolapValue};
use super::value::ffi_value_to_rust;
use super::{STOOLAP_ERROR, STOOLAP_OK};
use crate::ddl::{self, Column, ForeignKey, Key};
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

#[repr(C)]
pub struct StoolapForeignKeyDef {
    /// The child column
    pub column: *const c_char,
    /// The parent table, by stoolap's name
    pub parent: *const c_char,
    /// NULL: the parent's primary key
    pub parent_column: *const c_char,
    /// `STOOLAP_FK_*`
    pub on_delete: i32,
    pub on_update: i32,
}

/// A column an ALTER renames
#[repr(C)]
pub struct StoolapColumnRename {
    pub from: *const c_char,
    pub to: *const c_char,
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

/// Creates `table` (stoolap's name) from `columns`, `keys` and
/// `foreign_keys`, and, when `keep_from` is not NULL, the foreign keys of
/// that table, which a copying ALTER rebuilds into this one (`renames`,
/// the columns it renames; `dropped`, those it drops).
/// `STOOLAP_INDEX_FAILED`
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
    columns: *const StoolapColumnDef,
    ncolumns: i32,
    keys: *const StoolapKeyDef,
    nkeys: i32,
    foreign_keys: *const StoolapForeignKeyDef,
    nforeign_keys: i32,
    keep_from: *const c_char,
    renames: *const StoolapColumnRename,
    nrenames: i32,
    dropped: *const *const c_char,
    ndropped: i32,
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
        let mut fks = if keep_from.is_null() {
            Vec::new()
        } else {
            let renames = slice(renames, nrenames)?
                .iter()
                .map(|r| {
                    Ok((
                        text(r.from, "renamed column")?.to_string(),
                        text(r.to, "renamed column")?.to_string(),
                    ))
                })
                .collect::<Result<Vec<_>>>()?;
            let dropped = slice(dropped, ndropped)?
                .iter()
                .map(|&d| Ok(text(d, "dropped column")?.to_string()))
                .collect::<Result<Vec<_>>>()?;
            ddl::kept_foreign_keys(
                session,
                text(keep_from, "altered table")?,
                text(table, "table name")?,
                &renames,
                &dropped,
            )?
        };
        let new_fks = slice(foreign_keys, nforeign_keys)?
            .iter()
            .map(|f| {
                // A key an ALTER adds naming the altered table names its copy
                let mut parent = text(f.parent, "foreign key parent")?;
                if !keep_from.is_null()
                    && parent.to_lowercase() == text(keep_from, "altered table")?.to_lowercase()
                {
                    parent = text(table, "table name")?;
                }
                Ok(ForeignKey {
                    column: text(f.column, "foreign key column")?.to_string(),
                    parent: parent.to_string(),
                    parent_column: if f.parent_column.is_null() {
                        None
                    } else {
                        Some(text(f.parent_column, "foreign key parent column")?.to_string())
                    },
                    on_delete: f.on_delete,
                    on_update: f.on_update,
                })
            })
            .collect::<Result<Vec<_>>>()?;
        fks.extend(new_fks);
        failed = ddl::create_table(
            session,
            text(table, "table name")?,
            &cols,
            &key_list,
            &fks,
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

/// Drops `table` if it is there: a parent other tables' foreign keys name
/// is refused when `check_fks`, but for the children among `dropping`
/// (DROP DATABASE: the database's tables), which are dropped first.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, the strings NUL-terminated UTF-8,
/// `dropping` `ndropping` of them.
#[no_mangle]
pub unsafe extern "C" fn stoolap_drop_table(
    db: *mut StoolapDB,
    table: *const c_char,
    check_fks: i32,
    dropping: *const *const c_char,
    ndropping: i32,
) -> i32 {
    on_session(db, "stoolap_drop_table", |session| {
        let dropping = slice(dropping, ndropping)?
            .iter()
            .map(|&t| Ok(text(t, "dropped table")?.to_string()))
            .collect::<Result<Vec<_>>>()?;
        ddl::drop_table(
            session,
            text(table, "table name")?,
            check_fks != 0,
            &dropping,
        )?;
        Ok(STOOLAP_OK)
    })
}

/// Renames table `from` to `to`; `swap` when a copying ALTER renames its
/// table away (refused when another table's key names it).
///
/// # Safety
///
/// As `stoolap_drop_table`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_rename_table(
    db: *mut StoolapDB,
    from: *const c_char,
    to: *const c_char,
    swap: i32,
) -> i32 {
    on_session(db, "stoolap_rename_table", |session| {
        ddl::rename_table(
            session,
            text(from, "table name")?,
            text(to, "table name")?,
            swap != 0,
        )?;
        Ok(STOOLAP_OK)
    })
}

/// One change an in-place ALTER makes to a table's columns: `STOOLAP_ALTER_*`
pub const STOOLAP_ALTER_ADD_COLUMN: i32 = 1;
pub const STOOLAP_ALTER_DROP_COLUMN: i32 = 2;
pub const STOOLAP_ALTER_RENAME_COLUMN: i32 = 3;

/// Changes a column of `table` in place: adds `name` at the end (`kind`,
/// `nullable`, its rows holding `default`), drops it, or renames it to
/// `new_name`, as `op` says.
///
/// # Safety
///
/// `db` must be a valid `StoolapDB`, the strings NUL-terminated UTF-8
/// (`new_name` for a rename), `default` a valid value for an add.
#[no_mangle]
pub unsafe extern "C" fn stoolap_alter_column(
    db: *mut StoolapDB,
    table: *const c_char,
    op: i32,
    name: *const c_char,
    new_name: *const c_char,
    kind: i32,
    nullable: i32,
    default: *const StoolapValue,
) -> i32 {
    on_session(db, "stoolap_alter_column", |session| {
        let table = text(table, "table name")?;
        let name = text(name, "column name")?;
        match op {
            STOOLAP_ALTER_ADD_COLUMN => {
                let default = default
                    .as_ref()
                    .ok_or_else(|| Error::internal("no default"))?;
                ddl::add_column(
                    session,
                    table,
                    name,
                    kind,
                    nullable != 0,
                    &ffi_value_to_rust(default)?,
                )?
            }
            STOOLAP_ALTER_DROP_COLUMN => ddl::drop_column(session, table, name)?,
            STOOLAP_ALTER_RENAME_COLUMN => {
                ddl::rename_column(session, table, name, text(new_name, "column name")?)?
            }
            _ => return Err(Error::internal("unknown column change")),
        }
        Ok(STOOLAP_OK)
    })
}

/// Creates the index of `table`'s non-unique key `key` over `columns`
/// (`create`), or drops the one over them.
///
/// # Safety
///
/// As `stoolap_drop_table`, `columns` `ncolumns` strings.
#[no_mangle]
pub unsafe extern "C" fn stoolap_alter_index(
    db: *mut StoolapDB,
    table: *const c_char,
    key: *const c_char,
    create: i32,
    columns: *const *const c_char,
    ncolumns: i32,
) -> i32 {
    on_session(db, "stoolap_alter_index", |session| {
        let table = text(table, "table name")?;
        let columns = slice(columns, ncolumns)?
            .iter()
            .map(|&c| text(c, "key column"))
            .collect::<Result<Vec<_>>>()?;
        if create != 0 {
            ddl::create_index(session, table, text(key, "key name")?, &columns)?;
        } else {
            ddl::drop_index(session, table, &columns)?;
        }
        Ok(STOOLAP_OK)
    })
}

/// Sets `table`'s id counter so its next AUTO_INCREMENT id is `next`, or
/// past its largest row id (`ddl::set_auto_increment`).
///
/// # Safety
///
/// As `stoolap_drop_table`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_auto_increment_set(
    db: *mut StoolapDB,
    table: *const c_char,
    next: u64,
) -> i32 {
    on_session(db, "stoolap_auto_increment_set", |session| {
        ddl::set_auto_increment(session, text(table, "table name")?, next)?;
        Ok(STOOLAP_OK)
    })
}

/// The next AUTO_INCREMENT id `table`'s counter gives, in `*next`.
///
/// # Safety
///
/// As `stoolap_drop_table`; `next` a valid pointer.
#[no_mangle]
pub unsafe extern "C" fn stoolap_auto_increment_next(
    db: *mut StoolapDB,
    table: *const c_char,
    next: *mut u64,
) -> i32 {
    on_session(db, "stoolap_auto_increment_next", |session| {
        let handed_out = session.auto_increment_counter(text(table, "table name")?)?;
        if let Some(next) = next.as_mut() {
            *next = handed_out.max(0) as u64 + 1;
        }
        Ok(STOOLAP_OK)
    })
}

/// Raises `table`'s id counter to `id`, the explicit id of a row the
/// plugin buffers (`Session::note_auto_increment`).
///
/// # Safety
///
/// As `stoolap_drop_table`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_auto_increment_note(
    db: *mut StoolapDB,
    table: *const c_char,
    id: u64,
) -> i32 {
    on_session(db, "stoolap_auto_increment_note", |session| {
        session.note_auto_increment(text(table, "table name")?, id)?;
        Ok(STOOLAP_OK)
    })
}

/// Takes `*count` AUTO_INCREMENT ids of `table`, `step` apart, each
/// `offset` modulo `step`; the first in `*first` and how many in `*count`
/// (`Session::reserve_auto_increment`).
///
/// # Safety
///
/// As `stoolap_drop_table`; `count` and `first` valid pointers.
#[no_mangle]
pub unsafe extern "C" fn stoolap_auto_increment_reserve(
    db: *mut StoolapDB,
    table: *const c_char,
    count: *mut u64,
    step: u64,
    offset: u64,
    first: *mut u64,
) -> i32 {
    on_session(db, "stoolap_auto_increment_reserve", |session| {
        let wanted = count.as_ref().copied().unwrap_or(1);
        let (id, taken) =
            session.reserve_auto_increment(text(table, "table name")?, wanted, step, offset)?;
        if let Some(first) = first.as_mut() {
            *first = id;
        }
        if let Some(count) = count.as_mut() {
            *count = taken;
        }
        Ok(STOOLAP_OK)
    })
}

/// The version of `table`'s definition (`ddl::table_version`); 0 when it
/// cannot be read.
///
/// # Safety
///
/// As `stoolap_drop_table`.
#[no_mangle]
pub unsafe extern "C" fn stoolap_table_version(db: *mut StoolapDB, table: *const c_char) -> u64 {
    let mut version = 0;
    on_session(db, "stoolap_table_version", |session| {
        version = ddl::table_version(session, text(table, "table name")?)?;
        Ok(STOOLAP_OK)
    });
    version
}
