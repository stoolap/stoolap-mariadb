/*
 * Copyright 2026 Stoolap Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

class THD;

// sql_class.h-dependent helpers: live in stoolap_thd_inspect.cc to keep
// the heavy sql_class/wsrep include chain out of most TUs.
extern "C" int stoolap_thd_has_explicit_limit(THD* thd);
extern "C" int stoolap_thd_is_update_or_delete(THD* thd);
extern "C" int stoolap_thd_needs_per_row_dup_handling(THD* thd);

// THDVAR-backed; live in ha_stoolap.cc because THDVAR resolves against
// the sysvar symbols generated there.
extern "C" int stoolap_thd_trust_binary_strings(THD* thd);
extern "C" int stoolap_thd_explain_pushdown(THD* thd);
