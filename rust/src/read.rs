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
use std::sync::Arc;

use stoolap::core::{Error, Operator, Result, Row, Value};
use stoolap::storage::expression::{
    AndExpr, ComparisonExpr, ConstBoolExpr, Expression, NullCheckExpr, OrExpr,
};
use stoolap::storage::traits::{ScanAccess, Scanner, Table};

use crate::session::Session;

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

/// A read's condition in postfix order: `And(n)` / `Or(n)` join the `n`
/// nodes before them; what is left at the end is ANDed
pub enum Node {
    Cond(Cond),
    And(usize),
    Or(usize),
}

/// What a read returns and in which order
pub struct ReadSpec<'a> {
    /// The table's column count as the caller knows it
    pub table_columns: usize,
    /// The columns each row carries, in order
    pub columns: &'a [usize],
    pub conds: Vec<Node>,
    /// Columns to order by, ascending with NULL first unless `desc`
    pub order: &'a [usize],
    pub desc: bool,
    /// The statement may move rows to new ids: take the rows at open, not
    /// a page at a time, where a moved row would come back
    pub fixed: bool,
}

enum Source {
    Scan(Box<dyn Scanner>),
    Sorted(std::vec::IntoIter<(i64, Row)>),
    Paged(Pager),
}

/// A read of a whole table in the order of one indexed column, taken from
/// the index a page at a time: a MIN / MAX probe reads one short page
struct Pager {
    session: Arc<Session>,
    table: String,
    column: usize,
    desc: bool,
    /// Rows taken so far, where the next page starts
    offset: usize,
    page: usize,
    rows: std::vec::IntoIter<(i64, Row)>,
    done: bool,
    /// The column's value in the last row given, and the ids of the rows
    /// given with it: where a sorted scan goes on
    last: Option<(Value, Vec<i64>)>,
}

impl Pager {
    /// Loads the next page; false at the end
    fn fill(&mut self) -> Result<bool> {
        if self.done {
            return Ok(false);
        }
        let (column, asc, page, offset) = (self.column, !self.desc, self.page, self.offset);
        let got = self.session.with_table(&self.table, |t| {
            let name = t.schema().columns[column].name.clone();
            // MariaDB's order: NULL first ascending, last descending
            t.collect_rows_ordered_by_index(&name, asc, asc, page, offset)
        })?;
        let rows: Vec<(i64, Row)> = match got {
            Some(rows) => {
                let rows: Vec<(i64, Row)> = rows.into_iter().collect();
                self.done = rows.len() < page;
                rows
            }
            None => {
                // The index cannot answer now (the transaction wrote the
                // table, maybe the rows given): the rest from a sorted
                // scan, after the last row given, as an index cursor goes
                // on from its position
                self.done = true;
                let desc = self.desc;
                let all = self.session.with_table(&self.table, |t| {
                    sorted_rows(t.scan(&[], None)?, &[column], desc)
                })?;
                match &self.last {
                    None => all,
                    Some((value, ids)) => all
                        .into_iter()
                        .filter(|(id, row)| {
                            let o = compare_values(row.get(column), Some(value));
                            match if desc { o.reverse() } else { o } {
                                Ordering::Greater => true,
                                Ordering::Equal => !ids.contains(id),
                                Ordering::Less => false,
                            }
                        })
                        .collect(),
                }
            }
        };
        self.offset += rows.len();
        self.page = (page * 4).min(1 << 16);
        let more = !rows.is_empty();
        self.rows = rows.into_iter();
        Ok(more)
    }
}

