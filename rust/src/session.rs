// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! One plugin session on the engine: its own executor, with the open
//! transaction installed in it.
//!
//! Stoolap's `api::Transaction` keeps its storage transaction inside a
//! private executor, out of reach. Holding the executor here instead lets
//! the plugin hand the same storage transaction to SQL statements and,
//! between them, take it out for direct table access: the transaction's
//! pending writes live in the engine's per-transaction version stores, not
//! in the table handles, so nothing is lost moving it in and out.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use rustc_hash::FxHashMap;

use stoolap::api::{Database, ParamVec};
use stoolap::core::{Error, IsolationLevel, Result, Row};
use stoolap::executor::{CachedPlanRef, ExecutionContext, Executor};
use stoolap::storage::mvcc::registry::StatementView;
use stoolap::parser::ast::{RollbackStatement, Statement};
use stoolap::storage::traits::{QueryResult, Table, Transaction};

pub struct Session {
    exec: Executor,
    /// Whether a transaction is installed in `exec`
    open: AtomicBool,
    /// Direct access outside an open transaction: a transaction of its own
    /// for the rest of the MariaDB statement, so SQL writes stay autocommit
    read_tx: Mutex<Option<Box<dyn Transaction>>>,
    /// The isolation level of the open transaction
    level: Mutex<IsolationLevel>,
    /// The table handles direct access goes through in this statement, of
    /// the open transaction or else of `read_tx`, taken once per table;
    /// under read committed they read through one view of the statement,
    /// which also lets a scan stream instead of collecting every row
    tables: Mutex<FxHashMap<String, Box<dyn Table>>>,
    view: Mutex<Option<StatementView>>,
    /// Keeps the engine open while the session lives
    _db: Database,
}

impl Session {
    pub fn db(&self) -> &Database {
        &self._db
    }

    pub fn new(db: &Database) -> Arc<Self> {
        Arc::new(Session {
            exec: Executor::new(Arc::clone(db.engine())),
            open: AtomicBool::new(false),
            read_tx: Mutex::new(None),
            level: Mutex::new(IsolationLevel::ReadCommitted),
            tables: Mutex::new(FxHashMap::default()),
            view: Mutex::new(None),
            _db: db.clone(),
        })
    }

    /// Runs `sql`, in the open transaction when there is one
    pub fn run(&self, sql: &str, params: ParamVec) -> Result<Box<dyn QueryResult>> {
        if self.in_transaction() {
            let plan = self.exec.get_or_create_plan(sql)?;
            return self.run_plan(&plan, params);
        }
        if params.is_empty() {
            return self.exec.execute(sql);
        }
        if let Some(result) = self.exec.try_fast_path_with_params(sql, &params) {
            return result;
        }
        self.exec.execute_with_params(sql, params)
    }

    pub fn prepare(&self, sql: &str) -> Result<CachedPlanRef> {
        self.exec.get_or_create_plan(sql)
    }

    /// Runs a prepared plan, in the open transaction when there is one
    pub fn run_plan(&self, plan: &CachedPlanRef, params: ParamVec) -> Result<Box<dyn QueryResult>> {
        if self.in_transaction() {
            runs_in_transaction(&plan.statement)?;
        }
        let ctx = if params.is_empty() {
            ExecutionContext::new()
        } else {
            ExecutionContext::with_params(params)
        };
        self.exec.execute_with_cached_plan(plan, &ctx)
    }

    /// The level a transaction begun without naming one runs at
    pub fn isolation_level(&self) -> IsolationLevel {
        self.exec.isolation_level()
    }

    pub fn in_transaction(&self) -> bool {
        self.open.load(Ordering::Relaxed)
    }

    pub fn begin(&self, level: IsolationLevel) -> Result<()> {
        if self.in_transaction() {
            return Err(Error::internal("a transaction is already open"));
        }
        self.end_statement();
        let tx = self.exec.begin_transaction_with_isolation(level)?;
        self.exec.install_transaction(tx);
        *lock(&self.level) = level;
        self.open.store(true, Ordering::Relaxed);
        Ok(())
    }

    /// Commits the open transaction; a failed commit rolls it back
    pub fn commit(&self) -> Result<()> {
        let mut tx = self.take()?;
        let result = tx.commit();
        if result.is_err() {
            let _ = tx.rollback();
        }
        result
    }

