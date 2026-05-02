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
