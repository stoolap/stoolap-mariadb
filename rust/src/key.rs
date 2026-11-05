// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! A read's conditions from MariaDB key bounds. The plugin restores each
//! bound's key into a record (key_restore) and describes the key's parts;
//! the values are read here, as a write reads its record, and turned into
//! the conditions a key lookup or a range means.

use stoolap::core::{Result, Value};

use crate::read::{Cond, CondOp, Node};
use crate::record::{read_value, RecordIn, StoolapField, STOOLAP_FIELD_INT};

pub const STOOLAP_OP_EQ: i32 = 0;
pub const STOOLAP_OP_GT: i32 = 1;
pub const STOOLAP_OP_GE: i32 = 2;
pub const STOOLAP_OP_LT: i32 = 3;
pub const STOOLAP_OP_LE: i32 = 4;

/// Every bound part compared exactly: `=` on all but the last, `op` there
pub const STOOLAP_BOUND_KEY: i32 = 0;
/// Only the first part, inclusive whatever `op` says (`GE` lower, `LE`
/// upper; an upper bound keeps the NULLs, which sort first)
pub const STOOLAP_BOUND_LEADING: i32 = 1;

/// One bound of a read on a key
#[repr(C)]
pub struct StoolapKeyBound {
    /// A record the bound's key was restored into
    pub record: *const u8,
    /// The key parts it binds, from the first
    pub nparts: i32,
    /// `STOOLAP_OP_*` for its last part
    pub op: i32,
    /// Parts it leaves out, the caller's filter compares them (bit i)
    pub skip: u32,
    /// `STOOLAP_BOUND_*`
    pub mode: i32,
}

/// A key part's value in a bound
enum KeyValue {
    Null,
    /// Past every value stoolap holds: a BIGINT UNSIGNED above INT64_MAX,
    /// which the plugin refuses to write
    Beyond,
    Value(Value),
}

unsafe fn key_value(f: &StoolapField, src: &RecordIn) -> Result<KeyValue> {
    if f.null_mask != 0 && *src.record.add(f.null_offset as usize) & f.null_mask as u8 != 0 {
        return Ok(KeyValue::Null);
    }
    if f.kind == STOOLAP_FIELD_INT && f.length == 8 && f.is_unsigned != 0 {
        let top = *src.record.add(f.offset as usize + 7);
        if top & 0x80 != 0 {
            return Ok(KeyValue::Beyond);
        }
    }
    Ok(KeyValue::Value(read_value(f, src)?))
}

fn cond_op(op: i32) -> CondOp {
    match op {
        STOOLAP_OP_GT => CondOp::Gt,
        STOOLAP_OP_GE => CondOp::Ge,
        STOOLAP_OP_LT => CondOp::Lt,
        STOOLAP_OP_LE => CondOp::Le,
        _ => CondOp::Eq,
    }
}

fn cond(f: &StoolapField, op: CondOp, value: Value) -> Node {
    Node::Cond(Cond {
        column: f.column as usize,
        op,
        value,
    })
}

/// What `op` keeps of the rows when the bound's value is NULL (before
/// every value, in MariaDB's index order) or beyond them all: no row, the
/// NULLs, the others, or every row
enum Keeps {
    None,
    Null,
    NotNull,
    All,
}

fn keeps(op: i32, null: bool) -> Keeps {
    let upper = matches!(op, STOOLAP_OP_LT | STOOLAP_OP_LE);
    let inclusive = matches!(op, STOOLAP_OP_EQ | STOOLAP_OP_GE | STOOLAP_OP_LE);
    match (null, op == STOOLAP_OP_EQ, upper, inclusive) {
        // = and <= keep the NULLs, > the rest, >= all, < nothing
        (true, true, _, _) => Keeps::Null,
        (true, false, true, true) => Keeps::Null,
        (true, false, true, false) => Keeps::None,
        (true, false, false, true) => Keeps::All,
        (true, false, false, false) => Keeps::NotNull,
        // Beyond every value: < and <= keep all, the rest nothing
        (false, _, true, _) => Keeps::All,
        (false, _, false, _) => Keeps::None,
    }
}

/// Appends the conditions `bound` means over the key `parts`; false when
/// no row can meet them
///
/// # Safety
///
/// `bound.record` must be a record buffer of the layout `parts` describe.
pub unsafe fn bound_nodes(
    parts: &[StoolapField],
    bound: &StoolapKeyBound,
    src: &RecordIn,
    out: &mut Vec<Node>,
) -> Result<bool> {
    let n = (bound.nparts.max(0) as usize).min(parts.len());
    if n == 0 {
        return Ok(true);
    }
    if bound.mode == STOOLAP_BOUND_LEADING {
        let f = &parts[0];
        let upper = bound.op == STOOLAP_OP_LE;
        // A value it cannot bind only widens the candidates the caller
        // filters
        if let Ok(KeyValue::Value(v)) = key_value(f, src) {
            out.push(cond(f, if upper { CondOp::Le } else { CondOp::Ge }, v));
            if upper && f.null_mask != 0 {
                out.push(cond(f, CondOp::IsNull, Value::null_unknown()));
                out.push(Node::Or(2));
            }
        }
        return Ok(true);
    }

    // A composite non-equality bound is lexicographic: (a, b) >= (1, 5) is
    // a > 1 OR (a = 1 AND b >= 5), not a >= 1 AND b >= 5
    if n > 1 && bound.op != STOOLAP_OP_EQ {
        let mut values = Vec::with_capacity(n);
        for f in &parts[..n] {
            match key_value(f, src)? {
                KeyValue::Null => return Ok(false),
                KeyValue::Beyond => {
                    return Err(stoolap::core::Error::NotSupported(
                        "a BIGINT UNSIGNED bound past INT64_MAX".into(),
                    ))
                }
                KeyValue::Value(v) => values.push(v),
            }
        }
        let lower = matches!(bound.op, STOOLAP_OP_GT | STOOLAP_OP_GE);
        let strict = if lower { CondOp::Gt } else { CondOp::Lt };
        // The bound it implies on the leading part, which an index can
        // read (an OR alone is not)
        let lead = if lower { CondOp::Ge } else { CondOp::Le };
        out.push(cond(&parts[0], lead, values[0].clone()));
        for leg in 0..n {
            for (i, f) in parts[..=leg].iter().enumerate() {
                let op = if i < leg {
                    CondOp::Eq
                } else if leg < n - 1 {
                    strict
                } else {
                    cond_op(bound.op)
                };
                out.push(cond(f, op, values[i].clone()));
            }
            if leg > 0 {
                out.push(Node::And(leg + 1));
            }
        }
        out.push(Node::Or(n));
        return Ok(true);
    }

    for (i, f) in parts[..n].iter().enumerate() {
        if bound.skip & (1 << i) != 0 {
            continue;
        }
        let op = if i == n - 1 { bound.op } else { STOOLAP_OP_EQ };
        let null = match key_value(f, src)? {
            KeyValue::Value(v) => {
                out.push(cond(f, cond_op(op), v));
                continue;
            }
            KeyValue::Null => true,
            KeyValue::Beyond => false,
        };
        match keeps(op, null) {
            Keeps::None => return Ok(false),
            Keeps::Null => out.push(cond(f, CondOp::IsNull, Value::null_unknown())),
            Keeps::NotNull => out.push(cond(f, CondOp::IsNotNull, Value::null_unknown())),
            Keeps::All => {}
        }
    }
    Ok(true)
}
