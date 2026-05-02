"""Cross-session transactional correctness:
  - cached COUNT(*) must not leak transactional rows into the global cache;
  - pushed SELECT inside START TRANSACTION WITH CONSISTENT SNAPSHOT must
    actually see the snapshot, not the live row state.
"""

import time


def _records_live_counts(h):
    value = h.status("Stoolap_records_live_counts")
    return int(value or 0)


def run(h):
    h.section("COUNT(*) cache: in-flight tx must not poison other sessions")
    h.exec_script(f"""
CREATE TABLE cnt_leak (
    id INT NOT NULL PRIMARY KEY,
    v  INT NOT NULL
) ENGINE={h.engine};
INSERT INTO cnt_leak VALUES (1, 1);
""")

    # Warm both sessions' COUNT(*). Session A then opens a tx, inserts a
    # second row (uncommitted), and reads COUNT(*) inside the tx.
    h.assert_scalar("baseline COUNT before tx", "SELECT COUNT(*) FROM cnt_leak", "1")

    conn_a, cur_a = h.open_session(autocommit=False)
    try:
        cur_a.execute("START TRANSACTION")
        cur_a.execute("INSERT INTO cnt_leak VALUES (2, 2)")
        cur_a.execute("SELECT COUNT(*) FROM cnt_leak")
        a_count = cur_a.fetchall()[0][0]
        h.assert_eq("session A sees 2 inside tx", "2", a_count)

        # Session B (autocommit) must still see 1 -- A's row is uncommitted.
        b_count = h.sql("SELECT COUNT(*) FROM cnt_leak")
        h.assert_eq("session B blind to A's uncommitted insert", "1", b_count)

        cur_a.execute("ROLLBACK")
    finally:
        cur_a.close()
        conn_a.close()

    # After rollback, both sessions must agree on 1.
    h.assert_scalar("post-rollback COUNT", "SELECT COUNT(*) FROM cnt_leak", "1")

    h.section("COUNT(*) cache: tx-local stats survive own write")
    h.exec_script(f"""
CREATE TABLE cnt_tx_local (
    id INT NOT NULL PRIMARY KEY,
    v  INT NOT NULL
) ENGINE={h.engine};
INSERT INTO cnt_tx_local VALUES (1, 1);
""")

    conn_l, cur_l = h.open_session(autocommit=False)
    try:
        before = _records_live_counts(h)
        cur_l.execute("START TRANSACTION")
        cur_l.execute("EXPLAIN SELECT * FROM cnt_tx_local WHERE id >= 1")
        cur_l.fetchall()
        after_first = _records_live_counts(h)

        cur_l.execute("INSERT INTO cnt_tx_local VALUES (2, 2)")
        cur_l.execute("SELECT COUNT(*) FROM cnt_tx_local")
        h.assert_eq("tx-local count includes own insert", "2",
                    str(cur_l.fetchall()[0][0]))
        cur_l.execute("EXPLAIN SELECT * FROM cnt_tx_local WHERE id >= 1")
        cur_l.fetchall()
        after_second = _records_live_counts(h)

        h.assert_eq("first tx stats count used live path",
                    str(before + 1), str(after_first))
        h.assert_eq("post-write tx stats reused local cache",
                    str(after_first), str(after_second))
        cur_l.execute("ROLLBACK")
    finally:
        cur_l.close()
        conn_l.close()

    h.section("COUNT(*) cache: commit invalidates old committed counts")
    h.exec_script(f"""
CREATE TABLE cnt_commit (
    id INT NOT NULL PRIMARY KEY,
    v  INT NOT NULL
) ENGINE={h.engine};
INSERT INTO cnt_commit VALUES (1, 1);
""")
    h.assert_scalar("baseline commit COUNT", "SELECT COUNT(*) FROM cnt_commit", "1")

    conn_c, cur_c = h.open_session(autocommit=False)
    try:
        cur_c.execute("START TRANSACTION")
        cur_c.execute("INSERT INTO cnt_commit VALUES (2, 2)")
        cur_c.execute("SELECT COUNT(*) FROM cnt_commit")
        h.assert_eq("writer sees uncommitted commit row", "2",
                    str(cur_c.fetchall()[0][0]))

        # This autocommit read is allowed to cache the old committed value.
        # COMMIT must invalidate it, otherwise future COUNT(*) stays at 1
        # while row reads show both rows.
        h.assert_scalar("reader sees old count before commit",
                        "SELECT COUNT(*) FROM cnt_commit", "1")

        cur_c.execute("COMMIT")
    finally:
        cur_c.close()
        conn_c.close()

    h.assert_scalar("post-commit COUNT refreshed",
                    "SELECT COUNT(*) FROM cnt_commit", "2")
    h.assert_scalar("post-commit rows agree",
                    "SELECT COUNT(*) FROM (SELECT id FROM cnt_commit) z", "2")

    h.section("Pushed SELECT inside tx must see the session's own writes")
    # Without the pushdown factory opening the engine tx (or piggybacking
    # on one already opened via external_lock), the eager query would run
    # on the autocommit handle and miss the session's uncommitted INSERT.
    h.exec_script(f"""
CREATE TABLE tx_self (
    id INT NOT NULL PRIMARY KEY,
    v  INT NOT NULL
) ENGINE={h.engine};
INSERT INTO tx_self VALUES (1, 1);
""")

    conn_t, cur_t = h.open_session(autocommit=False)
    try:
        cur_t.execute("START TRANSACTION")
        cur_t.execute("INSERT INTO tx_self VALUES (2, 2)")

        # EXPLAIN confirms a pushdown path is involved. MariaDB prefers
        # normal handler range plans for these single-table shapes inside
        # an explicit tx, so use a pushed derived table.
        cur_t.execute(
            "EXPLAIN SELECT SUM(v) FROM "
            "(SELECT v FROM tx_self WHERE id >= 2) d")
        explain = " ".join(str(c) for r in cur_t.fetchall() for c in r)
        assert "PUSHED DERIVED" in explain, \
            f"derived aggregate didn't push: {explain}"

        cur_t.execute(
            "SELECT SUM(v) FROM (SELECT v FROM tx_self WHERE id >= 2) d")
        rows = cur_t.fetchall()
        h.assert_eq("pushed SELECT sees own uncommitted INSERT",
                    "1", str(len(rows)))
        if rows:
            h.assert_eq("pushed SELECT returns inserted value",
                        "2", str(rows[0][0]))

        cur_t.execute("ROLLBACK")
    finally:
        cur_t.close()
        conn_t.close()

    # Post-rollback: the row is gone, fresh sessions agree.
    h.assert_scalar("post-rollback row count",
                    "SELECT COUNT(*) FROM tx_self", "1")

    h.section("WITH CONSISTENT SNAPSHOT: pushed reads stay stable")
    h.exec_script(f"""
CREATE TABLE snap_stable (
    id INT NOT NULL PRIMARY KEY,
    v  INT NOT NULL
) ENGINE={h.engine};
INSERT INTO snap_stable VALUES (1, 10);
""")

    conn_r, cur_r = h.open_session(autocommit=False)
    try:
        cur_r.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT")
        cur_r.execute(
            "EXPLAIN SELECT SUM(v) FROM "
            "(SELECT v FROM snap_stable WHERE id >= 1) d")
        explain = " ".join(str(c) for r in cur_r.fetchall() for c in r)
        assert "PUSHED DERIVED" in explain, \
            f"snapshot derived aggregate didn't push: {explain}"

        cur_r.execute(
            "SELECT SUM(v) FROM (SELECT v FROM snap_stable WHERE id >= 1) d")
        first = str(cur_r.fetchall()[0][0])
        h.assert_eq("snapshot first read", "10", first)

        h.run_client("UPDATE snap_stable SET v = 20 WHERE id = 1")

        cur_r.execute(
            "SELECT SUM(v) FROM (SELECT v FROM snap_stable WHERE id >= 1) d")
        second = str(cur_r.fetchall()[0][0])
        h.assert_eq("snapshot second read stayed stable", first, second)
        cur_r.execute("COMMIT")
    finally:
        cur_r.close()
        conn_r.close()

    h.section("WITH CONSISTENT SNAPSHOT: tx must see its own writes too")
    # The handlerton callback opens a snapshot Stoolap tx at BEGIN time.
    # Subsequent writes / reads in the same session must still route
    # through that tx: visible to self, invisible to others until COMMIT.
    h.exec_script(f"""
CREATE TABLE snap_self (
    id INT NOT NULL PRIMARY KEY,
    v  INT NOT NULL
) ENGINE={h.engine};
INSERT INTO snap_self VALUES (1, 1);
""")

    conn_s, cur_s = h.open_session(autocommit=False)
    try:
        cur_s.execute("START TRANSACTION WITH CONSISTENT SNAPSHOT")
        cur_s.execute("INSERT INTO snap_self VALUES (2, 2)")
        cur_s.execute("SELECT COUNT(*) FROM snap_self")
        in_tx_cnt = cur_s.fetchall()[0][0]
        h.assert_eq("snapshot tx sees own write", "2", str(in_tx_cnt))

        # Other session must NOT see the in-flight insert.
        h.assert_scalar("other session blind to snapshot tx insert",
                        "SELECT COUNT(*) FROM snap_self", "1")

        cur_s.execute("ROLLBACK")
    finally:
        cur_s.close()
        conn_s.close()

    h.section("Plain REPEATABLE READ tx must hold a snapshot")
    # Default @@tx_isolation is REPEATABLE-READ. register_trx now picks
    # STOOLAP_ISOLATION_SNAPSHOT for that level, so reads inside the tx
    # don't shift after another session commits.
    h.exec_script(f"""
CREATE TABLE rr_inv (
    id INT NOT NULL PRIMARY KEY,
    qty INT NOT NULL
) ENGINE={h.engine};
INSERT INTO rr_inv VALUES (1, 10);
""")
    conn_rr, cur_rr = h.open_session(autocommit=False)
    try:
        cur_rr.execute("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")
        cur_rr.execute("START TRANSACTION")
        cur_rr.execute("SELECT qty FROM rr_inv WHERE id = 1")
        first = cur_rr.fetchall()[0][0]
        h.run_client("UPDATE rr_inv SET qty = qty + 1000 WHERE id = 1")
        cur_rr.execute("SELECT qty FROM rr_inv WHERE id = 1")
        second = cur_rr.fetchall()[0][0]
        h.assert_eq("REPEATABLE READ second read stable",
                    str(first), str(second))
        cur_rr.execute("COMMIT")
    finally:
        cur_rr.close()
        conn_rr.close()

    h.section("READ COMMITTED tx still sees post-BEGIN commits")
    # The flip side of the previous case: when the user opts down to
    # READ COMMITTED, register_trx must use stoolap's read-committed
    # isolation, so the second read DOES pick up the other session's
    # committed write.
    h.exec_script(f"""
CREATE TABLE rc_inv (
    id INT NOT NULL PRIMARY KEY,
    qty INT NOT NULL
) ENGINE={h.engine};
INSERT INTO rc_inv VALUES (1, 10);
""")
    conn_rc, cur_rc = h.open_session(autocommit=False)
    try:
        cur_rc.execute("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")
        cur_rc.execute("START TRANSACTION")
        cur_rc.execute("SELECT qty FROM rc_inv WHERE id = 1")
        first = cur_rc.fetchall()[0][0]
        h.run_client("UPDATE rc_inv SET qty = qty + 1000 WHERE id = 1")
        cur_rc.execute("SELECT qty FROM rc_inv WHERE id = 1")
        second = cur_rc.fetchall()[0][0]
        h.assert_eq("RC sees other session's commit",
                    "1010", str(second))
        h.assert_eq("RC first read still 10", "10", str(first))
        cur_rc.execute("COMMIT")
    finally:
        cur_rc.close()
        conn_rc.close()
