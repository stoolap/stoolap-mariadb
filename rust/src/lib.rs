// Copyright 2026 Stoolap Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

//! The MariaDB plugin's interface to the stoolap engine, linked into
//! ha_stoolap.so as a static library. It links stoolap as a Rust crate,
//! so the plugin can reach the engine below SQL text: the C functions in
//! include/stoolap_mariadb.h.

pub mod ffi;
pub mod read;
pub mod record;
pub mod session;
pub mod write;
