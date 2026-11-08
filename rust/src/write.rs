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
use stoolap::storage::expression::{
    AndExpr, ComparisonExpr, ConstBoolExpr, Expression, InListExpr,
};
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
    if table.has_unique_non_pk_indexes() && rows.len() > 1 {
        let rows: Vec<Row> = rows.into_iter().map(Row::from_values).collect();
        check_unique_batch(table, &rows)?;
        table.insert_batch(rows)?;
    } else if table.has_unique_non_pk_indexes() {
        let all: Vec<usize> = (0..table_columns).collect();
        let row = Row::from_values(rows.pop().ok_or_else(|| Error::internal("no row"))?);
        check_unique(table, NO_ROW, &row, &all)?;
        table.insert_discard(row)?;
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
    // Before the old row goes: the insert below misses pending rows, and
    // the row's own keys, which it gives up, are no conflict
    if table.has_unique_non_pk_indexes() {
        check_unique(table, row_id, &row, columns)?;
    }
    table.delete_by_row_ids(&[row_id])?;
    if let Err(e) = table.insert_discard(row) {
        // Put the row back: the statement fails, the transaction goes on
        table.insert_discard(old)?;
        return Err(e);
    }
    Ok(1)
}

/// Refuses `rows`, new rows of `table`, when a UNIQUE index holds one's
/// key, as `insert` does; a write through SQL, whose UNIQUE check may wait
/// for commit, asks first
pub(crate) fn check_insert(table: &dyn Table, rows: &[Row]) -> Result<()> {
    if !table.has_unique_non_pk_indexes() {
        return Ok(());
    }
    if let [row] = rows {
        let all: Vec<usize> = (0..table.schema().columns.len()).collect();
        return check_unique(table, NO_ROW, row, &all);
    }
    check_unique_batch(table, rows)
}

/// Refuses setting `columns` to `values` in row `row_id` when a UNIQUE
/// index would then hold its key for another row, as `check_insert`
pub(crate) fn check_update(
    table: &dyn Table,
    row_id: i64,
    columns: &[usize],
    values: &[Value],
) -> Result<()> {
    // Only a UNIQUE index over a changed column can conflict
    let schema = table.schema();
    let touched = table.get_unique_indexes().iter().any(|(_, names)| {
        names.iter().any(|n| {
            schema
                .get_column_index(n)
                .is_some_and(|i| columns.contains(&i))
        })
    });
    if !touched {
        return Ok(());
    }
    let Some((_, mut row)) = table
        .fetch_rows_by_ids(&[row_id], &ConstBoolExpr::true_expr())?
        .into_iter()
        .next()
    else {
        return Ok(());
    };
    for (&c, v) in columns.iter().zip(values) {
        row.set(c, v.clone())?;
    }
    check_unique(table, row_id, &row, columns)
}

/// Deletes row `row_id`; 0 when it is gone
pub fn delete(table: &mut dyn Table, row_id: i64) -> Result<i64> {
    Ok(table.delete_by_row_ids(&[row_id])? as i64)
}

/// Refuses `rows`, new rows, when two hold one key of a UNIQUE index or
/// one holds a key the table does. Each single-column index is read once
/// for all the keys: a check per row would read the transaction's own
/// rows, which no index holds, once for every row.
fn check_unique_batch(table: &dyn Table, rows: &[Row]) -> Result<()> {
    let schema = table.schema();
    for (index, names) in table.get_unique_indexes() {
        let Some(ids) = names
            .iter()
            .map(|n| schema.get_column_index(n))
            .collect::<Option<Vec<usize>>>()
        else {
            continue;
        };
        let conflict = |value: Vec<Value>, row_id| Error::UniqueConstraint {
            index: index.clone(),
            column: names.join(", "),
            value: value.into(),
            row_id,
        };
        let mut seen = std::collections::HashSet::with_capacity(rows.len());
        for row in rows {
            let key: Vec<Value> = ids
                .iter()
                .map(|&i| row.get(i).cloned().unwrap_or_else(Value::null_unknown))
                .collect();
            if key.iter().any(|v| v.is_null()) {
                continue; // NULLs never collide
            }
            if !seen.insert(key.clone()) {
                return Err(conflict(key, -1));
            }
        }
        if seen.is_empty() {
            continue;
        }
        let [column] = ids[..] else {
            // A composite key: row by row
            for row in rows {
                check_unique(table, NO_ROW, row, &ids)?;
            }
            continue;
        };
        let values: Vec<Value> = seen.into_iter().map(|mut k| k.remove(0)).collect();
        let mut filter = InListExpr::new(schema.columns[column].name.as_str(), values);
        filter.prepare_for_schema(schema);
        let plan = table.index_plan(&filter);
        let access = match &plan {
            Some(p) => ScanAccess::Index(p),
            None => ScanAccess::Seq,
        };
        let mut scanner = table.scan_access(access, &[], None, Some(&filter))?;
        if scanner.next() {
            let value = scanner.row().get(column).cloned();
            return Err(conflict(
                value.into_iter().collect(),
                scanner.current_row_id(),
            ));
        }
        if let Some(e) = scanner.err() {
            return Err(e.clone());
        }
    }
    Ok(())
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
