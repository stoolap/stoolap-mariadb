// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Tables from MariaDB's definitions. The plugin describes a table's
//! columns (with the stoolap type it stores each in) and keys; this builds
//! the stoolap CREATE TABLE and its indexes. MariaDB keeps no FOREIGN KEY
//! for an engine that does not claim them, so the plugin hands over the
//! ones its parser read, their parent tables under stoolap's names.

use std::sync::{Mutex, MutexGuard};

use stoolap::api::{Database, ParamVec};
use stoolap::core::schema::ROWID_NAME;
use stoolap::core::{Error, Result, Value};

use crate::session::Session;

/// Held by the DDL that creates a key naming a table, and by the rename a
/// copying ALTER swaps its table away with, around that rename's check:
/// no key comes to name the table between the check and the rename
static KEY_DDL: Mutex<()> = Mutex::new(());

fn key_ddl() -> MutexGuard<'static, ()> {
    KEY_DDL.lock().unwrap_or_else(|e| e.into_inner())
}

/// Refuses a copying ALTER of `table` when another table's key names it:
/// MariaDB swaps the copy in by renames, and a rename takes those keys to
/// the old table, which they would go on naming (stoolap#987)
fn refuse_referenced(session: &Session, table: &str) -> Result<()> {
    match session.referencing_tables(table).first() {
        Some((child, _, _)) => Err(Error::NotSupported(format!(
            "this ALTER TABLE copies the table, and a FOREIGN KEY of '{child}' names it: \
             the key would stay with the old table (stoolap#987)"
        ))),
        None => Ok(()),
    }
}

pub const STOOLAP_COL_INTEGER: i32 = 1;
pub const STOOLAP_COL_FLOAT: i32 = 2;
pub const STOOLAP_COL_TEXT: i32 = 3;

pub const STOOLAP_KEYDEF_PRIMARY: i32 = 0;
pub const STOOLAP_KEYDEF_UNIQUE: i32 = 1;
pub const STOOLAP_KEYDEF_INDEX: i32 = 2;

pub struct Column {
    pub name: String,
    pub kind: i32,
    pub nullable: bool,
    pub auto_increment: bool,
}

pub struct Key {
    pub name: String,
    pub columns: Vec<usize>,
    pub kind: i32,
}

pub fn quote(name: &str) -> String {
    format!("\"{}\"", name.replace('"', "\"\""))
}

/// What a column MariaDB names `_rowid_` (in any case) is called in
/// stoolap, which keeps that name for a row's identity: the name with a
/// character no MariaDB identifier holds, as `stoolap_column_name`
/// (src/stoolap_bridge.h) writes it
const ROWID_SUFFIX: char = '\u{F0000}';

/// Gives each column an older build stored as `_rowid_` the name the
/// plugin now writes for it. Stoolap opens such a table for DDL alone; the
/// plugin, which turns every MariaDB `_rowid_` into that name, reads it
/// again once renamed. A rename that fails leaves its table refusing reads,
/// and is reported.
pub fn rename_reserved_columns(db: &Database) {
    for schema in db.engine().get_all_schemas() {
        for column in &schema.columns {
            if !column.name.eq_ignore_ascii_case(ROWID_NAME) {
                continue;
            }
            let sql = format!(
                "ALTER TABLE {} RENAME COLUMN {} TO {}",
                quote(&schema.table_name),
                quote(&column.name),
                quote(&format!("{}{}", column.name, ROWID_SUFFIX))
            );
            match db.execute(&sql, ()) {
                Ok(_) => eprintln!(
                    "stoolap: renamed column {} of table {} to the name the \
                     plugin reads it by",
                    column.name, schema.table_name
                ),
                Err(e) => eprintln!(
                    "stoolap: cannot rename column {} of table {}: {e}",
                    column.name, schema.table_name
                ),
            }
        }
    }
}

/// The most bytes a table name takes in stoolap, which names a file after
/// it
const MAX_TABLE_NAME: usize = 255;

/// The bytes `c` takes in stoolap, which may keep it lowercased
fn stored_len(c: char) -> usize {
    c.len_utf8().max(c.to_lowercase().map(char::len_utf8).sum())
}

