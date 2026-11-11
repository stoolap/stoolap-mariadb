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
use stoolap::core::{Error, Result, Row, Value};

use crate::session::Session;
use crate::write;

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

/// Refuses `table` when a row holds a foreign key value its parent holds
/// in no row, as the executor refuses an INSERT: by an UPDATE setting each
/// key column to itself, which has the executor check every row's values
/// against their parents and hold each parent key until the transaction
/// ends, so a concurrent delete of one conflicts instead of leaving a row
/// without it
pub fn check_foreign_keys(session: &Session, table: &str) -> Result<()> {
    let schema = session.with_any_write_table(table, |t| Ok(t.schema().clone()))?;
    let mut columns: Vec<String> = Vec::new();
    for fk in &schema.foreign_keys {
        let column = quote(&fk.column_name);
        if !columns.contains(&column) {
            columns.push(column);
        }
    }
    if columns.is_empty() {
        return Ok(());
    }
    let sql = format!(
        "UPDATE {} SET {} WHERE {}",
        quote(table),
        columns
            .iter()
            .map(|c| format!("{c} = {c}"))
            .collect::<Vec<_>>()
            .join(", "),
        columns
            .iter()
            .map(|c| format!("{c} IS NOT NULL"))
            .collect::<Vec<_>>()
            .join(" OR ")
    );
    session.run_write(&sql, ParamVec::new())?;
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
