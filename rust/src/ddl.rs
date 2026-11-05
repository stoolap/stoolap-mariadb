// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Tables from MariaDB's definitions. The plugin describes a table's
//! columns (with the stoolap type it stores each in) and keys; this builds
//! the stoolap CREATE TABLE and its indexes. MariaDB strips FOREIGN KEY
//! clauses from what it hands an engine that does not claim them, so they
//! are read back from the statement's own text, their parent tables
//! renamed to stoolap's names.

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
/// a hash of the whole (FNV-1a, 64 bits). The escaping never writes `_2`,
/// so no name that fits meets a shortened one; the hash must not change,
/// a file-backed table keeps its name.
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
    for b in name.bytes() {
        hash = (hash ^ b as u64).wrapping_mul(0x0100_0000_01b3);
    }
    out.push_str(&format!("_2{hash:016x}"));
    out
}

/// Stoolap's table name for MariaDB's (db, table): `_` as `_0` and `/` as
/// `_1` in each, joined by `_1`, so distinct pairs never meet; fit_name
/// shortens a long one
pub fn flat_name(db: &str, table: &str) -> String {
    let escape = |s: &str, out: &mut String| {
        for c in s.chars() {
            match c {
                '_' => out.push_str("_0"),
                '/' => out.push_str("_1"),
                c => out.push(c),
            }
        }
    };
    let mut out = String::with_capacity(db.len() + table.len() + 4);
    if !db.is_empty() {
        escape(db, &mut out);
        out.push_str("_1");
    }
    escape(table, &mut out);
    fit_name(out)
}