    pub fn rollback(&self) -> Result<()> {
        self.take()?.rollback()
    }

    /// Runs `f` on the handle on `table` of the open transaction, or
    /// else of the statement's own read transaction
    pub fn with_table<R>(&self, table: &str, f: impl FnOnce(&mut dyn Table) -> Result<R>) -> Result<R> {
        let mut tables = lock(&self.tables);
        if !tables.contains_key(table) {
            let handle = if self.in_transaction() {
                // The executor holds the transaction between statements
                let tx = self.exec.take_transaction().ok_or(Error::TransactionEnded)?;
                let handle = self.open_table(tx.as_ref(), *lock(&self.level), table);
                self.exec.install_transaction(tx);
                handle?
            } else {
                let mut read_tx = lock(&self.read_tx);
                if read_tx.is_none() {
                    *read_tx = Some(self.exec.begin_transaction()?);
                }
                let level = self.exec.isolation_level();
                self.open_table(read_tx.as_deref().expect("begun above"), level, table)?
            };
            tables.insert(table.to_string(), handle);
        }
        f(tables.get_mut(table).expect("inserted above").as_mut())
    }

    /// `tx`'s handle on `table`, through the statement's view under read
    /// committed
    fn open_table(&self, tx: &dyn Transaction, level: IsolationLevel, table: &str) -> Result<Box<dyn Table>> {
        if level != IsolationLevel::ReadCommitted {
            return tx.get_table(table);
        }
        let mut view = lock(&self.view);
        if view.is_none() {
            *view = Some(self.exec.engine().registry().capture_view());
        }
        tx.get_table_with_view(table, view.as_ref().expect("captured above").clone())
    }

    /// Ends the statement's direct access: its table handles and view, and
    /// the read transaction if it opened one
    pub fn end_statement(&self) {
        lock(&self.tables).clear();
        *lock(&self.view) = None;
        if let Some(mut tx) = lock(&self.read_tx).take() {
            let _ = tx.rollback(); // it wrote nothing
        }
    }

    fn take(&self) -> Result<Box<dyn Transaction>> {
        self.open.store(false, Ordering::Relaxed);
        // The handles go before the transaction ends
        lock(&self.tables).clear();
        self.exec.take_transaction().ok_or(Error::TransactionEnded)
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        self.end_statement();
        if let Some(mut tx) = self.exec.take_transaction() {
            let _ = tx.rollback();
        }
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// What may run inside an open transaction: DML and savepoints, as
/// stoolap's own `Transaction` allows
fn runs_in_transaction(statement: &Statement) -> Result<()> {
    match statement {
        Statement::Select(_)
        | Statement::Insert(_)
        | Statement::Update(_)
        | Statement::Delete(_)
        | Statement::Savepoint(_)
        | Statement::ReleaseSavepoint(_)
        | Statement::Rollback(RollbackStatement {
            savepoint_name: Some(_),
            ..
        }) => Ok(()),
        _ => Err(Error::NotSupported(
            "Only DML statements are supported in transactions".to_string(),
        )),
    }
}

/// A statement's result rows, read one at a time
pub struct Rows {
    result: Box<dyn QueryResult>,
    columns: Vec<String>,
    closed: bool,
    /// A runtime error (an invalid REGEXP, ...) the iteration stopped on
    pending_error: Option<Error>,
}

impl Rows {
    pub fn new(result: Box<dyn QueryResult>) -> Self {
        Rows {
            columns: result.columns().to_vec(),
            result,
            closed: false,
            pending_error: None,
        }
    }

    pub fn columns(&self) -> &[String] {
        &self.columns
    }

    pub fn advance(&mut self) -> bool {
        if self.closed {
            return false;
        }
        if self.result.next() {
            return true;
        }
        self.pending_error = self.result.last_error();
        self.close();
        false
    }

    pub fn current_row(&self) -> &Row {
        self.result.row()
    }

    pub fn error(&mut self) -> Option<Error> {
        self.pending_error.take()
    }

    fn close(&mut self) {
        if !self.closed {
            let _ = self.result.close();
            self.closed = true;
        }
    }
}

impl Drop for Rows {
    fn drop(&mut self) {
        self.close();
    }
}
