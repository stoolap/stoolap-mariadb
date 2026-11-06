"""select_handler whole-SELECT pushdown.

When a SELECT references only stoolap tables and falls outside the
fall-through carve-outs (SP context, FOR UPDATE, INTO @var, ci-collated
string predicates, ...), the plugin's create_select factory hands the
rewritten SQL to stoolap and returns one pre-fetched StoolapRows handle.
EXPLAIN reports `select_type: PUSHED SELECT`.

When eligibility fails, the factory returns NULL and MariaDB executes
the query through the row-pump path; EXPLAIN shows the normal table-
access plan.
"""

import os
import re


def _direct_hits(h):
    """Read the global Stoolap_direct_modify_hits counter via SHOW STATUS."""
    rows = h._fetch("SHOW STATUS LIKE 'Stoolap_direct_modify_hits'")
    if not rows:
        return 0
    return int(rows[0][1])


def _explain_has(h, stmt, marker):
    """Return True if EXPLAIN <stmt> contains marker."""
    text = h.sql(f"EXPLAIN {stmt}")
    return marker in text


def _explain_with_session(h, setup, stmt, marker):
    """EXPLAIN with a SET-stmt-prefixed setup; True if marker present."""
    out = h.sql_with_session(setup, f"EXPLAIN {stmt}")
    return marker in out


