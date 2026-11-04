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
use std::sync::Arc;

use stoolap::api::{Database, ParamVec};
use stoolap::core::{Error, IsolationLevel, Result, Row};
use stoolap::executor::{CachedPlanRef, ExecutionContext, Executor};
use stoolap::parser::ast::{RollbackStatement, Statement};
use stoolap::storage::traits::{QueryResult, Transaction};

pub struct Session {
    exec: Executor,
    /// Whether a transaction is installed in `exec`
    open: AtomicBool,
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
        let tx = self.exec.begin_transaction_with_isolation(level)?;
        self.exec.install_transaction(tx);
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

    fn take(&self) -> Result<Box<dyn Transaction>> {
        self.open.store(false, Ordering::Relaxed);
        self.exec.take_transaction().ok_or(Error::TransactionEnded)
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        if let Some(mut tx) = self.exec.take_transaction() {
            let _ = tx.rollback();
        }
    }
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
