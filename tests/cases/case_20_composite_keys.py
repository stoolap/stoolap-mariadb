"""Composite PRIMARY KEY / UNIQUE: stoolap enforces both (a composite
PRIMARY KEY becomes a NOT NULL unique index), so the plugin creates them
and must name the right key on a duplicate, find the conflicting row for
ON DUPLICATE KEY UPDATE / REPLACE, and identify rows by every PRIMARY KEY
part on the row-pump UPDATE / DELETE path.

Expected values match InnoDB on the same statements."""

import os
import re

import mysql.connector


def _assert_errno(h, label, sql, expected_errno, key=None):
    """Run sql; assert errno, and that the message names `key` if given."""
    try:
        h.cur.execute(sql)
        try:
            h.cur.fetchall()
        except mysql.connector.errors.InterfaceError:
            pass
        h._fail(label, f"sql:    {sql}", "server: <no error>")
    except mysql.connector.Error as e:
        if e.errno == expected_errno and (key is None or f"'{key}'" in str(e)):
            h._pass(label)
        else:
            h._fail(label,
                    f"sql:      {sql}",
                    f"expected: errno {expected_errno}"
                    + (f" naming key '{key}'" if key else ""),
                    f"actual:   errno {e.errno}: {e}")


def run(h):
    h.section("CREATE with a composite PRIMARY KEY and a composite UNIQUE")
    h.exec_script(f"""
CREATE TABLE ck (
    a INT NOT NULL,
    b INT NOT NULL,
    c INT,
    d VARCHAR(10) COLLATE utf8mb4_bin,
    n INT NOT NULL DEFAULT 0,
    PRIMARY KEY (a, b),
    UNIQUE KEY uq_cd (c, d)
) ENGINE={h.engine};
INSERT INTO ck VALUES (1, 1, 10, 'x', 0), (1, 2, 10, 'y', 0),
                      (2, 1, NULL, 'x', 0), (3, 1, NULL, 'x', 0);
""")
    h.assert_scalar("rows in, NULL repeats in a UNIQUE part",
                    "SELECT COUNT(*) FROM ck", "4")

    h.section("Duplicates name the right key")
    _assert_errno(h, "composite PRIMARY KEY duplicate",
                  "INSERT INTO ck VALUES (1, 1, 99, 'z', 0)", 1062,
                  key="PRIMARY")
    _assert_errno(h, "composite UNIQUE duplicate",
                  "INSERT INTO ck VALUES (5, 5, 10, 'x', 0)", 1062,
                  key="uq_cd")
    h.assert_scalar("duplicates left the table unchanged",
                    "SELECT COUNT(*) FROM ck", "4")

    h.section("ON DUPLICATE KEY UPDATE / REPLACE find the conflicting row")
    h.assert_ok("ODKU on the composite PRIMARY KEY",
                "INSERT INTO ck VALUES (1, 2, 77, 'q', 0) "
                "ON DUPLICATE KEY UPDATE n = n + 1")
    h.assert_scalar("ODKU updated row (1, 2)",
                    "SELECT n FROM ck WHERE a = 1 AND b = 2", "1")
    h.assert_ok("ODKU on the composite UNIQUE",
                "INSERT INTO ck VALUES (9, 9, 10, 'x', 0) "
                "ON DUPLICATE KEY UPDATE n = n + 5")
    h.assert_scalar("ODKU updated row (1, 1)",
                    "SELECT n FROM ck WHERE a = 1 AND b = 1", "5")
    h.assert_ok("REPLACE on the composite PRIMARY KEY",
                "REPLACE INTO ck VALUES (1, 1, 11, 'r', 7)")
    h.assert_scalar("REPLACE rewrote row (1, 1)",
                    "SELECT CONCAT(c, d, n) FROM ck WHERE a = 1 AND b = 1",
                    "11r7")
    h.assert_scalar("no row added by ODKU / REPLACE",
                    "SELECT COUNT(*) FROM ck", "4")

    h.section("Row-pump UPDATE / DELETE identify rows by every PK part")
    # LIMIT keeps these off direct DML: update_row / delete_row per row.
    h.assert_ok("row-pump UPDATE",
                "UPDATE ck SET n = n + 100 ORDER BY a, b LIMIT 2")
    h.assert_scalar("row-pump UPDATE touched (1, 1) and (1, 2) only",
                    "SELECT GROUP_CONCAT(n ORDER BY a, b) FROM ck",
                    "107,101,0,0")
    h.assert_ok("row-pump UPDATE of a PRIMARY KEY part",
                "UPDATE ck SET b = 5 WHERE a = 3 ORDER BY a LIMIT 1")
    h.assert_scalar("row (3, 1) moved to (3, 5)",
                    "SELECT GROUP_CONCAT(a, ':', b ORDER BY a, b) FROM ck",
                    "1:1,1:2,2:1,3:5")
    _assert_errno(h, "row-pump UPDATE onto an existing PRIMARY KEY",
                  "UPDATE ck SET a = 2, b = 1 WHERE a = 1 AND b = 2 LIMIT 1",
                  1062, key="PRIMARY")
    _assert_errno(h, "direct UPDATE onto an existing PRIMARY KEY",
                  "UPDATE ck SET a = 2, b = 1 WHERE a = 1 AND b = 2", 1062)
    h.assert_ok("row-pump DELETE",
                "DELETE FROM ck ORDER BY a, b LIMIT 1")
    h.assert_scalar("row-pump DELETE removed (1, 1) only",
                    "SELECT GROUP_CONCAT(a, ':', b ORDER BY a, b) FROM ck",
                    "1:2,2:1,3:5")

    h.section("Lookups through the composite PRIMARY KEY")
    h.assert_scalar("full-key lookup",
                    "SELECT n FROM ck WHERE a = 1 AND b = 2", "101")
    h.assert_scalar("leading-part lookup",
                    "SELECT COUNT(*) FROM ck WHERE a = 3", "1")
    h.exec_script("""
CREATE TABLE ck_drv (a INT NOT NULL, b INT NOT NULL,
                     PRIMARY KEY (a, b)) ENGINE=InnoDB;
INSERT INTO ck_drv VALUES (1, 2), (2, 1), (3, 5), (4, 4);
""")
    probe = ("SELECT SUM(ck.n) + COUNT(*) FROM ck_drv d "
             "JOIN ck ON ck.a = d.a AND ck.b = d.b")
    h.assert_not_pushed("mixed-engine join probes the handler", probe)
    h.assert_scalar("mixed-engine eq_ref on both PK parts", probe, "104")

    h.section("ALTER TABLE adds a composite UNIQUE")
    h.assert_ok("ADD UNIQUE (a, n)",
                "ALTER TABLE ck ADD UNIQUE KEY uq_an (a, n)")
    _assert_errno(h, "the added UNIQUE is enforced",
                  "INSERT INTO ck VALUES (1, 9, 50, 'w', 101)", 1062,
                  key="uq_an")
    _assert_errno(h, "the PRIMARY KEY is still enforced after ALTER",
                  "INSERT INTO ck VALUES (2, 1, 51, 'v', 9)", 1062,
                  key="PRIMARY")
    _assert_errno(h, "the first UNIQUE is still enforced after ALTER",
                  "INSERT INTO ck VALUES (6, 6, 10, 'y', 9)", 1062,
                  key="uq_cd")

    h.section("A composite secondary KEY is one stoolap index on all parts")
    h.exec_script(f"""
CREATE TABLE ckx (id INT NOT NULL PRIMARY KEY, x INT NOT NULL,
                  y INT NOT NULL, KEY ix_xy (x, y)) ENGINE={h.engine};
INSERT INTO ckx VALUES (1, 1, 1), (2, 1, 2), (3, 2, 1), (4, 2, 2);
""")
    h.assert_scalar("composite KEY lookup",
                    "SELECT id FROM ckx WHERE x = 2 AND y = 1", "3")
    if h.engine == "STOOLAP" and h.errlog and os.path.exists(h.errlog):
        with open(h.errlog, "r", errors="replace") as f:
            start = len(f.read())
        h.run_client(f"""
USE {h.db};
SET stoolap_explain_pushdown = 1;
SELECT COUNT(*) FROM ckx WHERE x = 2 AND y = 1;
""", use_db=False)
        with open(h.errlog, "r", errors="replace") as f:
            plan = f.read()[start:]
        if re.search(r"CompositeIndex \S+ \(x =, y =\)", plan):
            h._pass("stoolap probes the composite index on both parts")
        else:
            h._fail("no two-part CompositeIndex probe in the pushed plan",
                    f"plan: {plan.strip()[-400:]}")
