"""Savepoint plumbing: SAVEPOINT / RELEASE SAVEPOINT / ROLLBACK TO
routes through the new bucket-A FFI (stoolap_tx_savepoint /
stoolap_tx_release_savepoint / stoolap_tx_rollback_to_savepoint).

MariaDB does not pass the user's SAVEPOINT name to the engine; the
plugin synthesises "sp<id>" from a per-connection counter and stashes
id+name in the engine-private chunk MariaDB allocates per SAVEPOINT
(handlerton::savepoint_offset). All three callbacks share that chunk
so a release / rollback finds the same name set issued.
"""

import mysql.connector


def _scalar(cur, sql):
    cur.execute(sql)
    rows = cur.fetchall()
    return rows[0][0] if rows else None


def run(h):
    h.exec_script(f"""
CREATE TABLE sp_t (id INT NOT NULL PRIMARY KEY, n INT) ENGINE={h.engine};
""")

    h.section("ROLLBACK TO undoes a savepoint's changes only")
    # BEGIN; INSERT 1; SAVEPOINT s1; INSERT 2; ROLLBACK TO s1; INSERT 3; COMMIT
    # Final visible rows: {1, 3}. Row 2 was inside the s1..rollback window.
    conn, cur = h.open_session(autocommit=False)
    try:
        cur.execute("START TRANSACTION")
        cur.execute("INSERT INTO sp_t VALUES (1, 10)")
        cur.execute("SAVEPOINT s1")
        cur.execute("INSERT INTO sp_t VALUES (2, 20)")
        cur.execute("ROLLBACK TO SAVEPOINT s1")
        cur.execute("INSERT INTO sp_t VALUES (3, 30)")
        cur.execute("COMMIT")

        h.assert_scalar("post-rollback row 1 present",
                        "SELECT n FROM sp_t WHERE id = 1", "10")
        h.assert_scalar("post-rollback row 2 absent (rolled back)",
                        "SELECT COUNT(*) FROM sp_t WHERE id = 2", "0")
        h.assert_scalar("post-rollback row 3 present",
                        "SELECT n FROM sp_t WHERE id = 3", "30")
    finally:
        cur.close()
        conn.close()
    h.exec_stmt("DELETE FROM sp_t")

    h.section("RELEASE SAVEPOINT keeps changes; tx commit persists them")
    # BEGIN; INSERT 1; SAVEPOINT s1; INSERT 2; RELEASE s1; INSERT 3; COMMIT
    # Final visible rows: {1, 2, 3}.
    conn, cur = h.open_session(autocommit=False)
    try:
        cur.execute("START TRANSACTION")
        cur.execute("INSERT INTO sp_t VALUES (1, 10)")
        cur.execute("SAVEPOINT s1")
        cur.execute("INSERT INTO sp_t VALUES (2, 20)")
        cur.execute("RELEASE SAVEPOINT s1")
        cur.execute("INSERT INTO sp_t VALUES (3, 30)")
        cur.execute("COMMIT")

        h.assert_scalar("post-release count = 3",
                        "SELECT COUNT(*) FROM sp_t", "3")
        h.assert_scalar("post-release sum = 60",
                        "SELECT SUM(n) FROM sp_t", "60")
    finally:
        cur.close()
        conn.close()
    h.exec_stmt("DELETE FROM sp_t")

    h.section("Nested savepoints: ROLLBACK TO outer drops inner state")
    # BEGIN; INSERT 1; SAVEPOINT s1; INSERT 2; SAVEPOINT s2; INSERT 3;
    # ROLLBACK TO s1; COMMIT
    # Final: {1}. Both inner savepoint and rows 2,3 are gone.
    conn, cur = h.open_session(autocommit=False)
    try:
        cur.execute("START TRANSACTION")
        cur.execute("INSERT INTO sp_t VALUES (1, 10)")
        cur.execute("SAVEPOINT s1")
        cur.execute("INSERT INTO sp_t VALUES (2, 20)")
        cur.execute("SAVEPOINT s2")
        cur.execute("INSERT INTO sp_t VALUES (3, 30)")
        cur.execute("ROLLBACK TO SAVEPOINT s1")
        cur.execute("COMMIT")

        h.assert_scalar("nested rollback: only row 1 remains",
                        "SELECT GROUP_CONCAT(id ORDER BY id) FROM sp_t",
                        "1")
    finally:
        cur.close()
        conn.close()
    h.exec_stmt("DELETE FROM sp_t")

    h.section("ROLLBACK TO an unknown savepoint -> error")
    conn, cur = h.open_session(autocommit=False)
    try:
        cur.execute("START TRANSACTION")
        cur.execute("INSERT INTO sp_t VALUES (1, 10)")
        try:
            cur.execute("ROLLBACK TO SAVEPOINT does_not_exist")
            h._fail("rollback to unknown savepoint should error",
                    "server: <no error>")
        except mysql.connector.Error as e:
            # MariaDB raises ER_SP_DOES_NOT_EXIST (1305) when no
            # engine recognises the name. Either that or the engine-
            # propagated error (HA_ERR_GENERIC -> 1296 with the
            # stoolap text) is acceptable -- both signal "name not
            # found" to the client.
            if e.errno in (1305, 1296):
                h._pass(
                    f"rollback to unknown savepoint surfaced errno {e.errno}")
            else:
                h._fail("rollback to unknown savepoint wrong errno",
                        f"expected: 1305 or 1296",
                        f"actual:   errno {e.errno}: {e}")
        finally:
            try:
                cur.execute("ROLLBACK")
            except Exception:
                pass
    finally:
        cur.close()
        conn.close()
    h.exec_stmt("DELETE FROM sp_t")

    h.section("Reused savepoint name overrides earlier one")
    # SAVEPOINT s1; INSERT a; SAVEPOINT s1; INSERT b; ROLLBACK TO s1
    # Per SQL spec, re-declaring a savepoint name discards the prior.
    # ROLLBACK TO s1 should undo only `b`, leaving `a`.
    conn, cur = h.open_session(autocommit=False)
    try:
        cur.execute("START TRANSACTION")
        cur.execute("SAVEPOINT s1")
        cur.execute("INSERT INTO sp_t VALUES (1, 10)")
        cur.execute("SAVEPOINT s1")
        cur.execute("INSERT INTO sp_t VALUES (2, 20)")
        cur.execute("ROLLBACK TO SAVEPOINT s1")
        cur.execute("COMMIT")

        h.assert_scalar(
            "reused-name rollback: row 1 kept, row 2 dropped",
            "SELECT GROUP_CONCAT(id ORDER BY id) FROM sp_t", "1")
    finally:
        cur.close()
        conn.close()
    h.exec_stmt("DELETE FROM sp_t")

    h.section("Drift detector: Stoolap_unmapped_errors stayed at baseline")
    h.cur.execute("SHOW STATUS LIKE 'Stoolap_unmapped_errors'")
    row = h.cur.fetchall()
    val = int(row[0][1]) if row else 0
    if val == 0:
        h._pass(f"Stoolap_unmapped_errors still {val}")
    else:
        h._fail(f"Stoolap_unmapped_errors moved to {val}",
                "savepoint paths produced an error wording the typed",
                "and prose mappers both gave up on -- inspect log for",
                "[Note] stoolap typed-error gap lines")