/// `name`, or when stoolap cannot hold it, its first characters, `_2` and
/// a hash of the whole lowercased (FNV-1a, 64 bits): stoolap matches table
/// names lowercased, so names in other letter case shorten to one it
/// matches too. The escaping never writes `_2`, so no name that fits meets
/// a shortened one; the hash must not change, a file-backed table keeps
/// its name.
pub fn fit_name(name: String) -> String {
    const TAIL: usize = 2 + 16;
    if name.chars().map(stored_len).sum::<usize>() <= MAX_TABLE_NAME {
        return name;
    }
    let mut out = String::with_capacity(MAX_TABLE_NAME);
    let mut len = 0;
    for c in name.chars() {
        len += stored_len(c);
        if len + TAIL > MAX_TABLE_NAME {
            break;
        }
        out.push(c);
    }
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for b in name.to_lowercase().bytes() {
        hash = (hash ^ b as u64).wrapping_mul(0x0100_0000_01b3);
    }
    out.push_str(&format!("_2{hash:016x}"));
    out
}

/// Creates `table` (stoolap's name) with its `foreign_keys` and secondary
/// indexes. Returns the indexes that failed, by name with the error, which
/// leave their queries on a full scan.
pub fn create_table(
    session: &Session,
    table: &str,
    columns: &[Column],
    keys: &[Key],
    foreign_keys: &[ForeignKey],
    if_not_exists: bool,
) -> Result<Vec<String>> {
    let column_name = |i: usize| -> Result<&str> {
        columns
            .get(i)
            .map(|c| c.name.as_str())
            .ok_or_else(|| Error::internal("key column out of range"))
    };
    let key_columns = |k: &Key| -> Result<String> {
        let names: Result<Vec<String>> = k
            .columns
            .iter()
            .map(|&i| column_name(i).map(quote))
            .collect();
        Ok(format!("({})", names?.join(", ")))
    };

    // A single-column PRIMARY KEY inline; a composite one first among the
    // table-level constraints, which stoolap numbers in order (the plugin
    // maps a duplicate's ordinal back to the key the same way)
    let primary = keys.iter().find(|k| k.kind == STOOLAP_KEYDEF_PRIMARY);
    let inline_pk = primary
        .filter(|k| k.columns.len() == 1)
        .map(|k| k.columns[0]);

    let mut sql = String::from("CREATE TABLE ");
    if if_not_exists {
        sql.push_str("IF NOT EXISTS ");
    }
    sql.push_str(&quote(table));
    sql.push_str(" (");
    for (i, c) in columns.iter().enumerate() {
        if i > 0 {
            sql.push_str(", ");
        }
        sql.push_str(&quote(&c.name));
        sql.push_str(match c.kind {
            STOOLAP_COL_INTEGER => " INTEGER",
            STOOLAP_COL_FLOAT => " FLOAT",
            STOOLAP_COL_TEXT => " TEXT",
            _ => return Err(Error::internal("unknown column type")),
        });
        if !c.nullable {
            sql.push_str(" NOT NULL");
        }
        if inline_pk == Some(i) {
            sql.push_str(" PRIMARY KEY");
        }
        if c.auto_increment {
            sql.push_str(" AUTO_INCREMENT");
        }
    }
    if let Some(pk) = primary.filter(|k| k.columns.len() > 1) {
        sql.push_str(", PRIMARY KEY ");
        sql.push_str(&key_columns(pk)?);
    }
    for k in keys.iter().filter(|k| k.kind == STOOLAP_KEYDEF_UNIQUE) {
        sql.push_str(", UNIQUE ");
        sql.push_str(&key_columns(k)?);
    }
    for fk in foreign_keys {
        foreign_key(fk, &mut sql)?;
    }
    sql.push(')');
    {
        let _held = (!foreign_keys.is_empty()).then(key_ddl);
        session.run(&sql, ParamVec::new())?;
    }

    // One stoolap index per non-unique key, over all of its parts
    // (stoolap serves leading-part equality and ranges from a composite
    // index too); without it the key MariaDB plans by reads a full scan
    let mut failed = Vec::new();
    for k in keys
        .iter()
        .filter(|k| k.kind == STOOLAP_KEYDEF_INDEX && !k.columns.is_empty())
    {
        let names = k
            .columns
            .iter()
            .map(|&i| column_name(i))
            .collect::<Result<Vec<_>>>()?;
        if let Err(e) = create_index(session, table, &k.name, &names) {
            failed.push(format!("{}: {}", k.name, e));
        }
    }
    Ok(failed)
}

/// The stoolap index of `table`'s non-unique key `key`
fn index_name(table: &str, key: &str) -> String {
    quote(&format!("{table}__{key}"))
}

