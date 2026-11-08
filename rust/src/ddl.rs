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

use stoolap::api::ParamVec;
use stoolap::core::{Error, Result};

use crate::session::Session;

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
        foreign_key(fk, table, &mut sql)?;
    }
    sql.push(')');
    session.run(&sql, ParamVec::new())?;

    // One stoolap index per non-unique key, over all of its parts
    // (stoolap serves leading-part equality and ranges from a composite
    // index too); without it the key MariaDB plans by reads a full scan
    let mut failed = Vec::new();
    for k in keys
        .iter()
        .filter(|k| k.kind == STOOLAP_KEYDEF_INDEX && !k.columns.is_empty())
    {
        let index_sql = format!(
            "CREATE INDEX IF NOT EXISTS {} ON {} {}",
            quote(&format!("{}__{}", table, k.name)),
            quote(table),
            key_columns(k)?
        );
        if let Err(e) = session.run(&index_sql, ParamVec::new()) {
            failed.push(format!("{}: {}", k.name, e));
        }
    }
    Ok(failed)
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
pub fn rename_table(session: &Session, from: &str, to: &str) -> Result<()> {
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
/// (`dropped`) under a key is refused, whatever column takes its name.
/// None when `from` is not a stoolap table (ALTER ... ENGINE=STOOLAP).
pub fn kept_foreign_keys(
    session: &Session,
    from: &str,
    renames: &[(String, String)],
    dropped: &[String],
) -> Result<Vec<ForeignKey>> {
    use stoolap::core::ForeignKeyAction;
    // Column names compare as MariaDB and stoolap compare them, by case
    // past ASCII too
    let same = |a: &str, b: &str| a.to_lowercase() == b.to_lowercase();
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
            if dropped.iter().any(|d| same(d, &fk.column_name)) {
                return Err(Error::NotSupported(format!(
                    "column '{}' is used by a FOREIGN KEY: it cannot be dropped",
                    fk.column_name
                )));
            }
            let column = renames
                .iter()
                .find(|(old, _)| same(old, &fk.column_name))
                .map_or(fk.column_name, |(_, new)| new.clone());
            Ok(ForeignKey {
                column,
                parent: fk.referenced_table,
                parent_column: Some(fk.referenced_column),
                on_delete: action(fk.on_delete),
                on_update: action(fk.on_update),
            })
        })
        .collect()
}

/// Appends `fk`, a key of `table`, as a clause of its CREATE TABLE
fn foreign_key(fk: &ForeignKey, table: &str, out: &mut String) -> Result<()> {
    // Stoolap looks the parent up at CREATE, before this table exists
    if fk.parent == table {
        return Err(Error::NotSupported(format!(
            "self-referencing FOREIGN KEY on '{}': stoolap resolves the \
             parent at CREATE-time, before the table is registered.",
            fk.column
        )));
    }
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
            foreign_key(&k, "d_1c", &mut out).map(|_| out)
        };
        assert_eq!(
            sql(fk("d_1p", Some("id"), STOOLAP_FK_CASCADE, STOOLAP_FK_SET_NULL)).unwrap(),
            ", FOREIGN KEY (\"p\"\"id\") REFERENCES \"d_1p\" (\"id\") ON DELETE CASCADE ON UPDATE SET NULL"
        );
        assert_eq!(
            sql(fk("o_1p", None, STOOLAP_FK_UNDEF, STOOLAP_FK_NO_ACTION)).unwrap(),
            ", FOREIGN KEY (\"p\"\"id\") REFERENCES \"o_1p\" ON UPDATE NO ACTION"
        );
        assert!(sql(fk("d_1c", Some("id"), STOOLAP_FK_UNDEF, STOOLAP_FK_UNDEF)).is_err());
        assert!(sql(fk("d_1p", Some("id"), 9, STOOLAP_FK_UNDEF)).is_err());
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
