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

/// A key lookup: `=` on every bound part but the last, `op` there
pub const STOOLAP_BOUND_KEY: i32 = 0;
/// Only the first part, inclusive whatever `op` says (`GE` lower, `LE`
/// upper; an upper bound keeps the NULLs, which sort first)
pub const STOOLAP_BOUND_LEADING: i32 = 1;
/// A range bound: a composite one compares in MariaDB's lexicographic key
/// order, (a, b) >= (1, 5) being a > 1 OR (a = 1 AND b >= 5)
pub const STOOLAP_BOUND_RANGE: i32 = 2;

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
#[derive(Clone)]
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

/// A comparison in MariaDB's index order: always true, never, or one
/// condition (its nodes)
enum Pred {
    True,
    False,
    Nodes(Vec<Node>),
}

/// `f op v` in MariaDB's index order, where NULL sorts before every value
/// and a value past INT64_MAX after every stored one. (Against a value, an
/// upper bound leaves the NULLs out, as stoolap compares: a range whose
/// lower bound keeps them loses them.)
fn compare(f: &StoolapField, op: i32, v: KeyValue) -> Pred {
    let upper = matches!(op, STOOLAP_OP_LT | STOOLAP_OP_LE);
    match v {
        KeyValue::Value(v) => Pred::Nodes(vec![cond(f, cond_op(op), v)]),
        KeyValue::Null => match op {
            STOOLAP_OP_EQ | STOOLAP_OP_LE => {
                Pred::Nodes(vec![cond(f, CondOp::IsNull, Value::null_unknown())])
            }
            STOOLAP_OP_GT => Pred::Nodes(vec![cond(f, CondOp::IsNotNull, Value::null_unknown())]),
            STOOLAP_OP_GE => Pred::True,
            _ => Pred::False,
        },
        KeyValue::Beyond => {
            if upper {
                Pred::True
            } else {
                Pred::False
            }
        }
    }
}

/// Appends `compare(f, op, v)` to `out`; false when it is never true
fn compare_into(f: &StoolapField, op: i32, v: KeyValue, out: &mut Vec<Node>) -> bool {
    match v {
        // The usual case, without the Vec a Pred holds
        KeyValue::Value(v) => {
            out.push(cond(f, cond_op(op), v));
            true
        }
        v => match compare(f, op, v) {
            Pred::False => false,
            Pred::True => true,
            Pred::Nodes(mut nodes) => {
                out.append(&mut nodes);
                true
            }
        },
    }
}

/// The AND of `preds` (None: never true), as nodes and their count of
/// top-level conditions
fn all(preds: Vec<Pred>) -> Option<(Vec<Node>, usize)> {
    let mut nodes = Vec::new();
    let mut n = 0;
    for p in preds {
        match p {
            Pred::False => return None,
            Pred::True => {}
            Pred::Nodes(mut v) => {
                nodes.append(&mut v);
                n += 1;
            }
        }
    }
    Some((nodes, n))
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

    if bound.mode == STOOLAP_BOUND_RANGE && n > 1 && bound.op != STOOLAP_OP_EQ {
        // Lexicographic: a OR of legs, leg k = (the parts before k equal,
        // part k strictly past, the last part by `op`), and the bound it
        // implies on the leading part, which an index can read (an OR
        // alone is not)
        let mut values = Vec::with_capacity(n);
        for f in &parts[..n] {
            values.push(key_value(f, src)?);
        }
        let lower = matches!(bound.op, STOOLAP_OP_GT | STOOLAP_OP_GE);
        let strict = if lower { STOOLAP_OP_GT } else { STOOLAP_OP_LT };
        let mut legs = Vec::new();
        for leg in 0..n {
            let preds = (0..=leg)
                .map(|i| {
                    let op = if i < leg {
                        STOOLAP_OP_EQ
                    } else if leg < n - 1 {
                        strict
                    } else {
                        bound.op
                    };
                    compare(&parts[i], op, values[i].clone())
                })
                .collect();
            match all(preds) {
                None => {}
                // A leg every row meets: so does the bound
                Some((_, 0)) => return Ok(true),
                Some(leg) => legs.push(leg),
            }
        }
        if legs.is_empty() {
            return Ok(false);
        }
        let lead = if lower { STOOLAP_OP_GE } else { STOOLAP_OP_LE };
        if let Pred::Nodes(mut nodes) = compare(&parts[0], lead, values[0].clone()) {
            out.append(&mut nodes);
        }
        let nlegs = legs.len();
        for (mut nodes, count) in legs {
            out.append(&mut nodes);
            if count > 1 {
                out.push(Node::And(count));
            }
        }
        if nlegs > 1 {
            out.push(Node::Or(nlegs));
        }
        return Ok(true);
    }

    // A lookup, the probe of every join: straight into `out`
    let start = out.len();
    for (i, f) in parts[..n].iter().enumerate() {
        if bound.skip & (1 << i) != 0 {
            continue;
        }
        let op = if i == n - 1 { bound.op } else { STOOLAP_OP_EQ };
        if !compare_into(f, op, key_value(f, src)?, out) {
            out.truncate(start);
            return Ok(false);
        }
    }
    Ok(true)
}