/// Creates the index of `table`'s non-unique key `key` over `columns`
pub fn create_index(session: &Session, table: &str, key: &str, columns: &[&str]) -> Result<()> {
    let columns: Vec<String> = columns.iter().map(|c| quote(c)).collect();
    session.run(
        &format!(
            "CREATE INDEX IF NOT EXISTS {} ON {} ({})",
            index_name(table, key),
            quote(table),
            columns.join(", ")
        ),
        ParamVec::new(),
    )?;
    Ok(())
}

/// Drops the index of `table`'s non-unique key `key`, if it is there (a
/// dropped column takes its indexes with it)
pub fn drop_index(session: &Session, table: &str, key: &str) -> Result<()> {
    session.run(
        &format!(
            "DROP INDEX IF EXISTS {} ON {}",
            index_name(table, key),
            quote(table)
        ),
        ParamVec::new(),
    )?;
    Ok(())
}

/// `value` as an SQL literal, None when stoolap would not read it back as
/// it is: text holding a NUL, which ends a literal
fn literal(value: &Value) -> Option<String> {
    match value {
        Value::Null(_) => Some("NULL".into()),
        Value::Integer(i) => Some(i.to_string()),
        Value::Float(f) if f.is_finite() => Some(format!("{f:?}")),
        Value::Text(t) if !t.as_str().contains('\0') => Some(format!(
            "'{}'",
            t.as_str().replace('\\', "\\\\").replace('\'', "''")
        )),
        _ => None,
    }
}

/// Adds column `name` at the end of `table`, its rows holding `default`
/// (the column's value for a row MariaDB wrote before it). In place: the
/// rows are not copied
pub fn add_column(
    session: &Session,
    table: &str,
    name: &str,
    kind: i32,
    nullable: bool,
    default: &Value,
) -> Result<()> {
    let ty = match kind {
        STOOLAP_COL_INTEGER => "INTEGER",
        STOOLAP_COL_FLOAT => "FLOAT",
        STOOLAP_COL_TEXT => "TEXT",
        _ => return Err(Error::internal("unknown column type")),
    };
    let default = literal(default)
        .ok_or_else(|| Error::NotSupported(format!("the default of column '{name}'")))?;
    session.run(
        &format!(
            "ALTER TABLE {} ADD COLUMN {} {ty}{} DEFAULT {default}",
            quote(table),
            quote(name),
            if nullable { "" } else { " NOT NULL" }
        ),
        ParamVec::new(),
    )?;
    Ok(())
}

/// Drops column `name` of `table`. One a FOREIGN KEY of the table uses is
/// refused, as InnoDB refuses it (stoolap would drop the key with it); one
/// another table's key references, by stoolap
pub fn drop_column(session: &Session, table: &str, name: &str) -> Result<()> {
    let lower = name.to_lowercase();
    let used = session.with_table(table, |t| {
        Ok(t.schema()
            .foreign_keys
            .iter()
            .any(|fk| fk.column_name.to_lowercase() == lower))
    })?;
    if used {
        return Err(Error::NotSupported(format!(
            "column '{name}' is used by a FOREIGN KEY: it cannot be dropped"
        )));
    }
    session.run(
        &format!("ALTER TABLE {} DROP COLUMN {}", quote(table), quote(name)),
        ParamVec::new(),
    )?;
    Ok(())
}

/// Renames column `from` of `table` to `to`; its indexes and keys follow.
/// Names in other letter case alone name the column already
pub fn rename_column(session: &Session, table: &str, from: &str, to: &str) -> Result<()> {
    if from.to_lowercase() == to.to_lowercase() {
        return Ok(());
    }
    session.run(
        &format!(
            "ALTER TABLE {} RENAME COLUMN {} TO {}",
            quote(table),
            quote(from),
            quote(to)
        ),
        ParamVec::new(),
    )?;
    Ok(())
}

/// Drops `table` if it is there. A parent other tables' foreign keys name
/// is refused, as InnoDB refuses it, unless `check_fks` is false; the
/// children among `dropping` (DROP DATABASE: the database's tables) go
/// with it, dropped first, each with its own children among them: Stoolap
/// refuses to drop a table whose rows another table's rows reference.
pub fn drop_table(
    session: &Session,
    table: &str,
    check_fks: bool,
    dropping: &[String],
) -> Result<()> {
    let dropping: Vec<String> = dropping.iter().map(|t| t.to_lowercase()).collect();
    drop_tree(session, table, check_fks, &dropping, &mut Vec::new())
}

