// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Writes through SQL, for a table a foreign key names (stoolap's executor
//! checks the keys) and for a row the handler names by its primary key
//! rather than its row id. They run as the plugin's SQL always has: in the
//! open transaction, or else autocommit; a multi-row insert outside one
//! runs in a transaction of its own, so it lands whole or not at all.

use stoolap::api::ParamVec;
use stoolap::core::{Error, Result, Value};

use crate::session::Session;

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

/// Inserts `rows`, each a value per column
pub fn insert(session: &Session, table: &str, rows: Vec<Vec<Value>>) -> Result<i64> {
    let Some(width) = rows.first().map(Vec::len) else {
        return Ok(0);
    };
    let marks: Vec<String> = (1..=width).map(|i| format!("${}", i)).collect();
    let sql = format!("INSERT INTO {} VALUES ({})", quote(table), marks.join(","));
    if rows.len() == 1 || session.in_transaction() {
        let mut total = 0;
        for row in rows {
            total += session.run(&sql, params(row))?.rows_affected();
        }
        return Ok(total);
    }
    let plan = session.prepare(&sql)?;
    session.begin(session.isolation_level())?;
    let mut total = 0;
    for row in rows {
        match session.run_plan(&plan, params(row)) {
            Ok(r) => total += r.rows_affected(),
            Err(e) => {
                let _ = session.rollback();
                return Err(e);
            }
        }
    }
    session.commit()?;
    Ok(total)
}

/// Sets `set` in the row `key` names
pub fn update(
    session: &Session,
    table: &str,
    set: Vec<(String, Value)>,
    key: Vec<(String, Value)>,
) -> Result<i64> {
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
    Ok(session.run(&sql, params(values))?.rows_affected())
}

/// Deletes the row `key` names
pub fn delete(session: &Session, table: &str, key: Vec<(String, Value)>) -> Result<i64> {
    let sql = format!(
        "DELETE FROM {} WHERE {}",
        quote(table),
        key_clause(&key, 1)?
    );
    Ok(session
        .run(&sql, params(key.into_iter().map(|(_, v)| v)))?
        .rows_affected())
}
