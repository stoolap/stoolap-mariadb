"""Transaction semantics: explicit BEGIN/COMMIT/ROLLBACK, autocommit,
rollback on error, write-conflict detection between concurrent sessions."""

import time


def run(h):
    h.exec_script(f"""
CREATE TABLE acct (
    id INT NOT NULL PRIMARY KEY,
    bal INT NOT NULL
) ENGINE={h.engine};
INSERT INTO acct VALUES (1, 100), (2, 200);
""")

    h.section("Explicit COMMIT")
    h.assert_ok("begin",            "START TRANSACTION")
    h.assert_ok("transfer step 1",  "UPDATE acct SET bal = bal - 30 WHERE id = 1")
    h.assert_ok("transfer step 2",  "UPDATE acct SET bal = bal + 30 WHERE id = 2")
    h.assert_ok("commit",           "COMMIT")
    h.assert_scalar("post-commit id 1", "SELECT bal FROM acct WHERE id = 1", "70")
    h.assert_scalar("post-commit id 2", "SELECT bal FROM acct WHERE id = 2", "230")

    h.section("Explicit ROLLBACK")
    # Multi-statement run shares one connection so ROLLBACK applies to the
    # earlier UPDATE.
    rc, out = h.run_client("""
START TRANSACTION;
UPDATE acct SET bal = 0;
ROLLBACK;
SELECT SUM(bal) FROM acct;
""")
    last = out.strip().splitlines()[-1] if out.strip() else ""
    h.assert_eq("rollback restored sum", "300", last)

    h.section("Autocommit")
    h.assert_scalar("autocommit on", "SELECT @@autocommit", "1")
    h.assert_ok("auto-DML", "UPDATE acct SET bal = bal + 1 WHERE id = 1")
    h.assert_scalar("visible elsewhere", "SELECT bal FROM acct WHERE id = 1", "71")

    h.section("Concurrent write conflict")
    # Session A holds a long-running transaction; session B races on the
    # same row. We accept either deadlock-class error OR clean serialization.
    a = h.run_async("""
START TRANSACTION;
SELECT SLEEP(0.05);
UPDATE acct SET bal = bal + 5 WHERE id = 1;
SELECT SLEEP(0.20);
COMMIT;
""")
    time.sleep(0.1)  # head start so A claims the row
    rc_b, out_b = h.run_client("""
START TRANSACTION;
UPDATE acct SET bal = bal + 7 WHERE id = 1;
COMMIT;
""")
    out_a = a.wait()  # noqa: F841 (kept for diagnostics)
    import re as _re
    if _re.search(r"deadlock|Got error 168|Got error|conflict|ERROR",
                  out_b, _re.IGNORECASE):
        h._pass("conflict surfaced as deadlock-class error")
    else:
        final = h.sql("SELECT bal FROM acct WHERE id = 1")
        if final in ("76", "78", "83"):
            h._pass(f"conflict serialized cleanly to bal={final}")
        else:
            h._fail("conflict produced unexpected state",
                    f"A: {out_a!r}", f"B: {out_b!r}", f"bal: {final}")

    h.section("Rollback on error mid-transaction")
    rc, out = h.run_client("""
START TRANSACTION;
INSERT INTO acct VALUES (3, 999);
INSERT INTO acct VALUES (3, 999);
ROLLBACK;
SELECT COUNT(*) FROM acct WHERE id = 3;
""", force=True)
    last = out.strip().splitlines()[-1] if out.strip() else ""
    h.assert_eq("post-error rollback", "0", last)

    h.section("Row-pump UPDATE keeps per-row duplicate handling")
    # Stoolap checks UPDATE's UNIQUE constraints at commit inside a tx.
    # Autocommit row-pump UPDATE must therefore not run in a statement
    # tx: the duplicate has to reach MariaDB from update_row, for IGNORE,
    # ODKU and AFTER triggers on other engines to behave as on InnoDB.
    h.exec_script(f"""
CREATE TABLE st_u (id INT NOT NULL PRIMARY KEY, u INT NOT NULL,
                   UNIQUE KEY st_uu (u)) ENGINE={h.engine};
INSERT INTO st_u VALUES (1, 1), (2, 3), (3, 6);
CREATE TABLE st_ig (id INT NOT NULL PRIMARY KEY, u INT NOT NULL,
                    UNIQUE KEY st_igu (u)) ENGINE={h.engine};
INSERT INTO st_ig VALUES (1, 10), (2, 20), (3, 30);
CREATE TABLE st_sw (id INT NOT NULL PRIMARY KEY, u INT NOT NULL,
                    UNIQUE KEY st_swu (u)) ENGINE={h.engine};
INSERT INTO st_sw VALUES (1, 10), (2, 20);
CREATE TABLE st_tr (id INT NOT NULL PRIMARY KEY, u INT NOT NULL,
                    UNIQUE KEY st_tru (u)) ENGINE={h.engine};
INSERT INTO st_tr VALUES (1, 10), (2, 20);
CREATE TABLE st_audit (id INT NOT NULL) ENGINE=InnoDB;
CREATE TRIGGER st_tr_au AFTER UPDATE ON st_tr FOR EACH ROW
    INSERT INTO st_audit VALUES (NEW.id);
""")
    h.assert_ok("UPDATE IGNORE skips only the conflicting row",
                "UPDATE IGNORE st_ig SET u = IF(id = 1, 11, 30) "
                "WHERE id <= 2 LIMIT 2")
    h.assert_scalar("UPDATE IGNORE kept the valid row",
                    "SELECT GROUP_CONCAT(u ORDER BY id) FROM st_ig",
                    "11,20,30")
    h.assert_err("ODKU swapping two UNIQUE values is refused",
                 "INSERT INTO st_sw VALUES (1, 20), (2, 10) "
                 "ON DUPLICATE KEY UPDATE u = VALUES(u)", "1062")
    h.assert_err("failing UPDATE with an AFTER trigger on InnoDB",
                 "UPDATE st_tr SET u = 20 WHERE id = 1 LIMIT 1", "1062")
    h.assert_scalar("the trigger's InnoDB row did not survive",
                    "SELECT COUNT(*) FROM st_audit", "0")

    h.section("UPDATE duplicate found at COMMIT reports 1062")
    # Stoolap checks UPDATE's UNIQUE constraints at commit inside an
    # explicit transaction; the commit error must keep its class.
    rc, out = h.run_client("""
START TRANSACTION;
UPDATE st_u SET u = 3 WHERE id = 1;
COMMIT;
""", force=True)
    h.assert_eq("COMMIT duplicate errno", True, "ERROR 1062" in out)
    h.assert_scalar("COMMIT duplicate rolled back",
                    "SELECT GROUP_CONCAT(u ORDER BY id) FROM st_u", "1,3,6")