/// `drop_table` of `table`, `dropped` the tables on its way down (a chain
/// of keys may come back to one of them)
fn drop_tree(
    session: &Session,
    table: &str,
    check_fks: bool,
    dropping: &[String],
    dropped: &mut Vec<String>,
) -> Result<()> {
    dropped.push(table.to_lowercase());
    for (child, column, parent_column) in session.referencing_tables(table) {
        if dropped.contains(&child) {
            continue;
        }
        if dropping.contains(&child) {
            drop_tree(session, &child, check_fks, dropping, dropped)?;
        } else if check_fks {
            return Err(Error::foreign_key_violation(
                &child,
                column,
                table,
                parent_column,
                format!("cannot drop table '{table}': still referenced by table '{child}'"),
            ));
        }
    }
    session.run(
        &format!("DROP TABLE IF EXISTS {}", quote(table)),
        ParamVec::new(),
    )?;
    Ok(())
}

/// Renames `from` to `to`
/// Renames `from` to `to`. A copying ALTER's `swap` of its table away
/// (from any engine's copy) is refused when another table's key names it
pub fn rename_table(session: &Session, from: &str, to: &str, swap: bool) -> Result<()> {
    let _held = swap.then(key_ddl);
    if swap {
        refuse_referenced(session, from)?;
    }
    session.run(
        &format!("ALTER TABLE {} RENAME TO {}", quote(from), quote(to)),
        ParamVec::new(),
    )?;
    Ok(())
}

/// A FOREIGN KEY, as MariaDB's parser read it
pub struct ForeignKey {
    pub column: String,
    /// The parent table, by stoolap's name
    pub parent: String,
    /// None: the parent's primary key
    pub parent_column: Option<String>,
    /// `STOOLAP_FK_*`
    pub on_delete: i32,
    pub on_update: i32,
}

/// MariaDB's ON DELETE / ON UPDATE actions (enum_fk_option)
pub const STOOLAP_FK_UNDEF: i32 = 0;
pub const STOOLAP_FK_RESTRICT: i32 = 1;
pub const STOOLAP_FK_NO_ACTION: i32 = 2;
pub const STOOLAP_FK_CASCADE: i32 = 3;
pub const STOOLAP_FK_SET_NULL: i32 = 4;
pub const STOOLAP_FK_SET_DEFAULT: i32 = 5;

/// The foreign keys of `from`, the table a copying ALTER rebuilds, for its
/// copy: MariaDB keeps none to hand back. A column the ALTER renames
/// (`renames`, old name to new) goes by its new name; one it drops
/// (`dropped`) under a key is refused, whatever column takes its name. A
/// key naming `from` itself names `to`, the copy, by the column's new name.
/// None when `from` is not a stoolap table (ALTER ... ENGINE=STOOLAP).
/// Refused when another table's key names `from` (`refuse_referenced`),
/// before the rows are copied; the swap's rename asks again
pub fn kept_foreign_keys(
    session: &Session,
    from: &str,
    to: &str,
    renames: &[(String, String)],
    dropped: &[String],
) -> Result<Vec<ForeignKey>> {
    use stoolap::core::ForeignKeyAction;
    // Column names compare as MariaDB and stoolap compare them, by case
    // past ASCII too
    let same = |a: &str, b: &str| a.to_lowercase() == b.to_lowercase();
    refuse_referenced(session, from)?;
    let fks = match session.with_table(from, |t| Ok(t.schema().foreign_keys.clone())) {
        Ok(fks) => fks,
        Err(Error::TableNotFound(_)) => return Ok(Vec::new()),
        Err(e) => return Err(e),
    };
    let action = |a: ForeignKeyAction| match a {
        ForeignKeyAction::Restrict => STOOLAP_FK_RESTRICT,
        ForeignKeyAction::Cascade => STOOLAP_FK_CASCADE,
        ForeignKeyAction::SetNull => STOOLAP_FK_SET_NULL,
        ForeignKeyAction::NoAction => STOOLAP_FK_NO_ACTION,
    };
    fks.into_iter()
        .map(|fk| {
            let own = same(&fk.referenced_table, from);
            for column in [Some(&fk.column_name), own.then_some(&fk.referenced_column)]
                .into_iter()
                .flatten()
            {
                if dropped.iter().any(|d| same(d, column)) {
                    return Err(Error::NotSupported(format!(
                        "column '{}' is used by a FOREIGN KEY: it cannot be dropped",
                        column
                    )));
                }
            }
            let renamed = |name: String| {
                renames
                    .iter()
                    .find(|(old, _)| same(old, &name))
                    .map_or(name, |(_, new)| new.clone())
            };
            let (parent, parent_column) = if own {
                (to.to_string(), renamed(fk.referenced_column))
            } else {
                (fk.referenced_table, fk.referenced_column)
            };
            Ok(ForeignKey {
                column: renamed(fk.column_name),
                parent,
                parent_column: Some(parent_column),
                on_delete: action(fk.on_delete),
                on_update: action(fk.on_update),
            })
        })
        .collect()
}

