"""Error-mapping smoke test: every known stoolap error class must
round-trip through map_stoolap_error to the right MariaDB errno.

Doubles as a drift detector: SHOW STATUS LIKE 'Stoolap_unmapped_errors'
must stay at 0 across the whole run. A stoolap upgrade that rewords any
of these messages will either flip the errno to 1296 (caught by the
specific assert_errno) or bump the unmapped counter (caught by the
final assertion).

The pattern table is in src/ha_stoolap.cc::map_stoolap_error; the
canonical stoolap formats are in ../stoolap/src/core/error.rs.
"""

import mysql.connector
import time


def _assert_errno(h, label, sql, expected_errno):
    """Run sql and assert e.errno equals expected_errno exactly."""
    try:
        h.cur.execute(sql)
        try:
            h.cur.fetchall()
        except mysql.connector.errors.InterfaceError:
            pass
        h._fail(label, f"sql:    {sql}", "server: <no error>")
    except mysql.connector.Error as e:
        if e.errno == expected_errno:
            h._pass(label)
        else:
            h._fail(label,
                    f"sql:      {sql}",
                    f"expected: errno {expected_errno}",
                    f"actual:   errno {e.errno}: {e}")


def _unmapped(h):
    """Snapshot Stoolap_unmapped_errors counter."""
    h.cur.execute("SHOW STATUS LIKE 'Stoolap_unmapped_errors'")
    row = h.cur.fetchall()
    return int(row[0][1]) if row else 0


def run(h):
    baseline_unmapped = _unmapped(h)

    h.section("Setup tables for each error class")
    h.exec_script(f"""
CREATE TABLE pk_t  (id INT NOT NULL PRIMARY KEY, n INT) ENGINE={h.engine};
INSERT INTO pk_t VALUES (1, 1);

CREATE TABLE uq_t  (id INT NOT NULL PRIMARY KEY,
                    u INT NOT NULL,
                    UNIQUE KEY uq_u (u)) ENGINE={h.engine};
INSERT INTO uq_t VALUES (1, 10), (2, 20);

CREATE TABLE p_t   (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO p_t VALUES (1);
CREATE TABLE c_t   (id INT NOT NULL PRIMARY KEY,
                    pid INT NOT NULL,
                    FOREIGN KEY (pid) REFERENCES p_t(id)) ENGINE={h.engine};
INSERT INTO c_t VALUES (10, 1);

CREATE TABLE conf_t (id INT NOT NULL PRIMARY KEY, n INT) ENGINE={h.engine};
INSERT INTO conf_t VALUES (1, 1);
""")

    h.section("Constraint violations -> ER_DUP_ENTRY (1062)")
    _assert_errno(h, "PK violation",
                  "INSERT INTO pk_t VALUES (1, 99)", 1062)
    _assert_errno(h, "UNIQUE violation (single-row INSERT)",
                  "INSERT INTO uq_t VALUES (3, 10)", 1062)
    # Direct UPDATE path -- exercises try_direct_modify's error mapping.
    # Row 2 holds u=20; UPDATE it to u=10 collides with row 1.
    _assert_errno(h, "UNIQUE violation via direct UPDATE",
                  "UPDATE uq_t SET u = 10 WHERE id = 2", 1062)

    h.section("Foreign key violation -> ER_NO_REFERENCED_ROW_2 (1452)")
    _assert_errno(h, "FK violation on INSERT",
                  "INSERT INTO c_t VALUES (11, 999)", 1452)
    _assert_errno(h, "FK violation via direct UPDATE",
                  "UPDATE c_t SET pid = 999 WHERE id = 10", 1452)

    h.section("Table lifecycle errors")
    _assert_errno(h, "CREATE TABLE on existing name -> 1050",
                  f"CREATE TABLE pk_t (x INT) ENGINE={h.engine}", 1050)
    _assert_errno(h, "SELECT from missing table -> 1146",
                  "SELECT * FROM no_such_table_for_real_t", 1146)

    h.section("Concurrency / locking -> ER_LOCK_DEADLOCK (1213)")
    # Open A first and let it hold an in-flight UPDATE on row 1; A
    # never commits within the section so the row stays locked.
    # Then B (a separate connection in autocommit mode) tries to
    # UPDATE the same row and must see ER_LOCK_DEADLOCK (1213,
    # SQLSTATE 40001). Avoiding the run_async/sleep race makes the
    # test deterministic across runners (real Linux, macOS, Docker
    # emulation), where subprocess spawn timing was flipping which
    # session won the row.
    conn_a, cur_a = h.open_session(autocommit=False)
    cur_a.execute("START TRANSACTION")
    cur_a.execute("UPDATE conf_t SET n = 2 WHERE id = 1")
    try:
        b_errno = None
        b_msg = ""
        conn_b, cur_b = h.open_session(autocommit=True)
        try:
            cur_b.execute("UPDATE conf_t SET n = 3 WHERE id = 1")
        except mysql.connector.Error as e:
            b_errno = e.errno
            b_msg = str(e)
        finally:
            cur_b.close()
            conn_b.close()

        if b_errno == 1213:
            h._pass("concurrent UPDATE conflict surfaces as 1213")
        else:
            h._fail("concurrent UPDATE conflict surfaces as 1213",
                    f"expected: errno 1213",
                    f"actual:   errno {b_errno}: {b_msg}")
    finally:
        try:
            cur_a.execute("ROLLBACK")
        except Exception:
            pass
        cur_a.close()
        conn_a.close()

    h.section("Drift detector: Stoolap_unmapped_errors stayed at baseline")
    final_unmapped = _unmapped(h)
    delta = final_unmapped - baseline_unmapped
    if delta == 0:
        h._pass(f"Stoolap_unmapped_errors unchanged (still {final_unmapped})")
    else:
        h._fail(f"Stoolap_unmapped_errors bumped by {delta}",
                "stoolap reworded an error class that map_stoolap_error",
                "no longer recognises -- check stoolap/src/core/error.rs",
                "vs the pattern table in src/ha_stoolap.cc::map_stoolap_error.")