/// Every row `scanner` gives, sorted by `order` (NULL first, all reversed
/// when `desc`)
fn sorted_rows(
    mut scanner: Box<dyn Scanner>,
    order: &[usize],
    desc: bool,
) -> Result<Vec<(i64, Row)>> {
    let mut rows = Vec::new();
    while scanner.next() {
        // take_row_with_id() gives 0 for the id on MVCC scanners
        let id = scanner.current_row_id();
        rows.push((id, scanner.take_row()));
    }
    if let Some(e) = scanner.err() {
        return Err(e.clone());
    }
    // Stable, and linear on rows the index already gave in order
    rows.sort_by(|a, b| {
        let o = compare_by(&a.1, &b.1, order);
        if desc {
            o.reverse()
        } else {
            o
        }
    });
    Ok(rows)
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
    /// The row `row_id`, if it is visible (the transaction's own pending
    /// rows included, which `collect_rows_by_ids` misses)
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
        let rows: Vec<(i64, Row)> = table
            .fetch_rows_by_ids(&[row_id], &ConstBoolExpr::true_expr())?
            .into_iter()
            .collect();
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

        // A key lookup (a join's eq_ref probe): the row by its id, with no
        // expression, plan or scanner
        if let [Node::Cond(Cond {
            column,
            op: CondOp::Eq,
            value,
        })] = spec.conds.as_slice()
        {
            if schema.pk_column_index() == Some(*column) {
                if let Value::Integer(id) = value
                    .clone()
                    .into_coerce_to_type(schema.columns[*column].data_type)
                {
                    let rows = table.fetch_rows_by_ids(&[id], &ConstBoolExpr::true_expr())?;
                    return Ok(Cursor {
                        source: Source::Sorted(rows.into_iter().collect::<Vec<_>>().into_iter()),
                        columns: spec.columns.to_vec(),
                    });
                }
            }
        }

        let mut exprs: Vec<Box<dyn Expression>> = Vec::with_capacity(spec.conds.len());
        for node in spec.conds {
            let cond = match node {
                Node::Cond(cond) => cond,
                Node::And(n) | Node::Or(n) => {
                    if n == 0 || n > exprs.len() {
                        return Err(Error::internal("condition joins missing operands"));
                    }
                    let operands = exprs.split_off(exprs.len() - n);
                    exprs.push(match node {
                        Node::And(_) => Box::new(AndExpr::new(operands)),
                        _ => Box::new(OrExpr::new(operands)),
                    });
                    continue;
                }
            };
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
            Source::Sorted(sorted_rows(scanner, spec.order, spec.desc)?.into_iter())
        };
        Ok(Cursor {
            source,
            columns: spec.columns.to_vec(),
        })
    }

    pub fn column_count(&self) -> usize {
        self.columns.len()
    }

    /// A read of `table` with no condition, ordered by one column: from the
    /// column's index a page at a time when it has one, else as `open`
    pub fn ordered(session: &Arc<Session>, table: &str, spec: ReadSpec<'_>) -> Result<Cursor> {
        if spec.fixed || !spec.conds.is_empty() || spec.order.len() != 1 {
            return session.with_table(table, |t| Cursor::open(t, spec));
        }
        let column = spec.order[0];
        let indexed = session.with_table(table, |t| {
            check_columns(t, spec.table_columns)?;
            if spec.columns.iter().any(|&c| c >= spec.table_columns) {
                return Err(Error::internal("column out of range"));
            }
            let name = &t.schema().columns[column].name;
            Ok(t.schema().pk_column_index() == Some(column) || t.has_index_on_column(name))
        })?;
        if !indexed {
            return session.with_table(table, |t| Cursor::open(t, spec));
        }
        Ok(Cursor {
            source: Source::Paged(Pager {
                session: Arc::clone(session),
                table: table.to_string(),
                column,
                desc: spec.desc,
                offset: 0,
                page: 16,
                rows: Vec::new().into_iter(),
                done: false,
                last: None,
            }),
            columns: spec.columns.to_vec(),
        })
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
            Source::Paged(pager) => {
                while n < max {
                    let Some((id, row)) = pager.rows.next() else {
                        if pager.fill()? {
                            continue;
                        }
                        break;
                    };
                    let value = row.get(pager.column);
                    match &mut pager.last {
                        Some((v, ids)) if compare_values(value, Some(v)) == Ordering::Equal => {
                            ids.push(id)
                        }
                        last => {
                            *last =
                                Some((value.cloned().unwrap_or_else(Value::null_unknown), vec![id]))
                        }
                    }
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
        let o = compare_values(a.get(c), b.get(c));
        if o != Ordering::Equal {
            return o;
        }
    }
    Ordering::Equal
}

/// MariaDB's order of two values: NULL (or none) before any value
fn compare_values(x: Option<&Value>, y: Option<&Value>) -> Ordering {
    let x = x.filter(|v| !v.is_null());
    let y = y.filter(|v| !v.is_null());
    match (x, y) {
        (None, None) => Ordering::Equal,
        (None, Some(_)) => Ordering::Less,
        (Some(_), None) => Ordering::Greater,
        (Some(x), Some(y)) => x.cmp(y),
    }
}
