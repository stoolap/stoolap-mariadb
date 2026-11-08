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
use stoolap::parser::ast::{RollbackStatement, Statement};
use stoolap::storage::mvcc::registry::StatementView;
use stoolap::storage::traits::{QueryResult, Table, Transaction};

pub struct Session {
    exec: Executor,
    /// Whether a transaction is installed in `exec`
    open: AtomicBool,
    /// Direct access outside an open transaction: a transaction of the
    /// MariaDB statement's own, which `end_statement` commits or rolls back
    stmt_tx: Mutex<Option<Box<dyn Transaction>>>,
    /// Whether a direct write went to `stmt_tx`, which then needs a commit
    stmt_wrote: AtomicBool,
    /// Whether the statement wrote in the open transaction, after a
    /// savepoint (`STMT_SAVEPOINT`) that `end_statement` releases or, for a
    /// failed statement, rolls back to: MariaDB undoes a failed statement
    /// and keeps the transaction
    stmt_savepoint: AtomicBool,
    /// The isolation level of the open transaction
    level: Mutex<IsolationLevel>,
    /// The table handles direct access goes through in this statement, of
    /// the open transaction or else of `stmt_tx`, taken once per table;
    /// under read committed reads go through one view of the statement,
    /// which also lets a scan stream instead of collecting every row.
    /// Writes go through handles of their own, with no view
    tables: Mutex<FxHashMap<String, Box<dyn Table>>>,
    write_tables: Mutex<FxHashMap<String, Box<dyn Table>>>,
    view: Mutex<Option<StatementView>>,
    /// Keeps the engine open while the session lives
    _db: Database,
}

impl Session {
    pub fn db(&self) -> &Database {
        &self._db
    }

    /// The tables whose foreign keys name `parent`, other than itself,
    /// each with the key's column and the parent column it names
    pub fn referencing_tables(&self, parent: &str) -> Vec<(String, String, String)> {
        let parent = parent.to_lowercase();
        self.exec
            .engine()
            .find_referencing_fks(&parent)
            .iter()
            .filter(|(child, _)| *child != parent)
            .map(|(child, fk)| {
                (
                    child.clone(),
                    fk.column_name.clone(),
                    fk.referenced_column.clone(),
                )
            })
            .collect()
    }

    pub fn new(db: &Database) -> Arc<Self> {
        Arc::new(Session {
            exec: Executor::new(Arc::clone(db.engine())),
            open: AtomicBool::new(false),
            stmt_tx: Mutex::new(None),
            stmt_wrote: AtomicBool::new(false),
            stmt_savepoint: AtomicBool::new(false),
            level: Mutex::new(IsolationLevel::ReadCommitted),
            tables: Mutex::new(FxHashMap::default()),
            write_tables: Mutex::new(FxHashMap::default()),
            view: Mutex::new(None),
            _db: db.clone(),
        })
    }

    /// Runs `sql`, in the open transaction when there is one. Outside one,
    /// a SELECT in a statement whose direct access opened a transaction
    /// reads in it, seeing the statement's direct writes; other SQL stays
    /// autocommit, as an UPDATE in a transaction would leave UNIQUE to
    /// commit
    pub fn run(&self, sql: &str, params: ParamVec) -> Result<Box<dyn QueryResult>> {
        if self.in_transaction() {
            let plan = self.exec.get_or_create_plan(sql)?;
            return self.run_plan(&plan, params);
        }
        if lock(&self.stmt_tx).is_some() {
            let plan = self.exec.get_or_create_plan(sql)?;
            if matches!(*plan.statement, Statement::Select(_)) {
                return self.run_in_stmt_tx(&plan, params);
            }
        }
        if params.is_empty() {
            return self.exec.execute(sql);
        }
        if let Some(result) = self.exec.try_fast_path_with_params(sql, &params) {
            return result;
        }
        self.exec.execute_with_params(sql, params)
    }

    /// Runs a write of the handler's through SQL: in the open transaction,
    /// or else in the statement's own, which `end_statement` commits or
    /// rolls back, so a statement's rows land whole or not at all
    pub fn run_write(&self, sql: &str, params: ParamVec) -> Result<Box<dyn QueryResult>> {
        if self.in_transaction() {
            return self.run(sql, params);
        }
        let plan = self.exec.get_or_create_plan(sql)?;
        {
            let mut stmt_tx = lock(&self.stmt_tx);
            if stmt_tx.is_none() {
                *stmt_tx = Some(self.exec.begin_transaction()?);
            }
        }
        self.stmt_wrote.store(true, Ordering::Relaxed);
        self.run_in_stmt_tx(&plan, params)
    }

