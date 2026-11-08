// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Writes through SQL, for a table a foreign key names (stoolap's executor
//! checks the keys) and for a row the handler names by its primary key
//! rather than its row id. They run in the open transaction, or else in the
//! statement's own (`Session::run_write`), so a statement lands whole or
//! not at all.

use stoolap::api::ParamVec;
use stoolap::core::{Error, ForeignKeyAction, Result, Row, Value};

use crate::session::Session;
use crate::write::{self, ReadCheck};

fn quote(name: &str) -> String {
    format!("\"{}\"", name.replace('"', "\"\""))
}

fn params(values: impl IntoIterator<Item = Value>) -> ParamVec {
    values.into_iter().collect()
}

/// `col = $n AND ...` over `key`, numbering from `first`
fn key_clause(key: &[(String, Value)], first: usize) -> Result<String> {
    if key.is_empty() {
        return Err(Error::invalid_argument(
            "row-pump UPDATE/DELETE on a table without a PRIMARY KEY is \
             unsupported. Stoolap has no stable row-id, so MariaDB's filesort+LIMIT \
             path could over-mutate byte-identical duplicates. Add a PRIMARY KEY, or \
             rewrite the statement so the WHERE clause alone selects the rows (no \
             ORDER BY/LIMIT) -- direct DML then handles it as one engine-side \
             UPDATE/DELETE.",
        ));
    }
    Ok(key
        .iter()
        .enumerate()
        .map(|(i, (name, _))| format!("{} = ${}", quote(name), first + i))
        .collect::<Vec<_>>()
        .join(" AND "))
}

/// Inserts `rows`, each a value per column. Each row's UNIQUE keys are
/// looked up first, as a direct insert's are: in a transaction stoolap
/// may check them only at commit, and MariaDB wants the row's own error.
/// The rows are checked in one batch; when that finds a conflict, row by
/// row, so the statement fails with the first failing row's error (an
/// earlier row's missing parent before a later row's duplicate).
pub fn insert(session: &Session, table: &str, rows: Vec<Vec<Value>>) -> Result<i64> {
    let Some(width) = rows.first().map(Vec::len) else {
        return Ok(0);
    };
    let marks: Vec<String> = (1..=width).map(|i| format!("${}", i)).collect();
    let sql = format!("INSERT INTO {} VALUES ({})", quote(table), marks.join(","));
    let new: Vec<Row> = rows.iter().cloned().map(Row::from_values).collect();
    let batch_ok = session
        .with_table(table, |t| write::check_insert(t, &new))
        .is_ok();
    let mut total = 0;
    for (row, checked) in rows.into_iter().zip(new) {
        if !batch_ok {
            session.with_table(table, |t| {
                write::check_insert(t, std::slice::from_ref(&checked))
            })?;
        }
        total += session.run_write(&sql, params(row))?.rows_affected();
    }
    Ok(total)
}

/// Sets `set` in the row `key` names. The row's new UNIQUE keys are looked
/// up first when its `row_id` is known (`check`: the columns `set` names,
/// by position), as `insert`'s are
pub fn update(
    session: &Session,
    table: &str,
    set: Vec<(String, Value)>,
    key: Vec<(String, Value)>,
    check: Option<(i64, &[usize])>,
) -> Result<i64> {
    if let Some((row_id, columns)) = check {
        let values: Vec<Value> = set.iter().map(|(_, v)| v.clone()).collect();
        session.with_table(table, |t| write::check_update(t, row_id, columns, &values))?;
    }
    let assign: Vec<String> = set
        .iter()
        .enumerate()
        .map(|(i, (name, _))| format!("{} = ${}", quote(name), i + 1))
        .collect();
    let sql = format!(
        "UPDATE {} SET {} WHERE {}",
        quote(table),
        assign.join(", "),
        key_clause(&key, set.len() + 1)?
    );
    let values = set.into_iter().chain(key).map(|(_, v)| v);
    Ok(session.run_write(&sql, params(values))?.rows_affected())
}

/// How deep a key's move takes the rows that name it along, as stoolap's
/// executor cascades
const MAX_CASCADE_DEPTH: usize = 16;

/// Moves row `row_id`, which `key` names by its primary key (the `pk`-th
/// of `table_columns` columns), to the key `to`, setting `rest` (the other
/// columns, `check` as `update`'s) too. Stoolap's UPDATE refuses its
/// primary key, which is the row id, so the row moves on the storage
/// handle, in the statement's transaction, as a direct write moves it; the
/// caller holds it first. What the executor would do around the key is
/// done here: a row a foreign key names refuses the move (RESTRICT, NO
/// ACTION) or takes its rows along (CASCADE, SET NULL), and a key that is
/// itself a foreign key needs its parent. The rest goes through SQL, where
/// the executor checks its keys. All of it under a savepoint: a row that
/// fails leaves nothing written, as MariaDB's IGNORE expects.
#[allow(clippy::too_many_arguments)]
pub fn move_key(
    session: &Session,
    table: &str,
    table_columns: usize,
    row_id: i64,
    pk: usize,
    key: (String, Value),
    to: Value,
    rest: Vec<(String, Value)>,
    check: Option<&[usize]>,
) -> Result<i64> {
    session.atomically(|| {
        move_row(
            session,
            table,
            table_columns,
            row_id,
            pk,
            key,
            to,
            rest,
            check,
            0,
        )
    })
}

