// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Direct writes: the handler's inserts, and updates and deletes of the
//! row it read, named by its row id. The table checks types, NOT NULL,
//! the primary key and UNIQUE as each row is written, except UNIQUE against
//! rows it holds in memory: an insert misses the transaction's own pending
//! rows and an update leaves the check to commit. MariaDB wants the error
//! from the row's own write (ON DUPLICATE KEY UPDATE, IGNORE, REPLACE), so
//! both look the keys up first.

use stoolap::core::{Error, Operator, Result, Row, Value};
use stoolap::storage::expression::{AndExpr, ComparisonExpr, ConstBoolExpr, Expression};
use stoolap::storage::traits::{ScanAccess, Table};

use crate::read::check_columns;

/// A row id no row has: the row an insert checks has none yet
const NO_ROW: i64 = i64::MIN;

/// Inserts `rows`, each a value per column
pub fn insert(
    table: &mut dyn Table,
    table_columns: usize,
    mut rows: Vec<Vec<Value>>,
) -> Result<i64> {
    check_columns(table, table_columns)?;
    let n = rows.len() as i64;
    if table.has_unique_non_pk_indexes() {
        // Row by row, each checked against the ones before it
        let all: Vec<usize> = (0..table_columns).collect();
        for values in rows {
            let row = Row::from_values(values);
            check_unique(table, NO_ROW, &row, &all)?;
            table.insert_discard(row)?;
        }
    } else if rows.len() == 1 {
        let row = rows.pop().expect("one row");
        table.insert_discard(Row::from_values(row))?;
    } else {
        table.insert_batch(rows.into_iter().map(Row::from_values).collect())?;
    }
    Ok(n)
}

/// Sets `columns` of row `row_id` to `values`; 0 when the row is gone
pub fn update(
    table: &mut dyn Table,
    table_columns: usize,
    row_id: i64,
    columns: &[usize],
    values: Vec<Value>,
) -> Result<i64> {
    check_columns(table, table_columns)?;
    if columns.len() != values.len() || columns.iter().any(|&c| c >= table_columns) {
        return Err(Error::internal("bad update columns"));
    }
    let pk = table.schema().pk_column_index();
    if !pk.is_some_and(|p| columns.contains(&p)) {
        if table.has_unique_non_pk_indexes() {
            let Some((_, mut row)) = table
                .fetch_rows_by_ids(&[row_id], &ConstBoolExpr::true_expr())?
                .into_iter()
                .next()
            else {
                return Ok(0);
            };
            for (&c, v) in columns.iter().zip(&values) {
                row.set(c, v.clone())?;
            }
            check_unique(table, row_id, &row, columns)?;
        }
        let mut set = |mut row: Row| -> Result<(Row, bool)> {
            for (&c, v) in columns.iter().zip(&values) {
                row.set(c, v.clone())?;
            }
            Ok((row, true))
        };
        return Ok(table.update_by_row_ids(&[row_id], &mut set)? as i64);
    }

    // An INTEGER primary key is the row id: the row moves to its new id
    let Some((_, old)) = table
        .fetch_rows_by_ids(&[row_id], &ConstBoolExpr::true_expr())?
        .into_iter()
        .next()
    else {
        return Ok(0);
    };
    let mut row = old.clone();
    for (&c, v) in columns.iter().zip(values) {
        row.set(c, v)?;
    }
    table.delete_by_row_ids(&[row_id])?;
    if let Err(e) = table.insert_discard(row) {
        // Put the row back: the statement fails, the transaction goes on
        table.insert_discard(old)?;
        return Err(e);
    }
    Ok(1)
}

/// Deletes row `row_id`; 0 when it is gone
pub fn delete(table: &mut dyn Table, row_id: i64) -> Result<i64> {
    Ok(table.delete_by_row_ids(&[row_id])? as i64)
}

/// Refuses `row`, the new state of row `row_id`, when a UNIQUE index over
/// one of the `changed` columns holds its key for another row
fn check_unique(table: &dyn Table, row_id: i64, row: &Row, changed: &[usize]) -> Result<()> {
    let schema = table.schema();
    for (index, names) in table.get_unique_indexes() {
        let Some(ids) = names
            .iter()
            .map(|n| schema.get_column_index(n))
            .collect::<Option<Vec<usize>>>()
        else {
            continue;
        };
        if !ids.iter().any(|i| changed.contains(i)) {
            continue;
        }
        let key: Vec<Value> = ids
            .iter()
            .map(|&i| row.get(i).cloned().unwrap_or_else(Value::null_unknown))
            .collect();
        if key.iter().any(|v| v.is_null()) {
            continue; // NULLs never collide
        }
        let mut exprs: Vec<Box<dyn Expression>> = ids
            .iter()
            .zip(&key)
            .map(|(&i, v)| {
                Box::new(ComparisonExpr::new(
                    schema.columns[i].name.as_str(),
                    Operator::Eq,
                    v.clone(),
                )) as Box<dyn Expression>
            })
            .collect();
        let mut filter: Box<dyn Expression> = if exprs.len() == 1 {
            exprs.pop().expect("one")
        } else {
            Box::new(AndExpr::new(exprs))
        };
        filter.prepare_for_schema(schema);
        let plan = table.index_plan(filter.as_ref());
        let access = match &plan {
            Some(p) => ScanAccess::Index(p),
            None => ScanAccess::Seq,
        };
        let mut scanner = table.scan_access(access, &[], None, Some(filter.as_ref()))?;
        while scanner.next() {
            let other = scanner.current_row_id();
            if other != row_id {
                return Err(Error::UniqueConstraint {
                    index,
                    column: names.join(", "),
                    value: key.into(),
                    row_id: other,
                });
            }
        }
        if let Some(e) = scanner.err() {
            return Err(e.clone());
        }
    }
    Ok(())
}
