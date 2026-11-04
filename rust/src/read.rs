// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! Direct reads: the handler's scans and key lookups on a table handle of
//! the open transaction, with no SQL, parse or plan in between.
//!
//! Columns are named by position: the plugin creates every stoolap table
//! with MariaDB's fields in field order and never alters one in place.

use std::cmp::Ordering;

use stoolap::core::{Error, Operator, Result, Row, Value};
use stoolap::storage::expression::{AndExpr, ComparisonExpr, Expression, NullCheckExpr};
use stoolap::storage::traits::{ScanAccess, Scanner, Table};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum CondOp {
    Eq,
    Gt,
    Ge,
    Lt,
    Le,
    IsNull,
    IsNotNull,
}

/// `column op value`; the value is unused for the NULL tests
pub struct Cond {
    pub column: usize,
    pub op: CondOp,
    pub value: Value,
}

/// What a read returns and in which order
pub struct ReadSpec<'a> {
    /// The table's column count as the caller knows it
    pub table_columns: usize,
    /// The columns each row carries, in order
    pub columns: &'a [usize],
    pub conds: Vec<Cond>,
    /// Columns to order by, ascending with NULL first unless `desc`
    pub order: &'a [usize],
    pub desc: bool,
}

enum Source {
    Scan(Box<dyn Scanner>),
    Sorted(std::vec::IntoIter<(i64, Row)>),
}

/// The rows of a read, handed out in chunks
pub struct Cursor {
    source: Source,
    columns: Vec<usize>,
}

/// Refuses a table whose column count is not the one MariaDB knows
pub fn check_columns(table: &dyn Table, table_columns: usize) -> Result<()> {
    let n = table.schema().columns.len();
    if n != table_columns {
        return Err(Error::internal(format!(
            "table {} has {} columns, MariaDB knows {}",
            table.name(),
            n,
            table_columns
        )));
    }
    Ok(())
}

impl Cursor {
    /// The row `row_id`, if it is visible
    pub fn by_id(
        table: &dyn Table,
        table_columns: usize,
        columns: &[usize],
        row_id: i64,
    ) -> Result<Cursor> {
        check_columns(table, table_columns)?;
        if columns.iter().any(|&c| c >= table_columns) {
            return Err(Error::internal("column out of range"));
        }
        let rows: Vec<(i64, Row)> = table.collect_rows_by_ids(&[row_id])?.into_iter().collect();
        Ok(Cursor {
            source: Source::Sorted(rows.into_iter()),
            columns: columns.to_vec(),
        })
    }

    pub fn open(table: &dyn Table, spec: ReadSpec<'_>) -> Result<Cursor> {
        let schema = table.schema();
        check_columns(table, spec.table_columns)?;
        let column_ok = |c: usize| {
            if c < schema.columns.len() {
                Ok(())
            } else {
                Err(Error::internal(format!(
                    "no column {} in {}",
                    c,
                    table.name()
                )))
            }
        };
        for &c in spec.columns.iter().chain(spec.order) {
            column_ok(c)?;
        }

        let mut exprs: Vec<Box<dyn Expression>> = Vec::with_capacity(spec.conds.len());
        for cond in spec.conds {
            column_ok(cond.column)?;
            let col = &schema.columns[cond.column];
            let op = match cond.op {
                CondOp::IsNull => {
                    exprs.push(Box::new(NullCheckExpr::is_null(col.name.as_str())));
                    continue;
                }
                CondOp::IsNotNull => {
                    exprs.push(Box::new(NullCheckExpr::is_not_null(col.name.as_str())));
                    continue;
                }
                CondOp::Eq => Operator::Eq,
                CondOp::Gt => Operator::Gt,
                CondOp::Ge => Operator::Gte,
                CondOp::Lt => Operator::Lt,
                CondOp::Le => Operator::Lte,
            };
            // As a bound parameter would be: in the column's type
            let value = cond.value.into_coerce_to_type(col.data_type);
            exprs.push(Box::new(ComparisonExpr::new(col.name.as_str(), op, value)));
        }
        let mut filter: Option<Box<dyn Expression>> = match exprs.len() {
            0 => None,
            1 => exprs.pop(),
            _ => Some(Box::new(AndExpr::new(exprs))),
        };
        if let Some(f) = filter.as_mut() {
            f.prepare_for_schema(schema);
        }

        // No column list: full rows, which `columns` picks from
        let scanner = match filter.as_deref() {
            None => table.scan(&[], None)?,
            Some(f) => {
                let plan = table.index_plan(f);
                let access = match &plan {
                    Some(p) => ScanAccess::Index(p),
                    None => ScanAccess::Seq,
                };
                table.scan_access(access, &[], None, Some(f))?
            }
        };

        let source = if spec.order.is_empty() {
            Source::Scan(scanner)
        } else {
            let mut scanner = scanner;
            let mut rows = Vec::new();
            while scanner.next() {
                rows.push(scanner.take_row_with_id());
            }
            if let Some(e) = scanner.err() {
                return Err(e.clone());
            }
            // Stable, and linear on rows the index already gave in order
            let (order, desc) = (spec.order, spec.desc);
            rows.sort_by(|a, b| {
                let o = compare_by(&a.1, &b.1, order);
                if desc {
                    o.reverse()
                } else {
                    o
                }
            });
            Source::Sorted(rows.into_iter())
        };
        Ok(Cursor {
            source,
            columns: spec.columns.to_vec(),
        })
    }

    pub fn column_count(&self) -> usize {
        self.columns.len()
    }

    /// Calls `emit` with each of up to `max` rows, its row id and the
    /// columns to read; the count is short only at the end
    pub fn next_rows(
        &mut self,
        max: usize,
        mut emit: impl FnMut(i64, &Row, &[usize]) -> Result<()>,
    ) -> Result<usize> {
        let mut n = 0;
        match &mut self.source {
            Source::Scan(scanner) => {
                while n < max && scanner.next() {
                    emit(scanner.current_row_id(), scanner.row(), &self.columns)?;
                    n += 1;
                }
                if n < max {
                    if let Some(e) = scanner.err() {
                        return Err(e.clone());
                    }
                }
            }
            Source::Sorted(rows) => {
                while n < max {
                    let Some((id, row)) = rows.next() else { break };
                    emit(id, &row, &self.columns)?;
                    n += 1;
                }
            }
        }
        Ok(n)
    }
}

/// MariaDB's key order: NULL before any value
fn compare_by(a: &Row, b: &Row, columns: &[usize]) -> Ordering {
    for &c in columns {
        let (x, y) = (a.get(c), b.get(c));
        let x = x.filter(|v| !v.is_null());
        let y = y.filter(|v| !v.is_null());
        let o = match (x, y) {
            (None, None) => Ordering::Equal,
            (None, Some(_)) => Ordering::Less,
            (Some(_), None) => Ordering::Greater,
            (Some(x), Some(y)) => x.cmp(y),
        };
        if o != Ordering::Equal {
            return o;
        }
    }
    Ordering::Equal
}