/// Creates `table` (stoolap's name; `db` the MariaDB database it is in)
/// and its secondary indexes; `create_sql` is the statement's text, read
/// for FOREIGN KEY clauses. Returns the indexes that failed, by name with
/// the error, which leave their queries on a full scan.
pub fn create_table(
    session: &Session,
    table: &str,
    db: &str,
    columns: &[Column],
    keys: &[Key],
    create_sql: Option<&[u8]>,
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
    if let Some(text) = create_sql {
        foreign_keys(text, db, table, &mut sql)?;
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

/// Drops `table` if it is there
pub fn drop_table(session: &Session, table: &str) -> Result<()> {
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

// -------------------------------------------------------------------------
// FOREIGN KEY clauses from MariaDB's CREATE TABLE text
// -------------------------------------------------------------------------

fn is_word(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_'
}

/// The comment at `p` and its end, as MariaDB's lexer skips them between
/// tokens: /* ... */ (not /*! ... */ or /*M! ... */, whose text it runs),
/// # and -- to the end of the line
fn comment_end(s: &[u8], p: usize) -> Option<usize> {
    let rest = &s[p..];
    if rest.starts_with(b"/*") && !rest.starts_with(b"/*!") && !rest.starts_with(b"/*M!") {
        return Some(
            s[p + 2..]
                .windows(2)
                .position(|w| w == b"*/")
                .map_or(s.len(), |at| p + 2 + at + 2),
        );
    }
    if rest.starts_with(b"#")
        || (rest.starts_with(b"--") && rest.get(2).is_none_or(|c| c.is_ascii_whitespace()))
    {
        return Some(
            s[p..]
                .iter()
                .position(|&c| c == b'\n')
                .map_or(s.len(), |at| p + at + 1),
        );
    }
    None
}

/// Past the white space and comments at `p`
fn skip_ws(s: &[u8], mut p: usize) -> usize {
    while p < s.len() {
        if s[p].is_ascii_whitespace() {
            p += 1;
        } else if let Some(end) = comment_end(s, p) {
            p = end;
        } else {
            break;
        }
    }
    p
}

/// `word` at `p`, any case, a word of its own: not part of an identifier
/// (x$FOREIGN) nor a qualified name's part (d.foreign). The position
/// after it
fn keyword(s: &[u8], p: usize, word: &str) -> Option<usize> {
    let end = p + word.len();
    if end > s.len() || !s[p..end].eq_ignore_ascii_case(word.as_bytes()) {
        return None;
    }
    if p > 0 && (is_ident_byte(s[p - 1]) || s[p - 1] == b'.') {
        return None;
    }
    if end < s.len() && is_ident_byte(s[end]) {
        return None;
    }
    Some(end)
}

/// A byte of a bare identifier: MariaDB's [0-9a-zA-Z$_] and the bytes of
/// a character past ASCII
fn is_ident_byte(c: u8) -> bool {
    is_word(c) || c == b'$' || c >= 0x80
}

/// An identifier at `p`, as its bytes: `quoted` or "quoted" (ANSI_QUOTES,
/// where the plain sql_mode would not have parsed the statement), a
/// doubled quote standing for one, or bare
fn ident(s: &[u8], mut p: usize) -> Option<(Vec<u8>, usize)> {
    if p >= s.len() {
        return None;
    }
    let q = s[p];
    if q == b'`' || q == b'"' {
        let mut out = Vec::new();
        p += 1;
        while p < s.len() {
            if s[p] == q {
                if p + 1 < s.len() && s[p + 1] == q {
                    out.push(q);
                    p += 2;
                    continue;
                }
                return Some((out, p + 1));
            }
            out.push(s[p]);
            p += 1;
        }
        return None;
    }
    let start = p;
    while p < s.len() && is_ident_byte(s[p]) {
        p += 1;
    }
    (p > start).then(|| (s[start..p].to_vec(), p))
}

/// Whether the word before `at` is DROP: ALTER TABLE ... DROP FOREIGN KEY
fn after_drop(s: &[u8], at: usize) -> bool {
    let mut end = at;
    while end > 0 && s[end - 1].is_ascii_whitespace() {
        end -= 1;
    }
    end >= 4 && s[end - 4..end].eq_ignore_ascii_case(b"DROP") && (end == 4 || !is_word(s[end - 5]))
}

/// The bytes inside the parentheses at `p`, and the position after them
fn paren_list(s: &[u8], p: usize) -> Option<(&[u8], usize)> {
    if p >= s.len() || s[p] != b'(' {
        return None;
    }
    let mut depth = 0;
    for (i, &c) in s.iter().enumerate().skip(p) {
        match c {
            b'(' => depth += 1,
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return Some((&s[p + 1..i], i + 1));
                }
            }
            _ => {}
        }
    }
    None
}

/// The FOREIGN KEY clause at `at`, rewritten into `out`: its end, or None
/// when it does not parse (the scan goes on past it)
fn foreign_key(
    s: &[u8],
    at: usize,
    db: &str,
    table: &str,
    out: &mut String,
) -> Result<Option<usize>> {
    type Clause<'a> = (&'a [u8], Vec<u8>, Vec<u8>, &'a [u8], Vec<u8>, usize);
    let parse = || -> Option<Clause<'_>> {
        let mut p = keyword(s, at, "FOREIGN")?;
        p = keyword(s, skip_ws(s, p), "KEY")?;
        // An index name before the column list
        p = skip_ws(s, p);
        if p < s.len() && s[p] != b'(' {
            p = skip_ws(s, ident(s, p)?.1);
        }
        let (child, np) = paren_list(s, p)?;
        p = keyword(s, skip_ws(s, np), "REFERENCES")?;
        p = skip_ws(s, p);
        let (mut parent_table, np) = ident(s, p)?;
        p = np;
        let mut parent_db = Vec::new();
        if p < s.len() && s[p] == b'.' {
            parent_db = parent_table;
            let (t, np) = ident(s, p + 1)?;
            parent_table = t;
            p = np;
        }
        p = skip_ws(s, p);
        let mut parent_columns: &[u8] = &[];
        if p < s.len() && s[p] == b'(' {
            let (cols, np) = paren_list(s, p)?;
            parent_columns = cols;
            p = skip_ws(s, np);
        }
        // ON DELETE / ON UPDATE actions, verbatim
        let mut actions = Vec::new();
        for _ in 0..2 {
            let Some(after_on) = keyword(s, p, "ON") else {
                break;
            };
            let kw = skip_ws(s, after_on);
            let Some(after_event) = keyword(s, kw, "DELETE").or_else(|| keyword(s, kw, "UPDATE"))
            else {
                break;
            };
            let action = skip_ws(s, after_event);
            let mut end = action;
            while end < s.len() && (s[end].is_ascii_alphabetic() || s[end] == b' ') {
                end += 1;
            }
            while end > action && s[end - 1].is_ascii_whitespace() {
                end -= 1;
            }
            // Rebuilt from its words: a comment between them stays out of
            // stoolap's SQL
            if !actions.is_empty() {
                actions.push(b' ');
            }
            actions.extend_from_slice(b"ON ");
            actions.extend_from_slice(&s[kw..after_event]);
            actions.push(b' ');
            actions.extend_from_slice(&s[action..end]);
            p = skip_ws(s, end);
        }
        Some((child, parent_db, parent_table, parent_columns, actions, p))
    };
    let Some((child, parent_db, parent_table, parent_columns, actions, end)) = parse() else {
        // FOREIGN not followed by KEY is no clause; DROP FOREIGN KEY names
        // a key to drop; anything else is a clause that would be lost, and
        // with it the key
        let is_clause = keyword(s, at, "FOREIGN")
            .and_then(|p| keyword(s, skip_ws(s, p), "KEY"))
            .is_some();
        if !is_clause || after_drop(s, at) {
            return Ok(None);
        }
        return Err(Error::NotSupported(
            "a FOREIGN KEY clause the plugin cannot read: write it as FOREIGN KEY \
             [name] (column) REFERENCES table (column)"
                .into(),
        ));
    };
    // The names as written, in the client's character set: as UTF-8 only,
    // since a lossy one could name another column ('\u{FFFD}' is one)
    let utf8 = |b: &[u8]| -> Result<String> {
        String::from_utf8(b.to_vec()).map_err(|_| {
            Error::NotSupported(
                "FOREIGN KEY names that are not UTF-8: create the table from a utf8mb4 \
                 connection"
                    .into(),
            )
        })
    };
    let (child, parent_db, parent_table, parent_columns, actions) = (
        utf8(child)?,
        utf8(&parent_db)?,
        utf8(&parent_table)?,
        utf8(parent_columns)?,
        utf8(&actions)?,
    );

    // Stoolap's foreign keys are single-column: dropping a composite one
    // would let the CREATE succeed and accept violating rows unseen
    if child.contains(',') || parent_columns.contains(',') {
        return Err(Error::NotSupported(format!(
            "composite FOREIGN KEY ({}): stoolap enforces single-column \
             foreign keys only.",
            child
        )));
    }
    // Stoolap's name for the parent comes from MariaDB's file name for it,
    // which encodes every character past [0-9A-Za-z_] (p$1 is p@00241)
    let plain = |n: &str| n.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_');
    let parent_db = if parent_db.is_empty() {
        db.to_string()
    } else {
        parent_db
    };
    if !plain(&parent_table) || !plain(&parent_db) {
        return Err(Error::NotSupported(format!(
            "FOREIGN KEY parent '{}': the plugin names a parent table only \
             when its database and table names are letters, digits and _",
            parent_table
        )));
    }
    let parent = flat_name(&parent_db, &parent_table);
    // Stoolap looks the parent up at CREATE, before this table exists
    if parent == table {
        return Err(Error::NotSupported(format!(
            "self-referencing FOREIGN KEY on '{}': stoolap resolves the \
             parent at CREATE-time, before the table is registered.",
            table
        )));
    }
    out.push_str(", FOREIGN KEY (");
    out.push_str(&child);
    out.push_str(") REFERENCES ");
    out.push_str(&quote(&parent));
    if !parent_columns.is_empty() {
        out.push_str(" (");
        out.push_str(&parent_columns);
        out.push(')');
    }
    if !actions.is_empty() {
        out.push(' ');
        out.push_str(&actions);
    }
    Ok(Some(end))
}

