"""CRUD smoke: INSERT / SELECT / UPDATE / DELETE / TRUNCATE / ALTER, plus
multi-row insert and INSERT...SELECT."""


def run(h):
    h.exec_script(f"""
CREATE TABLE t (
    id INT NOT NULL PRIMARY KEY,
    name VARCHAR(64) NOT NULL,
    qty INT
) ENGINE={h.engine};
""")

    h.section("INSERT / SELECT")
    h.assert_ok("single row",      "INSERT INTO t VALUES (1, 'a', 10)")
    h.assert_ok("multi-row",       "INSERT INTO t VALUES (2, 'b', 20), (3, 'c', NULL), (4, 'd', 40)")
    h.assert_scalar("row count",   "SELECT COUNT(*) FROM t", "4")
    h.assert_scalar("select scalar", "SELECT name FROM t WHERE id = 2", "b")
    h.assert_scalar("NULL preserved", "SELECT IFNULL(qty, 'NIL') FROM t WHERE id = 3", "NIL")
    h.assert_scalar("ORDER BY DESC", "SELECT id FROM t ORDER BY id DESC LIMIT 1", "4")

    h.section("UPDATE")
    h.assert_ok("update single",   "UPDATE t SET qty = 99 WHERE id = 1")
    h.assert_scalar("updated value", "SELECT qty FROM t WHERE id = 1", "99")
    h.assert_ok("update many",     "UPDATE t SET qty = qty + 1 WHERE id IN (2, 4)")
    h.assert_scalar("updated id 2", "SELECT qty FROM t WHERE id = 2", "21")
    h.assert_scalar("updated id 4", "SELECT qty FROM t WHERE id = 4", "41")
    h.assert_scalar("id 3 unchanged", "SELECT IFNULL(qty,'NIL') FROM t WHERE id = 3", "NIL")

    h.section("DELETE")
    h.assert_ok("delete one",      "DELETE FROM t WHERE id = 3")
    h.assert_scalar("after delete", "SELECT COUNT(*) FROM t", "3")
    h.assert_ok("delete range",    "DELETE FROM t WHERE id >= 2")
    h.assert_scalar("only id=1 remains", "SELECT id FROM t", "1")

    h.section("INSERT ... SELECT")
    h.exec_script(f"CREATE TABLE t2 (id INT PRIMARY KEY, name VARCHAR(64), qty INT) ENGINE={h.engine};")
    h.assert_ok("reseed source",   "INSERT INTO t VALUES (10,'x',100),(11,'y',110),(12,'z',120)")
    h.assert_ok("INSERT ... SELECT", "INSERT INTO t2 SELECT * FROM t")
    h.assert_scalar("copy size",   "SELECT COUNT(*) FROM t2", "4")
    h.assert_scalar("copy spot-check", "SELECT name FROM t2 WHERE id = 11", "y")

    h.section("TRUNCATE")
    h.assert_ok("truncate",        "TRUNCATE TABLE t2")
    h.assert_scalar("truncate emptied", "SELECT COUNT(*) FROM t2", "0")
    h.assert_ok("reuse after truncate", "INSERT INTO t2 VALUES (1,'fresh',1)")
    h.assert_scalar("post-truncate read", "SELECT name FROM t2", "fresh")

    h.section("ALTER TABLE")
    h.assert_ok("ADD COLUMN",      "ALTER TABLE t2 ADD COLUMN note VARCHAR(20) DEFAULT 'd'")
    h.assert_scalar("default applied", "SELECT note FROM t2 WHERE id = 1", "d")
    h.assert_ok("DROP COLUMN",     "ALTER TABLE t2 DROP COLUMN note")
    h.assert_err("drop missing column",
                 "ALTER TABLE t2 DROP COLUMN nope",
                 r"ERROR|missing|not exist|doesn't exist|cannot find|Unknown column")

    h.section("DROP TABLE")
    h.assert_ok("drop t2",         "DROP TABLE t2")
    h.assert_ok("drop IF EXISTS no-op", "DROP TABLE IF EXISTS noexist")

    h.section("No-PK row-pump UPDATE/DELETE refused")
    # Stoolap has no stable row-id, and its UPDATE/DELETE grammar has
    # no LIMIT clause. append_where_for_row used to identify a row by
    # every column value when no usable single-column PK existed; with
    # byte-identical duplicates that WHERE matched every duplicate, so
    # MariaDB's filesort+LIMIT path (UPDATE/DELETE ... ORDER BY ...
    # LIMIT N) called update_row/delete_row N times and stoolap
    # over-mutated every duplicate per call. Fix: refuse the row-pump
    # path when no usable PK is available, with a clear error pointing
    # users at the workarounds (add a PK; rewrite as direct DML).
    h.exec_script(f"""
CREATE TABLE nopk (a INT NOT NULL, b INT NOT NULL) ENGINE={h.engine};
INSERT INTO nopk VALUES (1,10),(1,10),(2,20);
""")
    h.assert_err("UPDATE ... ORDER BY LIMIT refused on no-PK",
                 "UPDATE nopk SET b = b + 100 ORDER BY a LIMIT 1",
                 r"row-pump UPDATE/DELETE.*PRIMARY KEY")
    # Refused statement must NOT have mutated any rows.
    h.assert_scalar("no-PK refusal left rows unchanged",
                    "SELECT COUNT(*) FROM nopk WHERE b = 10", "2")
    h.assert_err("DELETE ... ORDER BY LIMIT refused on no-PK",
                 "DELETE FROM nopk ORDER BY a LIMIT 1",
                 r"row-pump UPDATE/DELETE.*PRIMARY KEY")
    h.assert_scalar("no-PK DELETE refusal left rows unchanged",
                    "SELECT COUNT(*) FROM nopk", "3")
    # Direct DML (WHERE-only, no ORDER BY/LIMIT) still works.
    h.assert_ok("WHERE-only UPDATE on no-PK still works (direct path)",
                "UPDATE nopk SET b = 999 WHERE a = 2")
    h.assert_scalar("WHERE-only UPDATE applied",
                    "SELECT b FROM nopk WHERE a = 2", "999")
    # PK tables are unaffected: row-pump UPDATE...LIMIT continues to
    # work when there's a single-column PK to identify rows uniquely.
    h.exec_script(f"""
CREATE TABLE pk (id INT NOT NULL PRIMARY KEY, b INT NOT NULL) ENGINE={h.engine};
INSERT INTO pk VALUES (1,10),(2,10),(3,10);
""")
    h.assert_ok("PK row-pump UPDATE LIMIT still works",
                "UPDATE pk SET b = b + 100 ORDER BY id LIMIT 1")
    h.assert_scalar("PK row-pump LIMIT touched exactly one row",
                    "SELECT COUNT(*) FROM pk WHERE b = 110", "1")
