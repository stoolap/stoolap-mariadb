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

    h.section("Autocommit row-pump UPDATE/DELETE is one statement")
    # LIMIT keeps these off direct DML (update_row / delete_row per row).
    # A failure on the last row must undo the rows before it, as InnoDB
    # does; each row used to commit on its own.
    h.exec_script(f"""
CREATE TABLE st_u (id INT NOT NULL PRIMARY KEY, u INT NOT NULL,
                   UNIQUE KEY st_uu (u)) ENGINE={h.engine};
INSERT INTO st_u VALUES (1, 1), (2, 3), (3, 6);
CREATE TABLE st_p (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
CREATE TABLE st_c (id INT NOT NULL PRIMARY KEY, pid INT NOT NULL,
                   FOREIGN KEY (pid) REFERENCES st_p(id)) ENGINE={h.engine};
INSERT INTO st_p VALUES (1), (2), (3);
INSERT INTO st_c VALUES (10, 3);
""")
    # Rows 1 and 2 are fine; row 3 lands on row 1's new value.
    h.assert_err("row-pump UPDATE duplicate on the last row",
                 "UPDATE st_u SET u = IF(id = 3, 11, u + 10) "
                 "ORDER BY id LIMIT 3", "1062")
    h.assert_scalar("UPDATE left every row unchanged",
                    "SELECT GROUP_CONCAT(u ORDER BY id) FROM st_u", "1,3,6")
    h.assert_err("row-pump DELETE hits a referenced parent last",
                 "DELETE FROM st_p ORDER BY id LIMIT 3", "1451")
    h.assert_scalar("DELETE left every parent in place",
                    "SELECT COUNT(*) FROM st_p", "3")
    h.assert_ok("row-pump DELETE without a conflict",
                "DELETE FROM st_p ORDER BY id LIMIT 2")
    h.assert_scalar("conflict-free row-pump DELETE committed",
                    "SELECT GROUP_CONCAT(id ORDER BY id) FROM st_p", "3")

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
