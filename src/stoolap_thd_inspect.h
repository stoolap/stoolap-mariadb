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

// THD inspection helpers that need MariaDB's sql_class.h live in
// stoolap_thd_inspect.cc so most translation units do not pull in the
// heavy sql_class/wsrep include chain.
extern "C" int stoolap_thd_has_explicit_limit(THD* thd);
extern "C" int stoolap_thd_is_update_or_delete(THD* thd);
extern "C" int stoolap_thd_needs_per_row_dup_handling(THD* thd);

// These THDVAR-backed helpers deliberately stay near the sysvar
// definitions in ha_stoolap.cc; THDVAR(thd, varname) resolves against
// the generated sysvar symbols in that translation unit.
extern "C" int stoolap_thd_trust_binary_strings(THD* thd);
extern "C" int stoolap_thd_explain_pushdown(THD* thd);