    pub fn prepare(&self, sql: &str) -> Result<CachedPlanRef> {
        self.exec.get_or_create_plan(sql)
    }

    /// Runs a prepared plan, in the open transaction when there is one
    pub fn run_plan(&self, plan: &CachedPlanRef, params: ParamVec) -> Result<Box<dyn QueryResult>> {
        if self.in_transaction() {
            runs_in_transaction(&plan.statement)?;
            if matches!(
                *plan.statement,
                Statement::Insert(_) | Statement::Update(_) | Statement::Delete(_)
            ) {
                self.statement_writes()?;
            }
        }
        let ctx = if params.is_empty() {
            ExecutionContext::new()
        } else {
            ExecutionContext::with_params(params)
        };
        self.exec.execute_with_cached_plan(plan, &ctx)
    }

    /// Runs `plan` in the statement's own transaction, lent to the
    /// executor for the call
    fn run_in_stmt_tx(
        &self,
        plan: &CachedPlanRef,
        params: ParamVec,
    ) -> Result<Box<dyn QueryResult>> {
        let tx = lock(&self.stmt_tx).take().ok_or(Error::TransactionEnded)?;
        self.exec.install_transaction(tx);
        let ctx = if params.is_empty() {
            ExecutionContext::new()
        } else {
            ExecutionContext::with_params(params)
        };
        let result = self.exec.execute_with_cached_plan(plan, &ctx);
        *lock(&self.stmt_tx) = self.exec.take_transaction();
        result
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
        self.end_statement(true)?;
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

    /// Runs `f` on the read handle on `table` of the open transaction,
    /// or else of the statement's own transaction
    pub fn with_table<R>(
        &self,
        table: &str,
        f: impl FnOnce(&mut dyn Table) -> Result<R>,
    ) -> Result<R> {
        // A table the statement wrote is read through its write handle: a
        // view taken before the write would not see it
        if let Some(t) = lock(&self.write_tables).get_mut(table) {
            return f(t.as_mut());
        }
        self.with_handle(&self.tables, table, true, f)
    }

    /// Runs `f` on the write handle on `table`, or returns None for a
    /// table a foreign key names, as child or parent: those checks run in
    /// stoolap's executor, so such writes go through SQL
    pub fn with_write_table<R>(
        &self,
        table: &str,
        f: impl FnOnce(&mut dyn Table) -> Result<R>,
    ) -> Result<Option<R>> {
        let engine = self.exec.engine();
        if self.in_transaction() {
            self.statement_writes()?;
        } else {
            self.stmt_wrote.store(true, Ordering::Relaxed);
        }
        self.with_handle(&self.write_tables, table, false, |t| {
            if !t.schema().foreign_keys.is_empty()
                || !engine
                    .find_referencing_fks(&t.name().to_lowercase())
                    .is_empty()
            {
                Ok(None)
            } else {
                f(t).map(Some)
            }
        })
    }

    /// Whether a foreign key names `table` (as child or parent): its
    /// writes go through SQL, where the executor checks the keys
    pub fn writes_through_sql(&self, table: &str) -> Result<bool> {
        use stoolap::storage::traits::Engine;
        let engine = self.exec.engine();
        let schema = engine.get_table_schema(table)?;
        // Keys name their parent in lower case (a table named P under
        // lower_case_table_names = 0 is "P" here)
        Ok(!schema.foreign_keys.is_empty()
            || !engine
                .find_referencing_fks(&table.to_lowercase())
                .is_empty())
    }

    /// `table`'s column names, in order
    pub fn column_names(&self, table: &str) -> Result<Vec<String>> {
        use stoolap::storage::traits::Engine;
        let schema = self.exec.engine().get_table_schema(table)?;
        Ok(schema.columns.iter().map(|c| c.name.to_string()).collect())
    }

    fn with_handle<R>(
        &self,
        cache: &Mutex<FxHashMap<String, Box<dyn Table>>>,
        table: &str,
        read: bool,
        f: impl FnOnce(&mut dyn Table) -> Result<R>,
    ) -> Result<R> {
        let mut tables = lock(cache);
        if !tables.contains_key(table) {
            let handle = if self.in_transaction() {
                // The executor holds the transaction between statements
                let tx = self
                    .exec
                    .take_transaction()
                    .ok_or(Error::TransactionEnded)?;
                let with_view = read && *lock(&self.level) == IsolationLevel::ReadCommitted;
                let handle = self.open_table(tx.as_ref(), with_view, table);
                self.exec.install_transaction(tx);
                handle?
            } else {
                let mut stmt_tx = lock(&self.stmt_tx);
                if stmt_tx.is_none() {
                    *stmt_tx = Some(self.exec.begin_transaction()?);
                }
                let with_view =
                    read && self.exec.isolation_level() == IsolationLevel::ReadCommitted;
                self.open_table(stmt_tx.as_deref().expect("begun above"), with_view, table)?
            };
            tables.insert(table.to_string(), handle);
        }
        f(tables.get_mut(table).expect("inserted above").as_mut())
    }

    /// `tx`'s handle on `table`, through the statement's view when
    /// `with_view` (read committed reads only: a snapshot transaction
    /// takes none)
    fn open_table(
        &self,
        tx: &dyn Transaction,
        with_view: bool,
        table: &str,
    ) -> Result<Box<dyn Table>> {
        if !with_view {
            return tx.get_table(table);
        }
        let mut view = lock(&self.view);
        if view.is_none() {
            *view = Some(self.exec.engine().registry().capture_view());
        }
        tx.get_table_with_view(table, view.as_ref().expect("captured above").clone())
    }

    /// Marks the statement as writing in the open transaction: the first
    /// write takes a savepoint, between statements of the transaction
    fn statement_writes(&self) -> Result<()> {
        if self.stmt_savepoint.load(Ordering::Relaxed) {
            return Ok(());
        }
        let mut tx = self
            .exec
            .take_transaction()
            .ok_or(Error::TransactionEnded)?;
        let result = tx.create_savepoint(STMT_SAVEPOINT);
        self.exec.install_transaction(tx);
        result?;
        self.stmt_savepoint.store(true, Ordering::Relaxed);
        Ok(())
    }

    /// Ends the statement's direct access: its table handles and view, and
    /// the statement's own transaction if it opened one, committed or
    /// rolled back as `commit` says. In an open transaction, a statement
    /// that wrote keeps its writes or, failed, has them undone
    pub fn end_statement(&self, commit: bool) -> Result<()> {
        lock(&self.tables).clear();
        lock(&self.write_tables).clear();
        *lock(&self.view) = None;
        if self.stmt_savepoint.swap(false, Ordering::Relaxed) && self.in_transaction() {
            let mut tx = self
                .exec
                .take_transaction()
                .ok_or(Error::TransactionEnded)?;
            let result = if commit {
                tx.release_savepoint(STMT_SAVEPOINT)
            } else {
                tx.rollback_to_savepoint(STMT_SAVEPOINT)
                    .and_then(|()| tx.release_savepoint(STMT_SAVEPOINT))
            };
            self.exec.install_transaction(tx);
            result?;
        }
        let Some(mut tx) = lock(&self.stmt_tx).take() else {
            return Ok(());
        };
        // One that only read has nothing to commit, and rolls back cheaper
        let wrote = self.stmt_wrote.swap(false, Ordering::Relaxed);
        if !commit || !wrote {
            return tx.rollback();
        }
        let result = tx.commit();
        if result.is_err() {
            let _ = tx.rollback();
        }
        result
    }

    fn take(&self) -> Result<Box<dyn Transaction>> {
        self.open.store(false, Ordering::Relaxed);
        self.stmt_savepoint.store(false, Ordering::Relaxed);
        // The handles go before the transaction ends
        lock(&self.tables).clear();
        lock(&self.write_tables).clear();
        self.exec.take_transaction().ok_or(Error::TransactionEnded)
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.end_statement(false);
        if let Some(mut tx) = self.exec.take_transaction() {
            let _ = tx.rollback();
        }
    }
}

/// The savepoint a statement writing in an open transaction takes; a
/// user's savepoint has a name MariaDB makes up per connection, never this
const STMT_SAVEPOINT: &str = "__stoolap_mariadb_statement";

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

    /// The current row, taken out of the result
    pub fn take_row(&mut self) -> Row {
        self.result.take_row()
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_parent_with_upper_case_letters_writes_through_sql() {
        let db = Database::open("memory://").unwrap();
        let session = Session::new(&db);
        for sql in [
            r#"CREATE TABLE "d_1P" (id INTEGER PRIMARY KEY, u INTEGER UNIQUE)"#,
            r#"CREATE TABLE "d_1c" (id INTEGER PRIMARY KEY, pu INTEGER REFERENCES "d_1P"(u))"#,
        ] {
            session.run(sql, ParamVec::new()).unwrap();
        }
        assert!(session.writes_through_sql("d_1P").unwrap());
        assert!(session.writes_through_sql("d_1c").unwrap());
        let direct = session.with_write_table("d_1P", |_| Ok(())).unwrap();
        assert!(direct.is_none(), "the parent's writes go through SQL");
    }
}
