"""Persistent storage across a restart and a crash.

The case runs its own server (runner.ServerCtx.sibling) on a file://
DSN in a fresh directory, whatever DSN the suite runs on, and:
- writes tables with keys, a secondary index, a FOREIGN KEY and an
  AUTO_INCREMENT column, then stops the server cleanly and starts it;
- writes more, leaves a transaction open, kills the server (SIGKILL)
  and starts it again.
After each start, what was committed must be there, what was not must
be gone, and the tables must take reads, writes and DDL as before.
"""

import shutil

import mysql.connector

STOOLAP_DIR = "/tmp/stoolap-restart-stoolap"
DB = "restart"


def _connect(srv, db=DB):
    return mysql.connector.connect(user="root", unix_socket=srv.sock,
                                   database=db, autocommit=True)


def _rows(cur, sql):
    cur.execute(sql)
    return cur.fetchall()


def _check(h, srv, label, t_rows, c_rows):
    """The committed rows, through the primary key, the secondary index
    and a full scan; the FOREIGN KEY still enforced."""
    c = _connect(srv)
    cur = c.cursor()
    h.assert_eq(f"{label}: rows", t_rows,
                _rows(cur, "SELECT id, v, n FROM t ORDER BY id"))
    h.assert_eq(f"{label}: by the secondary index",
                [r for r in t_rows if r[1] == "b"],
                _rows(cur, "SELECT id, v, n FROM t FORCE INDEX (tv) "
                           "WHERE v = 'b' ORDER BY id"))
    h.assert_eq(f"{label}: by the primary key", [r for r in t_rows
                                                 if r[0] == t_rows[-1][0]],
                _rows(cur, f"SELECT id, v, n FROM t WHERE id = "
                           f"{t_rows[-1][0]}"))
    h.assert_eq(f"{label}: child rows", c_rows,
                _rows(cur, "SELECT id, pid FROM c ORDER BY id"))
    try:
        cur.execute("INSERT INTO c VALUES (999, 999)")
        h._fail(f"{label}: the FOREIGN KEY holds", "no error")
    except mysql.connector.Error as e:
        h.assert_eq(f"{label}: the FOREIGN KEY holds", 1452, e.errno)
    c.close()


def run(h):
    srv = h.server.sibling("restart", f"file://{STOOLAP_DIR}")
    shutil.rmtree(srv.data, ignore_errors=True)
    shutil.rmtree(STOOLAP_DIR, ignore_errors=True)
    srv.start()
    try:
        c = _connect(srv, None)
        cur = c.cursor()
        for stmt in [
            f"CREATE DATABASE {DB}", f"USE {DB}",
            "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, "
            "v VARCHAR(16) COLLATE utf8mb4_bin, n INT, KEY tv (v)) "
            "ENGINE=STOOLAP",
            "CREATE TABLE p (id INT PRIMARY KEY) ENGINE=STOOLAP",
            "CREATE TABLE c (id INT PRIMARY KEY, pid INT, "
            "FOREIGN KEY (pid) REFERENCES p(id)) ENGINE=STOOLAP",
            "INSERT INTO t (v, n) VALUES ('a', 1), ('b', 2), ('b', 3)",
            "INSERT INTO p VALUES (1), (2)",
            "INSERT INTO c VALUES (10, 1), (20, 2)",
            "UPDATE t SET n = 20 WHERE id = 2",
            "DELETE FROM t WHERE id = 1",
        ]:
            cur.execute(stmt)
        c.close()
        t_rows = [(2, "b", 20), (3, "b", 3)]
        c_rows = [(10, 1), (20, 2)]

        h.section("A clean restart keeps every committed row")
        srv.stop()
        srv.start()
        _check(h, srv, "after a restart", t_rows, c_rows)

        h.section("A crash keeps what was committed and drops what was not")
        c = _connect(srv)
        cur = c.cursor()
        cur.execute("INSERT INTO t (v, n) VALUES ('b', 4)")
        new_id = cur.lastrowid
        for stmt in [
            "UPDATE t SET v = 'c' WHERE id = 2",
            "DELETE FROM c WHERE id = 20",
            "BEGIN",
            "INSERT INTO t (v, n) VALUES ('b', 99)",
            "UPDATE t SET n = 99 WHERE id = 3",
            "DELETE FROM c WHERE id = 10",
        ]:
            cur.execute(stmt)
        srv.stop(kill=True)
        c.close()
        srv.start()
        t_rows = [(2, "c", 20), (3, "b", 3), (new_id, "b", 4)]
        c_rows = [(10, 1)]
        _check(h, srv, "after a crash", t_rows, c_rows)

        h.section("The tables take writes and DDL after recovery")
        c = _connect(srv)
        cur = c.cursor()
        cur.execute("INSERT INTO t (v, n) VALUES ('d', 5)")
        h.assert_eq("a new AUTO_INCREMENT id is past the committed ones",
                    True, cur.lastrowid > new_id)
        cur.execute("ALTER TABLE t ADD COLUMN w INT DEFAULT 7")
        cur.execute("UPDATE t SET w = n WHERE v = 'b'")
        h.assert_eq("after ALTER", [(2, 7), (3, 3), (new_id, 4)],
                    _rows(cur, f"SELECT id, w FROM t WHERE id <= {new_id} "
                               "ORDER BY id"))
        c.close()
        srv.stop()
        srv.start()
        c = _connect(srv)
        cur = c.cursor()
        h.assert_eq("ALTER and its writes survive a restart",
                    [(2, 7), (3, 3), (new_id, 4)],
                    _rows(cur, f"SELECT id, w FROM t WHERE id <= {new_id} "
                               "ORDER BY id"))
        cur.execute(f"DROP DATABASE {DB}")
        c.close()
    finally:
        srv.stop()
        shutil.rmtree(srv.data, ignore_errors=True)
        shutil.rmtree(STOOLAP_DIR, ignore_errors=True)