/// Appends every FOREIGN KEY clause of `sql` (outside quotes) to `out`,
/// each led by ", ". `sql` is the statement's bytes as the client sent
/// them: any character set, a NUL inside a literal
fn foreign_keys(s: &[u8], db: &str, table: &str, out: &mut String) -> Result<()> {
    let mut i = 0;
    while i < s.len() {
        if let Some(end) = comment_end(s, i) {
            i = end;
            continue;
        }
        match s[i] {
            q @ (b'\'' | b'"' | b'`') => {
                i += 1;
                while i < s.len() && s[i] != q {
                    i += if s[i] == b'\\' && i + 1 < s.len() {
                        2
                    } else {
                        1
                    };
                }
                i += 1;
            }
            _ if keyword(s, i, "FOREIGN").is_some() => {
                i = foreign_key(s, i, db, table, out)?.unwrap_or(i + 1);
            }
            _ => i += 1,
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fks(sql: &str, table: &str) -> Result<String> {
        let mut out = String::new();
        foreign_keys(sql.as_bytes(), "d", table, &mut out).map(|_| out)
    }

    #[test]
    fn foreign_key_clauses() {
        assert_eq!(flat_name("a_b", "c/d"), "a_0b_1c_1d");
        let long = flat_name(&"_".repeat(64), &"_".repeat(64));
        assert_eq!(long.len(), 255);
        assert!(long.starts_with("_0_0") && long.contains("_2"));
        assert_ne!(long, flat_name(&"_".repeat(64), &"_".repeat(63)));
        let fits = "a".repeat(255);
        assert_eq!(fit_name(fits.clone()), fits);
        assert_eq!(fit_name("a".repeat(256)).len(), 255);
        assert_eq!(
            fks(
                "CREATE TABLE c (id INT, pid INT, CONSTRAINT f FOREIGN KEY fk (pid) REFERENCES `p` (id) ON DELETE CASCADE ON UPDATE SET NULL)",
                "d_1c"
            )
            .unwrap(),
            ", FOREIGN KEY (pid) REFERENCES \"d_1p\" (id) ON DELETE CASCADE ON UPDATE SET NULL"
        );
        assert_eq!(
            fks(
                "CREATE TABLE c (pid INT, FOREIGN KEY (pid) REFERENCES o.p(id))",
                "d_1c"
            )
            .unwrap(),
            ", FOREIGN KEY (pid) REFERENCES \"o_1p\" (id)"
        );
        assert_eq!(
            fks(
                "CREATE TABLE c (v VARCHAR(9) DEFAULT 'FOREIGN KEY (x) REFERENCES y')",
                "d_1c"
            )
            .unwrap(),
            ""
        );
        assert!(fks(
            "CREATE TABLE c (a INT, b INT, FOREIGN KEY (a, b) REFERENCES p (x, y))",
            "d_1c"
        )
        .is_err());
        assert!(fks(
            "CREATE TABLE c (a INT, FOREIGN KEY (a) REFERENCES c (id))",
            "d_1c"
        )
        .is_err());
        // A NUL inside a literal, and bytes that are not UTF-8 (a latin1
        // comment), do not stop the scan
        let mut out = String::new();
        foreign_keys(
            b"CREATE TABLE c (b VARBINARY(4) DEFAULT 'a\0b', pid INT, FOREIGN KEY (pid) REFERENCES p (id)) COMMENT='caf\xe9'",
            "d",
            "d_1c",
            &mut out,
        )
        .unwrap();
        assert_eq!(out, ", FOREIGN KEY (pid) REFERENCES \"d_1p\" (id)");
        // ANSI_QUOTES
        assert_eq!(
            fks(
                "CREATE TABLE c (pid INT, FOREIGN KEY (pid) REFERENCES \"p\" (id))",
                "d_1c"
            )
            .unwrap(),
            ", FOREIGN KEY (pid) REFERENCES \"d_1p\" (id)"
        );
        // A parent whose MariaDB file name is encoded (p$1 is p@00241) is
        // refused rather than named wrong
        for parent in ["p$1", "`a``b`", "p\u{e9}"] {
            let sql =
                format!("CREATE TABLE c (pid INT, FOREIGN KEY (pid) REFERENCES {parent} (id))");
            assert!(fks(&sql, "d_1c").is_err(), "{parent}");
        }
        // FOREIGN in a name is no clause
        for sql in [
            "CREATE TABLE c (x$FOREIGN INT, y\u{e9}foreign INT)",
            "CREATE TABLE d.foreign (id INT)",
            "CREATE TABLE c (foreign_id INT)",
        ] {
            assert_eq!(fks(sql, "d_1c").unwrap(), "", "{sql}");
        }
        // Comments between the tokens of a clause, as MariaDB's lexer skips them
        assert_eq!(
            fks(
                "CREATE TABLE c (pid INT, FOREIGN /* a */ KEY # b\n (pid) -- c\n REFERENCES /*d*/ p (id) ON /* e */ DELETE CASCADE)",
                "d_1c"
            )
            .unwrap(),
            ", FOREIGN KEY (pid) REFERENCES \"d_1p\" (id) ON DELETE CASCADE"
        );
        // So is the current database's
        let mut out = String::new();
        assert!(foreign_keys(
            b"CREATE TABLE c (pid INT, FOREIGN KEY (pid) REFERENCES p (id))",
            "my-db",
            "my@002ddb_1c",
            &mut out,
        )
        .is_err());
        // DROP FOREIGN KEY names a key, comments are skipped, /*! */ runs
        assert_eq!(
            fks(
                "ALTER TABLE c DROP FOREIGN KEY fk_1, ADD COLUMN x INT",
                "d_1c"
            )
            .unwrap(),
            ""
        );
        assert_eq!(
            fks(
                "CREATE TABLE c (pid INT /* FOREIGN KEY later */, x INT) -- FOREIGN KEY\n",
                "d_1c"
            )
            .unwrap(),
            ""
        );
        assert_eq!(
            fks(
                "CREATE TABLE c (pid INT /*! , FOREIGN KEY (pid) REFERENCES p (id) */)",
                "d_1c"
            )
            .unwrap(),
            ", FOREIGN KEY (pid) REFERENCES \"d_1p\" (id)"
        );
        // A clause that does not read is refused, not dropped with its key
        assert!(fks(
            "CREATE TABLE c (pid INT, FOREIGN KEY (pid) REFERENCES (id))",
            "d_1c"
        )
        .is_err());
        // A name in another character set is refused, not read lossily
        // (which could name another column)
        let mut out = String::new();
        assert!(foreign_keys(
            b"CREATE TABLE c (pid INT, FOREIGN KEY (pid) REFERENCES p (`\xe9`))",
            "d",
            "d_1c",
            &mut out,
        )
        .is_err());
    }
}