#[allow(clippy::too_many_arguments)]
fn move_row(
    session: &Session,
    table: &str,
    table_columns: usize,
    row_id: i64,
    pk: usize,
    key: (String, Value),
    to: Value,
    rest: Vec<(String, Value)>,
    check: Option<&[usize]>,
    depth: usize,
) -> Result<i64> {
    if depth > MAX_CASCADE_DEPTH {
        return Err(Error::internal(format!(
            "foreign key cascade of '{}' deeper than {} tables",
            table, MAX_CASCADE_DEPTH
        )));
    }
    let (name, from) = key;
    // Keys are matched by the columns they resolve to (their names are in
    // lower case), and rows read through write handles, which take no
    // statement view: a row committed since the statement began counts
    let schema = session.with_any_write_table(table, |t| Ok(t.schema().clone()))?;
    for fk in &schema.foreign_keys {
        if schema.get_column_index(&fk.column_name) != Some(pk) || to.is_null() {
            continue;
        }
        let parent = &fk.referenced_table;
        let found = session.with_any_write_table(parent, |t| {
            match t.schema().get_column_index(&fk.referenced_column) {
                Some(i) => write::holds(t, i, &to),
                None => Ok(false),
            }
        })?;
        if !found {
            return Err(Error::foreign_key_violation(
                table,
                &fk.column_name,
                parent,
                &fk.referenced_column,
                format!(
                    "referenced row with {} = {} does not exist",
                    fk.referenced_column, to
                ),
            ));
        }
    }
    let mut follow: Vec<(String, Vec<(usize, Value)>)> = Vec::new();
    for (child, fk) in session.referencing_keys(table) {
        if schema.get_column_index(&fk.referenced_column) != Some(pk) {
            continue;
        }
        let column = session.with_any_write_table(&child, |t| {
            match t.schema().get_column_index(&fk.column_name) {
                Some(i) => Ok(write::holds(t, i, &from)?.then_some(i)),
                None => Ok(None),
            }
        })?;
        let Some(column) = column else {
            continue;
        };
        let value = match fk.on_update {
            ForeignKeyAction::Restrict | ForeignKeyAction::NoAction => {
                return Err(Error::foreign_key_violation(
                    &child,
                    &fk.column_name,
                    table,
                    &fk.referenced_column,
                    format!(
                        "cannot update row with {} = {} \u{2014} still referenced by table '{}'",
                        fk.referenced_column, from, child
                    ),
                ));
            }
            ForeignKeyAction::Cascade => to.clone(),
            ForeignKeyAction::SetNull => Value::null_unknown(),
        };
        match follow.iter_mut().find(|(c, _)| *c == child) {
            Some((_, columns)) => columns.push((column, value)),
            None => follow.push((child, vec![(column, value)])),
        }
    }
    if !rest.is_empty() {
        update(
            session,
            table,
            rest,
            vec![(name, from.clone())],
            check.map(|c| (row_id, c)),
        )?;
    }
    let unchecked = ReadCheck {
        columns: &[],
        values: &[],
        pad: &[],
    };
    let n = session.with_any_write_table(table, |t| {
        write::update(
            t,
            table_columns,
            row_id,
            &[pk],
            vec![to.clone()],
            &unchecked,
        )
    })?;
    for (child, columns) in follow {
        follow_key(session, &child, columns, &from, depth)?;
    }
    Ok(n)
}

/// Takes the rows of `child` that hold the moved key `from` in `columns`
/// to the columns' new values. A column that is the child's own primary key
/// moves its row as the key moved; the rest change in one UPDATE, so the
/// executor checks each row with all its keys moved.
fn follow_key(
    session: &Session,
    child: &str,
    columns: Vec<(usize, Value)>,
    from: &Value,
    depth: usize,
) -> Result<()> {
    let schema = session.with_any_write_table(child, |t| Ok(t.schema().clone()))?;
    let mut set = Vec::new();
    for (column, value) in columns {
        let name = schema.columns[column].name.clone();
        match from {
            // An INTEGER primary key is the row id
            Value::Integer(id) if schema.pk_column_index() == Some(column) => {
                move_row(
                    session,
                    child,
                    schema.columns.len(),
                    *id,
                    column,
                    (name, from.clone()),
                    value,
                    Vec::new(),
                    None,
                    depth + 1,
                )?;
            }
            _ => set.push((name, value)),
        }
    }
    if set.is_empty() {
        return Ok(());
    }
    let mut values = vec![from.clone()];
    let mut assign = Vec::new();
    for (name, value) in &set {
        values.push(value.clone());
        assign.push(format!(
            "{c} = CASE WHEN {c} = $1 THEN ${n} ELSE {c} END",
            c = quote(name),
            n = values.len()
        ));
    }
    let found: Vec<String> = set
        .iter()
        .map(|(c, _)| format!("{} = $1", quote(c)))
        .collect();
    let sql = format!(
        "UPDATE {} SET {} WHERE {}",
        quote(child),
        assign.join(", "),
        found.join(" OR ")
    );
    session.run_write(&sql, params(values))?;
    Ok(())
}

/// Deletes the row `key` names
pub fn delete(session: &Session, table: &str, key: Vec<(String, Value)>) -> Result<i64> {
    let sql = format!(
        "DELETE FROM {} WHERE {}",
        quote(table),
        key_clause(&key, 1)?
    );
    Ok(session
        .run_write(&sql, params(key.into_iter().map(|(_, v)| v)))?
        .rows_affected())
}