def run(h):
    h.exec_script(f"""
SET SESSION max_recursive_iterations = 1000000;
CREATE TABLE users (
    id INT NOT NULL PRIMARY KEY,
    age INT NOT NULL,
    balance DOUBLE NOT NULL,
    active BOOLEAN NOT NULL,
    KEY idx_users_age (age)
) ENGINE={h.engine};
INSERT INTO users (id, age, balance, active)
WITH RECURSIVE seq AS (SELECT 1 AS i UNION ALL SELECT i+1 FROM seq WHERE i < 1000)
SELECT i, (i % 62) + 18, i * 1.5, IF(i % 2 = 0, TRUE, FALSE) FROM seq;

CREATE TABLE orders (
    oid INT NOT NULL PRIMARY KEY,
    uid INT NOT NULL,
    amount DOUBLE NOT NULL,
    KEY idx_orders_uid (uid)
) ENGINE={h.engine};
INSERT INTO orders (oid, uid, amount)
WITH RECURSIVE seq AS (SELECT 1 AS i UNION ALL SELECT i+1 FROM seq WHERE i < 500)
SELECT i, (i % 1000) + 1, i * 0.7 FROM seq;

CREATE TABLE inn (id INT PRIMARY KEY, n INT) ENGINE=InnoDB;
INSERT INTO inn VALUES (1, 10), (2, 20);

CREATE TABLE strs (
    id INT NOT NULL PRIMARY KEY,
    n INT NOT NULL,
    s VARCHAR(40) NOT NULL,
    KEY idx_strs_n (n)
) ENGINE={h.engine};
INSERT INTO strs VALUES (1,1,'apple'),(2,2,'banana'),(3,3,'BANANA'),(4,4,'cherry');
""")

    h.section("Aggregations push to stoolap")
    h.assert_pushed("GROUP BY",     "SELECT age, COUNT(*) FROM users GROUP BY age")
    h.assert_scalar("GROUP BY result row count",
                    "SELECT COUNT(*) FROM (SELECT age, COUNT(*) c FROM users GROUP BY age) z",
                    "62")
    h.assert_pushed("AVG with HAVING",
                    "SELECT age, AVG(balance) a FROM users GROUP BY age HAVING a > 100")
    h.assert_pushed("DISTINCT", "SELECT DISTINCT age FROM users")

    h.section("Joins push to stoolap")
    h.assert_pushed("INNER JOIN",
                    "SELECT u.id, o.amount FROM users u JOIN orders o ON o.uid = u.id WHERE u.id < 100")
    h.assert_pushed("LEFT JOIN + GROUP BY",
                    "SELECT u.id, COUNT(o.oid) c FROM users u LEFT JOIN orders o "
                    "ON o.uid = u.id GROUP BY u.id")

    h.section("Subqueries push to stoolap")
    h.assert_pushed("EXISTS subquery",
                    "SELECT id FROM users u WHERE EXISTS "
                    "(SELECT 1 FROM orders o WHERE o.uid = u.id)")
    h.assert_pushed("scalar subquery",
                    "SELECT id, (SELECT COUNT(*) FROM orders o WHERE o.uid = users.id) c "
                    "FROM users WHERE id < 10")

    h.section("Numeric / id-driven queries push (no string columns referenced)")
    h.assert_pushed("WHERE on int col",  "SELECT id, n FROM strs WHERE n = 2")
    h.assert_pushed("ORDER BY int col",  "SELECT id FROM strs ORDER BY n LIMIT 5")

    h.section("ci-collation guard catches subqueries in projection")
    h.exec_script(f"""
CREATE TABLE proj_o (oid INT NOT NULL PRIMARY KEY, uid INT NOT NULL, status VARCHAR(20) NOT NULL) ENGINE={h.engine};
CREATE TABLE proj_u (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO proj_u VALUES (1), (2);
INSERT INTO proj_o VALUES (10, 1, 'COMPLETED'), (20, 1, 'pending'), (30, 2, 'completed');
""")
    h.assert_not_pushed("scalar subq with ci-string in projection",
                        "SELECT id, (SELECT MAX(status) FROM proj_o WHERE proj_o.uid = proj_u.id) sm FROM proj_u")

    h.section("ci-collation guard recurses into subquery predicates")
    h.exec_script(f"""
CREATE TABLE sqg_u (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
CREATE TABLE sqg_o (oid INT NOT NULL PRIMARY KEY, uid INT NOT NULL, status VARCHAR(20) NOT NULL) ENGINE={h.engine};
INSERT INTO sqg_u VALUES (1, 0), (2, 0);
INSERT INTO sqg_o VALUES (1, 1, 'COMPLETED'), (2, 2, 'pending');
""")
    h.assert_not_pushed("EXISTS subq with ci string",
                        "SELECT id FROM sqg_u WHERE EXISTS "
                        "(SELECT 1 FROM sqg_o WHERE sqg_o.status = 'completed')")
    h.assert_scalar("EXISTS subq ci match count",
                    "SELECT COUNT(*) FROM sqg_u WHERE EXISTS "
                    "(SELECT 1 FROM sqg_o WHERE sqg_o.status = 'completed')", "2")
    h.assert_not_pushed("IN subq with ci string",
                        "SELECT id FROM sqg_u WHERE id IN "
                        "(SELECT uid FROM sqg_o WHERE sqg_o.status = 'completed')")
    h.assert_scalar("IN subq ci match count",
                    "SELECT COUNT(*) FROM sqg_u WHERE id IN "
                    "(SELECT uid FROM sqg_o WHERE sqg_o.status = 'completed')", "1")

    h.section("ci-collation guard recurses into derived TABLE_LIST units")
    h.exec_script(f"""
CREATE TABLE dvg (id INT NOT NULL PRIMARY KEY, status VARCHAR(20)) ENGINE={h.engine};
INSERT INTO dvg VALUES (1, 'COMPLETED'), (2, 'pending'), (3, 'completed');
""")
    h.assert_not_pushed("derived WHERE with ci string",
                        "SELECT COUNT(*) FROM (SELECT status FROM dvg WHERE status = 'completed') d")
    h.assert_scalar("derived ci match count (outer)",
                    "SELECT COUNT(*) FROM (SELECT status FROM dvg WHERE status = 'completed') d", "2")

    h.assert_not_pushed("nested derived WHERE with ci string",
                        "SELECT COUNT(*) FROM "
                        "(SELECT status FROM (SELECT status FROM dvg WHERE status = 'completed') i) d")
    h.assert_scalar("nested derived ci match count",
                    "SELECT COUNT(*) FROM "
                    "(SELECT status FROM (SELECT status FROM dvg WHERE status = 'completed') i) d", "2")

    if _explain_with_session(h, ["SET stoolap_trust_binary_strings = 1"],
                             "SELECT COUNT(*) FROM (SELECT status FROM dvg WHERE status = 'completed') d",
                             "PUSHED SELECT"):
        h._pass("trust_binary_strings=1 pushes derived ci predicate")
    else:
        h._fail("trust_binary_strings=1 should push derived")

    h.section("EXISTS bare ci-field projection does not block pushdown")
    h.exec_script(f"""
CREATE TABLE exu (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
CREATE TABLE exo (oid INT NOT NULL PRIMARY KEY, uid INT NOT NULL, s VARCHAR(20)) ENGINE={h.engine};
INSERT INTO exu VALUES (1),(2);
INSERT INTO exo VALUES (1,1,'X'),(2,2,'y');
""")
    h.assert_pushed("EXISTS bare ci field projection (numeric predicate)",
                    "SELECT COUNT(*) FROM exu WHERE EXISTS "
                    "(SELECT s FROM exo WHERE exo.uid = exu.id)")
    h.assert_scalar("EXISTS bare ci proj count",
                    "SELECT COUNT(*) FROM exu WHERE EXISTS "
                    "(SELECT s FROM exo WHERE exo.uid = exu.id)", "2")
    h.assert_not_pushed("EXISTS with ci compare in WHERE still bails",
                        "SELECT COUNT(*) FROM exu WHERE EXISTS "
                        "(SELECT 1 FROM exo WHERE exo.s = 'x')")
    h.assert_not_pushed("IN with bare ci field projection still bails",
                        "SELECT COUNT(*) FROM exu WHERE 'x' IN "
                        "(SELECT s FROM exo WHERE exo.uid = exu.id)")
    h.assert_scalar("IN bare ci proj ci-correct count",
                    "SELECT COUNT(*) FROM exu WHERE 'x' IN "
                    "(SELECT s FROM exo WHERE exo.uid = exu.id)", "1")
    h.assert_not_pushed("ANY with bare ci field projection still bails",
                        "SELECT COUNT(*) FROM exu WHERE 'x' = ANY "
                        "(SELECT s FROM exo WHERE exo.uid = exu.id)")
    h.assert_pushed("EXISTS expression projection over ci string",
                    "SELECT COUNT(*) FROM exu WHERE EXISTS "
                    "(SELECT LOWER(s) FROM exo WHERE exo.uid = exu.id)")
    h.assert_scalar("EXISTS LOWER(s) proj count",
                    "SELECT COUNT(*) FROM exu WHERE EXISTS "
                    "(SELECT LOWER(s) FROM exo WHERE exo.uid = exu.id)", "2")
    h.assert_pushed("EXISTS scalar subquery in projection",
                    "SELECT COUNT(*) FROM exu WHERE EXISTS "
                    "(SELECT (SELECT MAX(oid) FROM exo) FROM exo WHERE exo.uid = exu.id)")
    h.assert_not_pushed("IN with LOWER(s) projection still bails",
                        "SELECT COUNT(*) FROM exu WHERE 'x' IN "
                        "(SELECT LOWER(s) FROM exo WHERE exo.uid = exu.id)")
    # The ci column is only projected (no comparison), so it pushes.
    h.assert_pushed("scalar subq with ci s projection pushes",
                    "SELECT id, (SELECT s FROM exo WHERE exo.uid = exu.id LIMIT 1) FROM exu")
    # LOWER cases by the collation (Turkish i), which stoolap does not.
    h.assert_not_pushed("scalar subq with LOWER(s) projection bails",
                        "SELECT id, (SELECT LOWER(s) FROM exo WHERE exo.uid = exu.id LIMIT 1) FROM exu")

    h.section("ci-collation guard recurses into derived nested inside subqueries")
    h.exec_script(f"""
CREATE TABLE sdv_u (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
CREATE TABLE sdv_o (id INT NOT NULL PRIMARY KEY, status VARCHAR(20)) ENGINE={h.engine};
INSERT INTO sdv_u VALUES (1),(2);
INSERT INTO sdv_o VALUES (1, 'COMPLETED'), (2, 'pending');
""")
    h.assert_not_pushed("EXISTS over derived ci WHERE",
                        "SELECT COUNT(*) FROM sdv_u WHERE EXISTS "
                        "(SELECT 1 FROM (SELECT status FROM sdv_o WHERE status = 'completed') d)")
    h.assert_scalar("EXISTS-derived ci match count",
                    "SELECT COUNT(*) FROM sdv_u WHERE EXISTS "
                    "(SELECT 1 FROM (SELECT status FROM sdv_o WHERE status = 'completed') d)", "2")

    h.assert_not_pushed("IN over derived ci WHERE",
                        "SELECT COUNT(*) FROM sdv_u WHERE id IN "
                        "(SELECT 1 FROM (SELECT status FROM sdv_o WHERE status = 'completed') d)")
    h.assert_scalar("IN-derived ci match count",
                    "SELECT COUNT(*) FROM sdv_u WHERE id IN "
                    "(SELECT 1 FROM (SELECT status FROM sdv_o WHERE status = 'completed') d)", "1")

    h.assert_not_pushed("scalar subq over derived ci WHERE",
                        "SELECT id, (SELECT MAX(status) FROM "
                        "(SELECT status FROM sdv_o WHERE status = 'completed') d) FROM sdv_u")

    if _explain_with_session(h, ["SET stoolap_trust_binary_strings = 1"],
                             "SELECT COUNT(*) FROM sdv_u WHERE EXISTS "
                             "(SELECT 1 FROM (SELECT status FROM sdv_o WHERE status = 'completed') d)",
                             "PUSHED SELECT"):
        h._pass("trust_binary_strings=1 pushes EXISTS-derived ci predicate")
    else:
        h._fail("trust_binary_strings=1 should push EXISTS-derived")

    # Generated SQL quotes and aliases every name, so a column named like
    # its table is no hazard.
    h.section("Shadow column (column shares table name) pushes")
    h.exec_script(f"""
CREATE TABLE shsel (id INT NOT NULL PRIMARY KEY, shsel INT NOT NULL) ENGINE={h.engine};
INSERT INTO shsel VALUES (1, 100), (2, 200);
""")
    h.assert_pushed("SELECT col-named-as-table pushes", "SELECT shsel FROM shsel WHERE id = 1")
    h.assert_scalar("pushed SELECT returns the column value",
                    "SELECT shsel FROM shsel WHERE id = 1", "100")
    h.assert_pushed("non-shadow schema still pushes",
                    "SELECT id FROM users WHERE id = 1")

    h.section("DECIMAL columns are not pushed for compare predicates")
    h.exec_script(f"""
CREATE TABLE dec_t (id INT NOT NULL PRIMARY KEY, v DECIMAL(10,2)) ENGINE={h.engine};
INSERT INTO dec_t VALUES (1, 1.50), (2, 2.75), (3, 1.50);
""")
    h.assert_not_pushed("WHERE DECIMAL = literal", "SELECT id FROM dec_t WHERE v = 1.50")
    h.assert_scalar("WHERE DECIMAL row pump count",
                    "SELECT COUNT(*) FROM dec_t WHERE v = 1.50", "2")
    h.assert_not_pushed("BETWEEN on DECIMAL", "SELECT id FROM dec_t WHERE v BETWEEN 1.0 AND 2.0")
    h.assert_scalar("BETWEEN DECIMAL row pump count",
                    "SELECT COUNT(*) FROM dec_t WHERE v BETWEEN 1.0 AND 2.0", "2")
    h.assert_not_pushed("ORDER BY DECIMAL", "SELECT id FROM dec_t ORDER BY v")
    h.assert_not_pushed("GROUP BY DECIMAL", "SELECT v, COUNT(*) FROM dec_t GROUP BY v")
    h.assert_pushed("pure DECIMAL projection still pushes", "SELECT id, v FROM dec_t")
    h.assert_not_pushed("subquery DECIMAL compare",
                        "SELECT id FROM dec_t WHERE id IN (SELECT id FROM dec_t WHERE v >= 2)")

    h.section("JOIN ON predicates respect ci / DECIMAL guards")
    h.exec_script(f"""
CREATE TABLE jl (id INT NOT NULL PRIMARY KEY, s VARCHAR(20)) ENGINE={h.engine};
CREATE TABLE jr (id INT NOT NULL PRIMARY KEY, s VARCHAR(20)) ENGINE={h.engine};
INSERT INTO jl VALUES (1, 'COMPLETED');
INSERT INTO jr VALUES (1, 'completed');
""")
    h.assert_not_pushed("LEFT JOIN ON ci compare bails",
                        "SELECT jl.id, jr.id FROM jl LEFT JOIN jr ON jr.s = jl.s")
    out = h.sql("SELECT jl.id, jr.id FROM jl LEFT JOIN jr ON jr.s = jl.s")
    h.assert_eq("LEFT JOIN row pump ci-folded match", "1\t1", out)

    h.assert_not_pushed("INNER JOIN ON ci compare bails",
                        "SELECT jl.id, jr.id FROM jl JOIN jr ON jr.s = jl.s")
    h.assert_pushed("JOIN ON INT still pushes",
                    "SELECT jl.id, jr.id FROM jl JOIN jr ON jl.id = jr.id")

    h.exec_script(f"""
CREATE TABLE jdl (id INT NOT NULL PRIMARY KEY, v DECIMAL(10,2)) ENGINE={h.engine};
CREATE TABLE jdr (id INT NOT NULL PRIMARY KEY, v DECIMAL(10,2)) ENGINE={h.engine};
INSERT INTO jdl VALUES (1, 1.50);
INSERT INTO jdr VALUES (1, 1.50);
""")
    h.assert_not_pushed("DECIMAL JOIN ON bails",
                        "SELECT jdl.id, jdr.id FROM jdl JOIN jdr ON jdl.v = jdr.v")

    if _explain_with_session(h, ["SET stoolap_trust_binary_strings = 1"],
                             "SELECT jl.id, jr.id FROM jl LEFT JOIN jr ON jr.s = jl.s",
                             "PUSHED SELECT"):
        h._pass("trust_binary_strings=1 pushes ci JOIN ON")
    else:
        h._fail("trust_binary_strings=1 should push (ci JOIN ON)")

    h.section("stoolap_explain_pushdown surfaces engine plan to error log")
    h.exec_script(f"""
CREATE TABLE explg (id INT NOT NULL PRIMARY KEY, n INT NOT NULL,
                     KEY idx_n (n)) ENGINE={h.engine};
INSERT INTO explg VALUES (1,10),(2,20),(3,30),(4,40);
""")
    if h.errlog and os.path.exists(h.errlog):
        # Trigger a pushdown with the explain flag on.
        h.run_client(f"""
USE {h.db};
SET stoolap_explain_pushdown = 1;
SELECT COUNT(*) FROM explg WHERE n >= 20;
""", use_db=False)
        with open(h.errlog, "r", errors="replace") as f:
            errlog_text = f.read()
        if re.search(r"stoolap\[explain\]:.*explg", errlog_text):
            h._pass("pushdown plan reaches err log when explain_pushdown=1")
        else:
            h._fail(f"no stoolap[explain] line found in {h.errlog}")

        prev = errlog_text.count("stoolap[explain]")
        h.run_client(f"""
USE {h.db};
SET stoolap_explain_pushdown = 0;
SELECT COUNT(*) FROM explg WHERE n >= 30;
""", use_db=False)
        with open(h.errlog, "r", errors="replace") as f:
            new_count = f.read().count("stoolap[explain]")
        if new_count == prev:
            h._pass("explain_pushdown=0 produces no plan dump")
        else:
            h._fail(f"unexpected plan dump (count {prev} -> {new_count})")

    h.section("cond_push declines when direct path will reject (predicate alignment)")
    h.exec_script(f"""
CREATE TABLE pda (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO pda VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE pda SET n = 999 WHERE id = 1 ORDER BY id LIMIT 1",
                 use_db=False)
    after = _direct_hits(h)
    if after == before:
        h._pass("ORDER BY+LIMIT UPDATE did not claim direct (correctly declined)")
    else:
        h._fail(f"ORDER BY+LIMIT UPDATE incorrectly took direct (counter {before} -> {after})")
    h.assert_scalar("ORDER BY+LIMIT UPDATE only id=1",       "SELECT n FROM pda WHERE id = 1", "999")
    h.assert_scalar("ORDER BY+LIMIT UPDATE id=2 untouched",  "SELECT n FROM pda WHERE id = 2", "2")
    h.assert_scalar("ORDER BY+LIMIT UPDATE id=5 untouched",  "SELECT n FROM pda WHERE id = 5", "5")

    h.section("WHERE-bearing direct DML routes through direct path")
    h.exec_script(f"""
CREATE TABLE pdc (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO pdc VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE pdc SET n = 99 WHERE id = 1", use_db=False)
    h.run_client(f"USE {h.db}; UPDATE pdc SET n = n + 100 WHERE n > 2", use_db=False)
    h.run_client(f"USE {h.db}; DELETE FROM pdc WHERE id = 5", use_db=False)
    after = _direct_hits(h)
    delta = after - before
    if delta >= 3:
        h._pass(f"WHERE UPDATE/DELETE took direct path (delta={delta})")
    else:
        h._fail(f"WHERE UPDATE/DELETE didn't route through direct (delta={delta}, expected >=3)")
    h.assert_scalar("WHERE UPDATE id=1 then >2",  "SELECT n FROM pdc WHERE id = 1", "199")
    h.assert_scalar("WHERE UPDATE n>2 row 3",     "SELECT n FROM pdc WHERE id = 3", "103")
    h.assert_scalar("WHERE UPDATE skipped row 2", "SELECT n FROM pdc WHERE id = 2", "2")
    h.assert_scalar("WHERE DELETE id=5 result",   "SELECT COUNT(*) FROM pdc WHERE id = 5", "0")

    h.section("Direct DML with a column sharing a table name")
    # The statement is written from MariaDB's Item tree, so a column named
    # like a table is a column: these run as direct DML.
    h.exec_script(f"""
CREATE TABLE shadow (id INT NOT NULL PRIMARY KEY, shadow INT NOT NULL) ENGINE={h.engine};
INSERT INTO shadow VALUES (1, 100), (2, 200), (3, 300);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE shadow SET shadow = 999 WHERE id = 1", use_db=False)
    after = _direct_hits(h)
    h.assert_eq("UPDATE column-name=table-name ran direct", 1, after - before)
    h.assert_scalar("UPDATE column=table applied",
                    "SELECT shadow FROM shadow WHERE id = 1", "999")
    h.assert_scalar("UPDATE skipped other rows",
                    "SELECT shadow FROM shadow WHERE id = 2", "200")

    h.exec_script(f"""
CREATE TABLE shdw (id INT NOT NULL PRIMARY KEY, SHDW INT NOT NULL) ENGINE={h.engine};
INSERT INTO shdw VALUES (1, 11), (2, 22);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE shdw SET SHDW = 77 WHERE id = 1", use_db=False)
    after = _direct_hits(h)
    h.assert_eq("case-insensitive column=table ran direct", 1,
                after - before)
    h.assert_scalar("ci column=table UPDATE applied",
                    "SELECT SHDW FROM shdw WHERE id = 1", "77")

    h.exec_script(f"""
CREATE TABLE clean (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO clean VALUES (1, 10), (2, 20);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE clean SET n = 99 WHERE id = 1", use_db=False)
    after = _direct_hits(h)
    if after > before:
        h._pass(f"clean schema still takes direct path (delta={after - before})")
    else:
        h._fail("clean schema lost direct path (no delta)")

    h.exec_script(f"""
CREATE TABLE shadow_outer (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
CREATE TABLE shadow_inner (uid INT NOT NULL PRIMARY KEY, shadow_inner INT NOT NULL) ENGINE={h.engine};
INSERT INTO shadow_outer VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO shadow_inner VALUES (1, 1), (2, 2), (3, 3);
""")
    before = _direct_hits(h)
    h.run_client(f"""
USE {h.db};
UPDATE shadow_outer SET n = 999 WHERE EXISTS (
    SELECT 1 FROM shadow_inner
    WHERE shadow_inner.uid = shadow_outer.id AND shadow_inner = 1)""",
                 use_db=False)
    after = _direct_hits(h)
    h.assert_eq("EXISTS-subquery shadow leaf ran direct", 1, after - before)
    h.assert_scalar("EXISTS-subquery shadow UPDATE applied (id=1)",
                    "SELECT n FROM shadow_outer WHERE id = 1", "999")
    h.assert_scalar("EXISTS-subquery shadow other rows untouched",
                    "SELECT n FROM shadow_outer WHERE id = 2", "20")

    h.run_client(f"""
USE {h.db};
UPDATE shadow_outer SET n = 555 WHERE id IN (
    SELECT uid FROM shadow_inner WHERE shadow_inner = 2)""", use_db=False)
    h.assert_scalar("IN-subquery shadow UPDATE applied (id=2)",
                    "SELECT n FROM shadow_outer WHERE id = 2", "555")

    # Cross-leaf shadow: outer leaf's column matches a SUBQUERY leaf's
    # table name (`SET o = o + 1` with a subquery on table `o`).
    h.exec_script(f"""
CREATE TABLE x_outer (id INT NOT NULL PRIMARY KEY, o INT NOT NULL) ENGINE={h.engine};
CREATE TABLE o (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO x_outer VALUES (1, 100), (2, 200);
INSERT INTO o VALUES (1, 1), (2, 2);
""")
    before = _direct_hits(h)
    h.run_client(f"""
USE {h.db};
UPDATE x_outer SET o = o + 1 WHERE EXISTS (
    SELECT 1 FROM o WHERE o.id = x_outer.id)""", use_db=False)
    after = _direct_hits(h)
    h.assert_eq("cross-leaf shadow ran direct", 1, after - before)
    h.assert_scalar("cross-leaf shadow UPDATE id=1 applied",
                    "SELECT o FROM x_outer WHERE id = 1", "101")
    h.assert_scalar("cross-leaf shadow UPDATE id=2 applied",
                    "SELECT o FROM x_outer WHERE id = 2", "201")

    h.section("Mixed-quote db.tbl direct DML")
    # All four quoting combos run cleanly when the target db differs
    # from the session's current db.
    h.exec_script(f"""
CREATE DATABASE IF NOT EXISTS mixq_other;
CREATE TABLE mixq_other.q (id INT PRIMARY KEY, n INT) ENGINE={h.engine};
INSERT INTO mixq_other.q VALUES (1, 0);
""")
    other = h.run_async(f"""
USE {h.db};
UPDATE mixq_other.q       SET n = 1 WHERE id = 1;
UPDATE `mixq_other`.q     SET n = 2 WHERE id = 1;
UPDATE mixq_other.`q`     SET n = 3 WHERE id = 1;
UPDATE `mixq_other`.`q`   SET n = 4 WHERE id = 1;
SELECT n FROM mixq_other.q;
""", use_db=False)
    out = other.wait()
    last = out.strip().splitlines()[-1] if out.strip() else ""
    h.assert_eq("all four db.tbl quoting forms updated through direct DML",
                "4", last)
    h.exec_script("DROP DATABASE mixq_other")

    h.section("Direct DML names tables as the handler does")
    # Each table is named from its opened path, as the handler does: a
    # database or table name MariaDB encodes (my-db, P\u00e9, t$1) and,
    # under lower_case_table_names, the statement's own letter case.
    c, cur = h.open_session()
    try:
        script = [
            "DROP DATABASE IF EXISTS `DmlCase`", "CREATE DATABASE `DmlCase`",
            "USE `DmlCase`",
            f"CREATE TABLE pQ (id INT PRIMARY KEY, v INT) ENGINE={h.engine}",
            "INSERT INTO pQ VALUES (1, 0), (2, 0), (3, 0), (4, 0)",
            "DROP DATABASE IF EXISTS `my-db`", "CREATE DATABASE `my-db`",
            f"CREATE TABLE `my-db`.`P\u00e9` (id INT PRIMARY KEY, v INT) "
            f"ENGINE={h.engine}",
            "INSERT INTO `my-db`.`P\u00e9` VALUES (1, 0), (2, 0), (3, 0)",
            f"CREATE TABLE `my-db`.`t$1` (id INT PRIMARY KEY) ENGINE={h.engine}",
            "INSERT INTO `my-db`.`t$1` VALUES (1), (2)",
        ]
        for stmt in script:
            cur.execute(stmt)
        before = _direct_hits(h)
        for stmt in [
            "DELETE FROM pQ WHERE id = 1",
            "DELETE FROM DmlCase.pQ WHERE id = 2",
            "UPDATE `pQ` SET v = 7 WHERE id = 3",
            "DELETE FROM `my-db`.`P\u00e9` WHERE id = 1",
            "UPDATE `my-db`.`P\u00e9` SET v = 5 WHERE id = 2",
            "USE `my-db`",
            "DELETE FROM `P\u00e9` WHERE id = 3",
            "DELETE FROM t$1 WHERE id = 1",
        ]:
            cur.execute(stmt)
        hits = _direct_hits(h) - before
        cur.execute("SELECT id, v FROM `DmlCase`.pQ ORDER BY id")
        h.assert_eq("upper-case database: bare, qualified and quoted names",
                    [(3, 7), (4, 0)], cur.fetchall())
        cur.execute("SELECT id, v FROM `my-db`.`P\u00e9` ORDER BY id")
        h.assert_eq("encoded database and table names", [(2, 5)],
                    cur.fetchall())
        cur.execute("SELECT id FROM `my-db`.`t$1`")
        h.assert_eq("a table name with $", [(2,)], cur.fetchall())
        h.assert_eq("every statement ran as direct DML", 7, hits)
        # A word that reads like the table: the keyword
        # SET for a table `set` (any case, as MariaDB may fold), a call of
        # a function the table shares its name with: all direct DML.
        for stmt in [
            f"CREATE TABLE `set` (id INT PRIMARY KEY, v INT) ENGINE={h.engine}",
            "INSERT INTO `set` VALUES (1, 0), (2, 0)",
            f"CREATE TABLE concat (id INT PRIMARY KEY, v INT) "
            f"ENGINE={h.engine}",
            "INSERT INTO concat VALUES (1, 0)",
            f"CREATE TABLE `abs` (id INT PRIMARY KEY, v INT) ENGINE={h.engine}",
            "INSERT INTO `abs` VALUES (1, 0), (2, 0), (3, 0), (4, 0), (5, 0)",
        ]:
            cur.execute(stmt)
        before = _direct_hits(h)
        for stmt in [
            "UPDATE `set` SET v = 1 WHERE id = 1",
            "UPDATE `set` set v = 2 WHERE id = 2",
            "UPDATE concat SET v = concat('1', '2') WHERE id = 1",
            # A comment between the call's name and its `(`
            "UPDATE `abs` SET v = ABS/**/(-id) WHERE id = 1",
            "UPDATE `abs` SET v = abs -- x\n (-id) WHERE id = 2",
            "UPDATE `abs` SET v = abs /*! (-id) */ WHERE id = 3",
            "UPDATE `abs` SET v = `ABS`(-id) WHERE id = 4",
            # MariaDB takes -- before a control character as a comment
            "UPDATE `abs` SET v = abs--\x01 x\n(-id) WHERE id = 5",
        ]:
            cur.execute(stmt)
        # concat's string into an INT converts: the row-pump
        h.assert_eq("keyword and call words ran direct", 7,
                    _direct_hits(h) - before)
        cur.execute("SELECT id, v FROM `set` ORDER BY id")
        h.assert_eq("a table named set", [(1, 1), (2, 2)], cur.fetchall())
        cur.execute("SELECT v FROM concat")
        h.assert_eq("a table named like a function", [(12,)], cur.fetchall())
        cur.execute("SELECT id, v FROM `abs` ORDER BY id")
        h.assert_eq("a call with a comment before its (",
                    [(1, 1), (2, 2), (3, 3), (4, 4), (5, 5)],
                    cur.fetchall())
        cur.execute("DROP DATABASE `DmlCase`")
        cur.execute("DROP DATABASE `my-db`")
    finally:
        c.close()

    h.section("Direct DML takes any statement text MariaDB parses")
    # DELETE QUICK / LOW_PRIORITY, UPDATE LOW_PRIORITY, "t" under
    # ANSI_QUOTES, a spaced d . t, comments: with and without WHERE.
    # DELETE IGNORE is left to the row-pump (it skips a row's error).
    c, cur = h.open_session()
    try:
        for stmt in [
            "DROP DATABASE IF EXISTS dml_opt", "CREATE DATABASE dml_opt",
            "USE dml_opt",
            f"CREATE TABLE t (id INT PRIMARY KEY, v INT) ENGINE={h.engine}",
            "INSERT INTO t VALUES (1, 0), (2, 0), (3, 0), (4, 0), (5, 0), "
            "(6, 0), (7, 0)",
        ]:
            cur.execute(stmt)
        before = _direct_hits(h)
        for stmt in [
            "DELETE QUICK FROM t WHERE id = 1",
            "DELETE LOW_PRIORITY FROM t WHERE id = 2",
            "DELETE IGNORE FROM t WHERE id = 3",
            "UPDATE LOW_PRIORITY t SET v = 1 WHERE id = 4",
            "UPDATE dml_opt . t SET v = 2 WHERE id = 5",
            "DELETE FROM dml_opt/**/.t WHERE id = 6",
            "SET SESSION sql_mode = CONCAT(@@sql_mode, ',ANSI_QUOTES')",
            'UPDATE "t" SET v = 3 WHERE id = 7',
            "SET SESSION sql_mode = DEFAULT",
        ]:
            cur.execute(stmt)
        h.assert_eq("all but DELETE IGNORE ran direct", 6,
                    _direct_hits(h) - before)
        cur.execute("SELECT id, v FROM t ORDER BY id")
        h.assert_eq("each ran", [(4, 1), (5, 2), (7, 3)], cur.fetchall())
        for stmt in ["DELETE QUICK FROM t", "INSERT INTO t VALUES (1, 0)",
                     "DELETE FROM dml_opt . t"]:
            cur.execute(stmt)
        cur.execute("SELECT COUNT(*) FROM t")
        h.assert_eq("without WHERE too", [(0,)], cur.fetchall())
        # A comment holding an apostrophe, a name holding a backtick, a
        # mixed-quote form that is a longer name's prefix
        for stmt in [
            "INSERT INTO t VALUES (1, 0), (2, 0), (3, 0)",
            f"CREATE TABLE tx (id INT PRIMARY KEY) ENGINE={h.engine}",
            "INSERT INTO tx VALUES (2), (9)",
            f"CREATE TABLE `a``b` (id INT PRIMARY KEY) ENGINE={h.engine}",
            "INSERT INTO `a``b` VALUES (1), (2)",
            "DELETE /* it's fine */ FROM t WHERE id = 1",
            "DELETE FROM `dml_opt`.tx WHERE id IN "
            "(SELECT id FROM `dml_opt`.t)",
            "DELETE FROM `a``b` WHERE id = 1",
        ]:
            cur.execute(stmt)
        cur.execute("SELECT id FROM t ORDER BY id")
        h.assert_eq("a comment with an apostrophe", [(2,), (3,)],
                    cur.fetchall())
        cur.execute("SELECT id FROM tx")
        h.assert_eq("a qualified name's prefix", [(9,)], cur.fetchall())
        cur.execute("SELECT id FROM `a``b`")
        h.assert_eq("a name with a backtick", [(2,)], cur.fetchall())
        for stmt in ["DELETE /* it's fine */ FROM t", "DELETE FROM `a``b`"]:
            cur.execute(stmt)
        cur.execute("SELECT (SELECT COUNT(*) FROM t), "
                    "(SELECT COUNT(*) FROM `a``b`)")
        h.assert_eq("both without WHERE", [(0, 0)], cur.fetchall())
        # Executable comments: MariaDB runs their text, stoolap skips it
        before = _direct_hits(h)
        for stmt in [
            "INSERT INTO t VALUES (1, 0), (2, 0), (3, 0), (4, 0)",
            "UPDATE t SET v = 1 /*!WHERE id = 1 */",
            "DELETE FROM t /*M!100000 WHERE id = 2 */",
            "DELETE /*! FROM */ t WHERE id = 3",
        ]:
            cur.execute(stmt)
        cur.execute("SELECT id, v FROM t ORDER BY id")
        h.assert_eq("executable comments keep their text", [(1, 1), (4, 0)],
                    cur.fetchall())
        h.assert_eq("and ran direct", 3, _direct_hits(h) - before)
        cur.execute("DELETE /*! FROM */ t")
        cur.execute("SELECT COUNT(*) FROM t")
        h.assert_eq("without WHERE too", [(0,)], cur.fetchall())
        cur.execute("DROP DATABASE dml_opt")
    finally:
        c.close()

    h.section("Direct DML respects ci-collation guard")
    h.exec_script(f"""
CREATE TABLE pds (id INT NOT NULL PRIMARY KEY, s VARCHAR(32), n INT NOT NULL) ENGINE={h.engine};
INSERT INTO pds VALUES (1, 'BANANA', 0), (2, 'banana', 0), (3, 'apple', 0);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE pds SET n = 7 WHERE s = 'banana'", use_db=False)
    h.run_client(f"USE {h.db}; DELETE FROM pds WHERE s = 'APPLE'", use_db=False)
    after = _direct_hits(h)
    if after == before:
        h._pass("ci-string DML did not claim direct (correctly declined)")
    else:
        h._fail(f"ci-string DML incorrectly took direct (counter {before} -> {after})")
    h.assert_scalar("ci UPDATE matched both cases", "SELECT COUNT(*) FROM pds WHERE n = 7", "2")
    h.assert_scalar("ci DELETE matched upper literal", "SELECT COUNT(*) FROM pds", "2")

    h.section("Direct DML guards SET expressions for ci / DECIMAL hazards")
    h.exec_script(f"""
CREATE TABLE setg_u (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
CREATE TABLE setg_o (oid INT NOT NULL PRIMARY KEY, status VARCHAR(20) NOT NULL) ENGINE={h.engine};
INSERT INTO setg_u VALUES (1, 0);
INSERT INTO setg_o VALUES (10, 'COMPLETED');
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE setg_u SET n = (SELECT COUNT(*) FROM setg_o WHERE status = 'completed')",
                 use_db=False)
    after = _direct_hits(h)
    if after == before:
        h._pass("ci subquery in SET avoided direct path")
    else:
        h._fail(f"ci subquery in SET took direct (counter {before} -> {after})")
    h.assert_scalar("ci subquery row pump count", "SELECT n FROM setg_u WHERE id = 1", "1")

    h.exec_script(f"""
CREATE TABLE setg_d (id INT NOT NULL PRIMARY KEY, v DECIMAL(10,2)) ENGINE={h.engine};
INSERT INTO setg_d VALUES (1, 3.14);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE setg_d SET v = v + 1 WHERE id = 1", use_db=False)
    after = _direct_hits(h)
    if after == before:
        h._pass("DECIMAL SET expression avoided direct path")
    else:
        h._fail(f"DECIMAL SET expression took direct (counter {before} -> {after})")
    h.assert_scalar("DECIMAL SET row pump applied", "SELECT v FROM setg_d WHERE id = 1", "4.14")

    h.exec_script(f"""
CREATE TABLE setg_clean (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO setg_clean VALUES (1, 10);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE setg_clean SET n = n * 2 WHERE id = 1", use_db=False)
    after = _direct_hits(h)
    if after > before:
        h._pass(f"numeric SET still takes direct (delta={after - before})")
    else:
        h._fail("numeric SET lost direct path")
    h.assert_scalar("numeric SET applied", "SELECT n FROM setg_clean WHERE id = 1", "20")

    h.section("Direct UPDATE bails when target field is DECIMAL")
    h.exec_script(f"""
CREATE TABLE dt (id INT NOT NULL PRIMARY KEY, v DECIMAL(38,10),
                   n INT NOT NULL) ENGINE={h.engine};
INSERT INTO dt VALUES (1, 0, 0);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE dt SET v = 1234567890123456789.1234567890 WHERE id = 1",
                 use_db=False)
    after = _direct_hits(h)
    if after == before:
        h._pass("DECIMAL target avoided direct path")
    else:
        h._fail(f"DECIMAL target took direct (counter {before} -> {after})")
    h.assert_scalar("DECIMAL row pump full precision",
                    "SELECT v FROM dt WHERE id = 1", "1234567890123456789.1234567890")

    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE dt SET n = 99 WHERE id = 1", use_db=False)
    after = _direct_hits(h)
    if after > before:
        h._pass("non-DECIMAL target on same table still direct")
    else:
        h._fail("non-DECIMAL target lost direct path")
    h.assert_scalar("non-DECIMAL update applied", "SELECT n FROM dt WHERE id = 1", "99")

    h.section("SELECT DISTINCT over ci string bails to row pump")
    h.exec_script(f"""
CREATE TABLE distci (id INT NOT NULL PRIMARY KEY, s VARCHAR(20)) ENGINE={h.engine};
INSERT INTO distci VALUES (1,'COMPLETED'),(2,'completed'),(3,'pending');
""")
    h.assert_not_pushed("SELECT DISTINCT over ci VARCHAR", "SELECT DISTINCT s FROM distci")
    h.assert_scalar("DISTINCT row pump ci-folded count",
                    "SELECT COUNT(*) FROM (SELECT DISTINCT s FROM distci) z", "2")
    h.assert_pushed("SELECT DISTINCT over INT still pushes", "SELECT DISTINCT id FROM distci")
    h.assert_pushed("non-DISTINCT over ci VARCHAR still pushes", "SELECT s FROM distci")

    if _explain_with_session(h, ["SET stoolap_trust_binary_strings = 1"],
                             "SELECT DISTINCT s FROM distci", "PUSHED SELECT"):
        h._pass("trust_binary_strings=1 pushes DISTINCT over ci")
    else:
        h._fail("trust_binary_strings=1 should push (DISTINCT)")

    h.section("Direct UPDATE/DELETE hooks fire (pre_direct_* wired)")
    h.exec_script(f"""
CREATE TABLE pdh (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO pdh VALUES (1, 1), (2, 2), (3, 3);
""")
    before = _direct_hits(h)
    h.run_client(f"USE {h.db}; UPDATE pdh SET n = n + 100", use_db=False)
    after = _direct_hits(h)
    if after > before:
        h._pass(f"UPDATE no-WHERE incremented direct-modify counter ({before} -> {after})")
    else:
        h._fail(f"UPDATE no-WHERE did not increment counter (still {after})")
    h.assert_scalar("UPDATE no-WHERE result", "SELECT MIN(n) FROM pdh", "101")

    h.section("Direct UPDATE/DELETE with subquery names all leaves")
    h.exec_script(f"""
CREATE TABLE sq_u (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
CREATE TABLE sq_o (oid INT NOT NULL PRIMARY KEY, uid INT NOT NULL) ENGINE={h.engine};
INSERT INTO sq_u VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO sq_o VALUES (10, 1), (20, 1), (30, 2);
""")
    h.assert_ok("UPDATE WHERE EXISTS subquery",
                "UPDATE sq_u SET n = 99 WHERE EXISTS "
                "(SELECT 1 FROM sq_o WHERE sq_o.uid = sq_u.id)")
    h.assert_scalar("UPDATE subquery row 1",       "SELECT n FROM sq_u WHERE id = 1", "99")
    h.assert_scalar("UPDATE subquery row 2",       "SELECT n FROM sq_u WHERE id = 2", "99")
    h.assert_scalar("UPDATE subquery skipped row", "SELECT n FROM sq_u WHERE id = 3", "0")

    h.assert_ok("DELETE WHERE IN subquery",
                "DELETE FROM sq_u WHERE id IN (SELECT uid FROM sq_o WHERE oid = 30)")
    h.assert_scalar("DELETE subquery rows left", "SELECT COUNT(*) FROM sq_u", "2")

    h.section("Direct UPDATE reads SET's assigned columns as MariaDB does")
    # MariaDB assigns SET left to right, a later value reading the new
    # column; stoolap reads the old row. Those go to the row-pump.
    h.exec_script(f"""
CREATE TABLE set_ord (id INT NOT NULL PRIMARY KEY, a INT, b INT) ENGINE={h.engine};
INSERT INTO set_ord VALUES (1, 1, 0), (2, 5, 9), (3, 1, 1);
""")
    before = _direct_hits(h)
    h.assert_ok("SET a then b = a",
                "UPDATE set_ord SET a = a + 1, b = a WHERE id = 1")
    h.assert_ok("SET a = b, b = a", "UPDATE set_ord SET a = b, b = a WHERE id = 2")
    h.assert_eq("both on the row-pump", 0, _direct_hits(h) - before)
    h.assert_ok("SET b = a, a = 7 reads only the old row",
                "UPDATE set_ord SET b = a + 1, a = 7 WHERE id = 3")
    h.assert_eq("and runs direct", 1, _direct_hits(h) - before)
    h.assert_scalar("SET values", "SELECT GROUP_CONCAT(a, ':', b ORDER BY id) "
                    "FROM set_ord", "2:2,9:9,7:2")

    h.section("Direct UPDATE converts SET values as MariaDB does")
    # MariaDB converts a value to its column; stoolap by its own rules
    # (1.20 into a VARCHAR is '1.2', 2.7 into an INT is 2). Only a value of
    # the column's class runs direct.
    h.exec_script(f"""
CREATE TABLE set_cv (id INT NOT NULL PRIMARY KEY, v VARCHAR(20), n INT,
                     f DOUBLE) ENGINE={h.engine};
INSERT INTO set_cv VALUES (1, 'a', 0, 0), (2, 'a', 0, 0), (3, 'a', 0, 0);
""")
    before = _direct_hits(h)
    rc, out = h.run_client("""
PREPARE s FROM 'UPDATE set_cv SET v = ? WHERE id = 1';
SET @p = CAST(1.20 AS DECIMAL(3,2));
EXECUTE s USING @p;
UPDATE set_cv SET v = 1.50, n = 2.7 WHERE id = 2;
""")
    h.assert_eq("conversions on the row-pump", 0, _direct_hits(h) - before)
    h.assert_ok("an integer into a float, a NULL",
                "UPDATE set_cv SET f = 3, n = NULL WHERE id = 3")
    h.assert_eq("run direct", 1, _direct_hits(h) - before)
    h.assert_ok("a constant expression is not evaluated with no row",
                "UPDATE set_cv SET f = EXP(1000) WHERE n = 9")
    rc, out = h.run_client("""
ALTER TABLE set_cv ADD COLUMN m INT DEFAULT 5;
PREPARE d FROM 'UPDATE set_cv SET n = ?, m = ? WHERE id = 3';
EXECUTE d USING DEFAULT, DEFAULT;
PREPARE i FROM 'UPDATE set_cv SET m = ? WHERE id = 2';
EXECUTE i USING IGNORE;
""")
    h.assert_eq("DEFAULT and IGNORE parameters run", 0, rc)
    h.assert_scalar("their values",
                    "SELECT GROUP_CONCAT(CONCAT_WS(':', IFNULL(n, 'N'), m) "
                    "ORDER BY id) FROM set_cv WHERE id > 1", "3:5,N:5")
    h.exec_script("ALTER TABLE set_cv DROP COLUMN m")
    h.assert_scalar("converted values",
                    "SELECT GROUP_CONCAT(CONCAT_WS(':', v, IFNULL(n, 'N'), f) "
                    "ORDER BY id) FROM set_cv", "1.20:0:0,1.50:3:0,a:N:3")

    h.section("Direct UPDATE keeps MariaDB's checks on the written row")
    # CHECK constraints and ON UPDATE CURRENT_TIMESTAMP go to the row-pump;
    # a value out of its column fails as in strict mode: a literal on the
    # row-pump, an expression in the statement (one per UPDATE).
    h.exec_script(f"""
CREATE TABLE wck (id INT NOT NULL PRIMARY KEY, n TINYINT NOT NULL,
                  s VARCHAR(4), v INT, c INT CHECK (c > 0)) ENGINE={h.engine};
INSERT INTO wck VALUES (1, 120, 'ab', 2147483646, 5);
CREATE TABLE wts (id INT PRIMARY KEY, a INT, ts TIMESTAMP NULL DEFAULT NULL
                  ON UPDATE CURRENT_TIMESTAMP) ENGINE={h.engine};
INSERT INTO wts VALUES (1, 1, '2000-01-01 00:00:00');
""")
    h.assert_err("CHECK constraint", "UPDATE wck SET c = -1 WHERE id = 1",
                 "4025")
    h.assert_ok("ON UPDATE CURRENT_TIMESTAMP", "UPDATE wts SET a = 2")
    h.assert_scalar("ON UPDATE column set",
                    "SELECT ts > '2000-01-01 00:00:00' FROM wts", "1")
    h.exec_script("ALTER TABLE wck DROP CONSTRAINT IF EXISTS c")
    h.exec_script("ALTER TABLE wck MODIFY c INT")
    before = _direct_hits(h)
    h.assert_ok("a counter that fits", "UPDATE wck SET n = n + 1 WHERE id = 1")
    h.assert_ok("an INT that fits", "UPDATE wck SET v = v + 1 WHERE id = 1")
    h.assert_ok("a string that fits", "UPDATE wck SET s = '\u00e9\u00e9\u00e9\u00e9' "
                "WHERE id = 1")
    h.assert_eq("run direct", 3, _direct_hits(h) - before)
    h.assert_err("a TINYINT past its range", "UPDATE wck SET n = n + 10 WHERE id = 1",
                 "1264")
    h.assert_err("an INT past its range", "UPDATE wck SET v = v + 1 WHERE id = 1",
                 "1264")
    h.assert_err("a literal past its range", "UPDATE wck SET n = 300 WHERE id = 1",
                 "1264")
    h.assert_err("a literal too long", "UPDATE wck SET s = 'abcde' WHERE id = 1",
                 "1406")
    h.assert_err("NULL into NOT NULL", "UPDATE wck SET n = NULL WHERE id = 1",
                 "1048")
    h.assert_scalar("the row kept its values",
                    "SELECT CONCAT_WS(':', n, s, v) FROM wck",
                    "121:\u00e9\u00e9\u00e9\u00e9:2147483647")

    h.section("Direct DML keeps string literals")
    h.exec_script(f"""
CREATE TABLE bts (id INT NOT NULL PRIMARY KEY, note VARCHAR(64) NOT NULL) ENGINE={h.engine};
INSERT INTO bts VALUES (1, 'old');
""")
    h.run_client(f"USE {h.db}; UPDATE bts SET note = 'use `bts` here' WHERE id = 1",
                 use_db=False)
    h.assert_scalar("literal containing backticked table preserved",
                    "SELECT note FROM bts WHERE id = 1", "use `bts` here")

    h.run_client(f"USE {h.db}; UPDATE `bts` SET note = 'after-bt' WHERE id = 1",
                 use_db=False)
    h.assert_scalar("backticked table-ref still routes via direct path",
                    "SELECT note FROM bts WHERE id = 1", "after-bt")

    h.run_client(
        f"USE {h.db}; UPDATE `bts` SET note = 'mention `bts` and again `bts`' "
        "WHERE id = 1", use_db=False)
    h.assert_scalar("table-ref rewritten, embedded literal preserved",
                    "SELECT note FROM bts WHERE id = 1",
                    "mention `bts` and again `bts`")

    h.section("stoolap_trust_binary_strings bypasses ci-collation guard")
    out_default = h.sql("EXPLAIN SELECT id FROM strs WHERE s = 'banana'")
    out_trusted = h.sql_with_session(
        ["SET stoolap_trust_binary_strings = 1"],
        "EXPLAIN SELECT id FROM strs WHERE s = 'banana'")
    if "PUSHED SELECT" in out_default:
        h._fail("default guard should bail on ci string predicate")
    else:
        h._pass("default guard bails on ci string predicate")
    if "PUSHED SELECT" in out_trusted:
        h._pass("trust_binary_strings=1 pushes ci string predicate")
    else:
        h._fail(f"trust_binary_strings=1 should push (got: {out_trusted})")

    h.section("Pushdown bails cleanly on unsupported shapes (row-pump fallback)")
    h.assert_not_pushed("ci string equality",    "SELECT id FROM strs WHERE s = 'banana'")
    h.assert_not_pushed("ci string LIKE",        "SELECT id FROM strs WHERE s LIKE 'b%'")
    h.assert_not_pushed("ci string in ORDER BY", "SELECT id FROM strs ORDER BY s LIMIT 5")
    h.assert_not_pushed("ci string in GROUP BY", "SELECT s, COUNT(*) FROM strs GROUP BY s")
    h.assert_not_pushed("cross-engine JOIN",     "SELECT u.id FROM users u JOIN inn i ON i.id = u.id")
    h.assert_not_pushed("FOR UPDATE",            "SELECT id FROM users WHERE id = 1 FOR UPDATE")

    h.section("Outer aggregate over stoolap-only derived pushes whole")
    h.assert_pushed("MAX over derived",
                    "SELECT MAX(b) FROM (SELECT balance b FROM users WHERE id < 100) z")
    h.assert_pushed("COUNT(*) over derived",
                    "SELECT COUNT(*) FROM (SELECT id FROM users GROUP BY id) z")
    h.assert_pushed("SUM over derived",
                    "SELECT SUM(c) FROM (SELECT age, COUNT(*) c FROM users GROUP BY age) z")
    h.assert_scalar("MAX over derived value",
                    "SELECT MAX(n) FROM (SELECT id*7 n FROM users WHERE id < 5) z", "28")
    h.assert_scalar("COUNT over derived value",
                    "SELECT COUNT(*) FROM (SELECT age FROM users GROUP BY age) z", "62")

    h.section("Pure projection of ci-collated strings still pushes")
    h.assert_pushed("project ci string in SELECT list", "SELECT id, s FROM strs WHERE n = 2")
    h.assert_pushed("project ci string with int ORDER BY", "SELECT id, s FROM strs ORDER BY n LIMIT 5")
    h.assert_not_pushed("MIN(ci string) in projection", "SELECT n, MIN(s) FROM strs GROUP BY n")

    h.section("Result correctness on pushed paths")
    h.assert_scalar("SUM via pushdown",  "SELECT SUM(amount) FROM orders", "87675")
    h.assert_scalar("MIN/MAX same row",  "SELECT MIN(id), MAX(id) FROM users", "1\t1000")
    h.assert_scalar("JOIN row count",
                    "SELECT COUNT(*) FROM users u JOIN orders o ON o.uid = u.id "
                    "WHERE u.id <= 50", "49")

    h.section("UNION pushdown (whole unit -> stoolap set-op executor)")
    h.exec_script(f"""
CREATE TABLE u1 (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO u1 VALUES (1, 10), (2, 20), (3, 30);
CREATE TABLE u2 (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO u2 VALUES (3, 30), (4, 40), (5, 50);
""")
    h.assert_pushed_union("UNION ALL",
                          "SELECT id, n FROM u1 UNION ALL SELECT id, n FROM u2")
    h.assert_pushed_union("UNION (DISTINCT)",
                          "SELECT id FROM u1 UNION SELECT id FROM u2")
    h.assert_pushed_union("UNION + WHERE + ORDER + LIMIT",
                          "SELECT id, n FROM u1 WHERE n >= 20 UNION ALL "
                          "SELECT id, n FROM u2 WHERE n <= 40 ORDER BY n LIMIT 4")
    h.assert_scalar("UNION ALL row count",
                    "SELECT COUNT(*) FROM (SELECT id FROM u1 UNION ALL SELECT id FROM u2) z",
                    "6")
    h.assert_scalar("UNION DISTINCT row count",
                    "SELECT COUNT(*) FROM (SELECT id FROM u1 UNION SELECT id FROM u2) z",
                    "5")

    h.section("UNION ci-collation guard (set-op semantics on ci strings)")
    h.exec_script(f"""
CREATE TABLE ucl (id INT NOT NULL PRIMARY KEY, s VARCHAR(20)) ENGINE={h.engine};
CREATE TABLE ucr (id INT NOT NULL PRIMARY KEY, s VARCHAR(20)) ENGINE={h.engine};
INSERT INTO ucl VALUES (1,'COMPLETED');
INSERT INTO ucr VALUES (2,'completed');
""")
    if "PUSHED UNION" in h.sql("EXPLAIN SELECT s FROM ucl UNION SELECT s FROM ucr"):
        h._fail("UNION over ci VARCHAR pushed (must bail)")
    else:
        h._pass("UNION over ci VARCHAR did not push")
    h.assert_scalar("UNION ci dedup count (row pump applies ci)",
                    "SELECT COUNT(*) FROM (SELECT s FROM ucl UNION SELECT s FROM ucr) z",
                    "1")

    if "PUSHED UNION" in h.sql("EXPLAIN SELECT s FROM ucl UNION ALL SELECT s FROM ucr ORDER BY s"):
        h._fail("UNION ALL + ORDER BY ci pushed (must bail)")
    else:
        h._pass("UNION ALL + ORDER BY ci did not push")

    if "PUSHED UNION" in h.sql("EXPLAIN SELECT s FROM ucl UNION ALL SELECT s FROM ucr"):
        h._pass("UNION ALL with no compare surface still pushes")
    else:
        h._fail("UNION ALL without compare bailed unexpectedly")

    if "PUSHED UNION" in h.sql("EXPLAIN SELECT id FROM ucl UNION SELECT id FROM ucr"):
        h._pass("UNION on INT col still pushes (binary-safe)")
    else:
        h._fail("UNION on INT col bailed unexpectedly")

    if _explain_with_session(h, ["SET stoolap_trust_binary_strings = 1"],
                             "SELECT s FROM ucl UNION SELECT s FROM ucr",
                             "PUSHED UNION"):
        h._pass("trust_binary_strings=1 pushes UNION over ci VARCHAR")
    else:
        h._fail("trust_binary_strings=1 should push (UNION ci)")

    h.section("Derived-table partial pushdown (hybrid stoolap + InnoDB)")
    h.exec_script("""
CREATE TABLE inn_u (id INT NOT NULL PRIMARY KEY, label VARCHAR(40)) ENGINE=InnoDB;
INSERT INTO inn_u VALUES (1, 'alice'), (2, 'bob'), (3, 'carol');
""")
    h.assert_pushed_derived("stoolap derived inside InnoDB outer JOIN",
                            "SELECT u.label, x.cnt FROM inn_u u "
                            "JOIN (SELECT uid, COUNT(*) cnt FROM orders GROUP BY uid) x "
                            "ON x.uid = u.id")
    out = h.sql(
        "SELECT u.label, x.cnt FROM inn_u u "
        "JOIN (SELECT uid, COUNT(*) cnt FROM orders GROUP BY uid) x "
        "ON x.uid = u.id ORDER BY u.id")
    h.assert_eq("hybrid result", "bob\t1\ncarol\t1", out)

    h.exec_script(f"""
CREATE TABLE pp_users (id INT NOT NULL PRIMARY KEY, role VARCHAR(16)) ENGINE=InnoDB;
CREATE TABLE pp_orders (id INT NOT NULL PRIMARY KEY, uid INT NOT NULL, amt INT NOT NULL) ENGINE={h.engine};
INSERT INTO pp_users VALUES (1,'admin'),(2,'user'),(3,'admin');
INSERT INTO pp_orders VALUES (10,1,100),(20,1,200),(30,2,300),(40,3,400);
""")
    if "PUSHED DERIVED" in h.sql(
            "EXPLAIN SELECT u.id FROM pp_users u "
            "JOIN (SELECT uid FROM pp_orders WHERE amt > 150) d ON d.uid = u.id "
            "WHERE u.role = 'admin' ORDER BY u.id"):
        h._pass("non-prepared derived still pushes")
    else:
        h._fail("non-prepared derived no longer pushes")

    rc, out = h.run_client("""
PREPARE pe FROM 'EXPLAIN SELECT u.id FROM pp_users u JOIN (SELECT uid FROM pp_orders WHERE amt > ?) d ON d.uid = u.id WHERE u.role = ? ORDER BY u.id';
SET @amt = 150, @role = 'admin';
EXECUTE pe USING @amt, @role;""")
    # The derived SQL is generated per EXECUTE with the bound values.
    if "PUSHED DERIVED" in out:
        h._pass("prepared derived pushed")
    else:
        h._fail("prepared derived not pushed", f"explain: {out}")

    rc, out = h.run_client("""
PREPARE pp FROM 'SELECT u.id FROM pp_users u JOIN (SELECT uid FROM pp_orders WHERE amt > ?) d ON d.uid = u.id WHERE u.role = ? ORDER BY u.id';
SET @amt = 150, @role = 'admin';
EXECUTE pp USING @amt, @role;
SET @amt = 350;
EXECUTE pp USING @amt, @role;""")
    rows = ",".join(line.strip() for line in out.splitlines() if line.strip())
    h.assert_eq("prepared derived, re-executed with new values", "1,3,3", rows)

    h.section("Prepared-statement pushdown (binary protocol)")
    expected_range = h.sql("SELECT COUNT(*) FROM users WHERE age >= 30 AND age <= 40")
    rc, out = h.run_client("""
PREPARE stmt FROM 'SELECT COUNT(*) FROM users WHERE age >= ? AND age <= ?';
SET @lo = 30, @hi = 40;
EXECUTE stmt USING @lo, @hi;
DEALLOCATE PREPARE stmt;
""")
    actual_range = out.strip().splitlines()[-1] if out.strip() else ""
    h.assert_eq("prepared range query result", expected_range, actual_range)

    rc, out = h.run_client("""
PREPARE stmt2 FROM 'SELECT id FROM users WHERE id = ?';
SET @i = 42;
EXECUTE stmt2 USING @i;
SET @i = 100;
EXECUTE stmt2 USING @i;
DEALLOCATE PREPARE stmt2;""")
    lines = out.strip().splitlines()
    h.assert_eq("prepared by-id, value 42",  "42",  lines[0] if lines else "")
    h.assert_eq("prepared by-id, value 100", "100", lines[1] if len(lines) > 1 else "")

    h.exec_script(f"""
CREATE TABLE pdml (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO pdml VALUES (1, 10), (2, 20), (3, 30), (4, 40);
""")
    rc, out = h.run_client("""
PREPARE upstmt FROM 'UPDATE pdml SET n = ? WHERE id = ?';
SET @new = 99, @key = 2;
EXECUTE upstmt USING @new, @key;
DEALLOCATE PREPARE upstmt;
SELECT n FROM pdml WHERE id = 2;""")
    h.assert_eq("prepared UPDATE bound params", "99", out.strip().splitlines()[-1])

    rc, out = h.run_client("""
PREPARE delstmt FROM 'DELETE FROM pdml WHERE id = ?';
SET @key = 3;
EXECUTE delstmt USING @key;
DEALLOCATE PREPARE delstmt;
SELECT COUNT(*) FROM pdml;""")
    h.assert_eq("prepared DELETE bound params", "3", out.strip().splitlines()[-1])

    h.section("DECIMAL prepared params bind as exact TEXT, not lossy FLOAT")
    h.exec_script(f"""
CREATE TABLE prdec (id BIGINT NOT NULL PRIMARY KEY, n INT) ENGINE={h.engine};
INSERT INTO prdec VALUES (9007199254740993, 1), (10, 2);
""")
    rc, out = h.run_client("""
PREPARE pd FROM 'SELECT id FROM prdec WHERE id = ?';
SET @x = CAST(9007199254740993 AS DECIMAL(20,0));
EXECUTE pd USING @x;
DEALLOCATE PREPARE pd;""")
    h.assert_eq("DECIMAL param matches BIGINT > 2^53",
                "9007199254740993", out.strip().splitlines()[-1])

    # The pushed SQL is generated from the Item tree, where an EXECUTE's
    # placeholders are bound values, so they are written as literals.
    h.section("Prepared placeholders in projection and ORDER BY push")
    h.exec_script(f"""
CREATE TABLE prsel (id INT NOT NULL PRIMARY KEY, n INT) ENGINE={h.engine};
INSERT INTO prsel VALUES (1, 10), (2, 20);
""")
    rc, out = h.run_client("""
PREPARE pp1 FROM 'EXPLAIN SELECT ? AS lit, id FROM prsel WHERE id = ?';
SET @v = 'hello', @id = 1;
EXECUTE pp1 USING @v, @id;
DEALLOCATE PREPARE pp1;""")
    if "PUSHED SELECT" in out:
        h._pass("projection placeholder pushed")
    else:
        h._fail("projection placeholder not pushed", f"explain: {out}")

    rc, out = h.run_client("""
PREPARE pp2 FROM 'SELECT ? AS lit, id FROM prsel WHERE id = ?';
SET @v = 'hello', @id = 1;
EXECUTE pp2 USING @v, @id;
DEALLOCATE PREPARE pp2;""")
    first_line = out.strip().splitlines()[0] if out.strip() else ""
    h.assert_eq("projection placeholder yields bound value", "hello\t1", first_line)

    rc, out = h.run_client("""
PREPARE pp3 FROM 'EXPLAIN SELECT id FROM prsel ORDER BY id + ?';
SET @x = 100;
EXECUTE pp3 USING @x;
DEALLOCATE PREPARE pp3;""")
    if "PUSHED SELECT" in out:
        h._pass("ORDER BY expr placeholder pushed")
    else:
        h._fail("ORDER BY expr placeholder not pushed", f"explain: {out}")
    rc, out = h.run_client("""
PREPARE pp3r FROM 'SELECT id FROM prsel ORDER BY n * ?';
SET @x = -1;
EXECUTE pp3r USING @x;
DEALLOCATE PREPARE pp3r;""")
    h.assert_eq("ORDER BY expr placeholder orders by the bound value",
                "2,1", ",".join(l.strip() for l in out.splitlines() if l.strip()))

    rc, out = h.run_client("""
PREPARE pp4 FROM 'EXPLAIN SELECT id, n FROM prsel WHERE id = ?';
SET @id = 1;
EXECUTE pp4 USING @id;
DEALLOCATE PREPARE pp4;""")
    if "PUSHED SELECT" in out:
        h._pass("WHERE-only placeholder still pushes")
    else:
        h._fail("WHERE-only placeholder lost pushdown")

    rc, out = h.run_client("""
PREPARE pu FROM 'EXPLAIN SELECT id FROM prsel WHERE id = ? UNION ALL SELECT id FROM prsel WHERE id = ?';
SET @a = 1, @b = 2;
EXECUTE pu USING @a, @b;
DEALLOCATE PREPARE pu;""")
    if "PUSHED UNION" in out:
        h._pass("prepared UNION with WHERE-only params still pushes")
    else:
        h._fail("prepared UNION lost pushdown")

    rc, out = h.run_client("""
PREPARE pu2 FROM 'SELECT id FROM prsel WHERE id = ? UNION ALL SELECT id FROM prsel WHERE id = ?';
SET @a = 1, @b = 2;
EXECUTE pu2 USING @a, @b;
DEALLOCATE PREPARE pu2;""")
    rows = sorted(line.strip() for line in out.splitlines() if line.strip())
    h.assert_eq("prepared UNION result rows", "1,2", ",".join(rows))

    rc, out = h.run_client("""
PREPARE pu3 FROM 'EXPLAIN SELECT ? FROM prsel WHERE id = 1 UNION ALL SELECT id FROM prsel WHERE id = 2';
SET @v = 'hello';
EXECUTE pu3 USING @v;
DEALLOCATE PREPARE pu3;""")
    if "PUSHED UNION" in out:
        h._fail("prepared UNION with projection placeholder still pushed")
    else:
        h._pass("prepared UNION with projection placeholder bailed")

    # tests/diff/record_reads.sql compares these results with InnoDB; here
    # they must push, so the rows come through the pushdown record path.
    h.section("record_reads.sql pushdown shapes push")
    h.exec_script(f"""
CREATE TABLE recr (id INT NOT NULL PRIMARY KEY, t TINYINT, i INT, m MEDIUMINT,
                   f FLOAT, d DOUBLE, v VARCHAR(10)) ENGINE={h.engine} DEFAULT CHARSET=utf8mb4;
INSERT INTO recr VALUES (1, 1, 1, 1, 1.5, 2.5, 'a');
CREATE TABLE reck (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO reck VALUES (1);
""")
    h.assert_pushed("record_reads whole SELECT",
                    "SELECT * FROM recr ORDER BY id")
    h.assert_pushed("record_reads expressions",
                    "SELECT id, t + 1, i - 1, -m, f * 2, d / 2, CONCAT(v, 'x'), "
                    "CHAR_LENGTH(v) FROM recr ORDER BY id")
    h.assert_pushed("review_fixes short decimal literals",
                    "SELECT 12345678901234.5, 0.000000000000000123456789, "
                    "-98765.4321 FROM recr")
    h.assert_not_pushed("review_fixes long decimal literal",
                        "SELECT 12345678901234567890.5 FROM recr")
    h.assert_pushed("bool_values select list",
                    "SELECT id, (t + 1 > 1) + 1, (i > 0) * 10, i > 0, (i > 0) = (m > 0) "
                    "FROM recr ORDER BY id")
    h.assert_pushed_derived("record_reads derived table",
                            "SELECT STRAIGHT_JOIN k.id, x.t, x.d, x.v FROM reck k JOIN "
                            "(SELECT id, t, d, v FROM recr WHERE id > 0) x "
                            "ON x.id = k.id ORDER BY k.id")