/// Appends `fk` as a clause of its table's CREATE TABLE
fn foreign_key(fk: &ForeignKey, out: &mut String) -> Result<()> {
    out.push_str(", FOREIGN KEY (");
    out.push_str(&quote(&fk.column));
    out.push_str(") REFERENCES ");
    out.push_str(&quote(&fk.parent));
    if let Some(c) = &fk.parent_column {
        out.push_str(" (");
        out.push_str(&quote(c));
        out.push(')');
    }
    for (event, action) in [("DELETE", fk.on_delete), ("UPDATE", fk.on_update)] {
        let words = match action {
            STOOLAP_FK_UNDEF => continue,
            STOOLAP_FK_RESTRICT => "RESTRICT",
            STOOLAP_FK_NO_ACTION => "NO ACTION",
            STOOLAP_FK_CASCADE => "CASCADE",
            STOOLAP_FK_SET_NULL => "SET NULL",
            STOOLAP_FK_SET_DEFAULT => "SET DEFAULT",
            _ => return Err(Error::internal("unknown FOREIGN KEY action")),
        };
        out.push_str(" ON ");
        out.push_str(event);
        out.push(' ');
        out.push_str(words);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn foreign_key_clauses() {
        let fk = |parent: &str, parent_column: Option<&str>, on_delete, on_update| ForeignKey {
            column: "p\"id".into(),
            parent: parent.into(),
            parent_column: parent_column.map(Into::into),
            on_delete,
            on_update,
        };
        let sql = |k: ForeignKey| {
            let mut out = String::new();
            foreign_key(&k, &mut out).map(|_| out)
        };
        assert_eq!(
            sql(fk("d_1p", Some("id"), STOOLAP_FK_CASCADE, STOOLAP_FK_SET_NULL)).unwrap(),
            ", FOREIGN KEY (\"p\"\"id\") REFERENCES \"d_1p\" (\"id\") ON DELETE CASCADE ON UPDATE SET NULL"
        );
        assert_eq!(
            sql(fk("o_1p", None, STOOLAP_FK_UNDEF, STOOLAP_FK_NO_ACTION)).unwrap(),
            ", FOREIGN KEY (\"p\"\"id\") REFERENCES \"o_1p\" ON UPDATE NO ACTION"
        );
        // A key naming its own table
        assert_eq!(
            sql(fk("d_1c", Some("id"), STOOLAP_FK_UNDEF, STOOLAP_FK_UNDEF)).unwrap(),
            ", FOREIGN KEY (\"p\"\"id\") REFERENCES \"d_1c\" (\"id\")"
        );
        assert!(sql(fk("d_1p", Some("id"), 9, STOOLAP_FK_UNDEF)).is_err());
    }

    #[test]
    fn literals() {
        assert_eq!(
            literal(&Value::Integer(i64::MIN)).unwrap(),
            "-9223372036854775808"
        );
        assert_eq!(literal(&Value::Float(1e-7)).unwrap(), "1e-7");
        assert_eq!(
            literal(&Value::text("it's \\ ok")).unwrap(),
            "'it''s \\\\ ok'"
        );
        assert_eq!(literal(&Value::null_unknown()).unwrap(), "NULL");
        assert!(literal(&Value::text("a\0b")).is_none());
    }

    #[test]
    fn long_names() {
        let long = fit_name(format!("{}_1{}", "_0".repeat(64), "_0".repeat(64)));
        assert_eq!(long.len(), 255);
        assert!(long.starts_with("_0_0") && long.contains("_2"));
        assert_ne!(
            long,
            fit_name(format!("{}_1{}", "_0".repeat(64), "_0".repeat(63)))
        );
        let fits = "a".repeat(255);
        assert_eq!(fit_name(fits.clone()), fits);
        assert_eq!(fit_name("a".repeat(256)).len(), 255);
        assert_eq!(
            fit_name("a".repeat(256)).to_lowercase(),
            fit_name("A".repeat(256)).to_lowercase()
        );
    }
}
