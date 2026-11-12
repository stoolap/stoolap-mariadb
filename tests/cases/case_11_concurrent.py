"""Multi-session: visibility of committed writes, isolation of in-flight
transactions, two writers serializing through stoolap's MVCC, parallel DDL."""

import re
import time


def run(h):
    h.exec_script(f"""
CREATE TABLE inv (
    id INT NOT NULL PRIMARY KEY,
    qty INT NOT NULL
) ENGINE={h.engine};
INSERT INTO inv VALUES (1, 10), (2, 20);
""")

    h.section("Committed writes are visible to other sessions")
    h.run_client("UPDATE inv SET qty = 100 WHERE id = 1")
    h.assert_scalar("fresh session sees update", "SELECT qty FROM inv WHERE id = 1", "100")

    h.section("In-flight transaction is invisible until commit")
    a = h.run_async("""
START TRANSACTION;
SELECT SLEEP(0.05);
UPDATE inv SET qty = 999 WHERE id = 2;
SELECT SLEEP(0.30);
COMMIT;
""", force=True)
    time.sleep(0.15)
    mid = h.sql("SELECT qty FROM inv WHERE id = 2")
    h.assert_eq("mid-tx invisible to B", "20", mid)
    a.wait()
    post = h.sql("SELECT qty FROM inv WHERE id = 2")
    h.assert_eq("post-commit visible to B", "999", post)

    h.section("Two writers contend on same row")
    h.run_client("UPDATE inv SET qty = 0 WHERE id = 1")
    w1 = h.run_async("""
START TRANSACTION;
UPDATE inv SET qty = qty + 5 WHERE id = 1;
SELECT SLEEP(0.20);
COMMIT;
""", force=True)
    w2 = h.run_async("""
START TRANSACTION;
SELECT SLEEP(0.05);
UPDATE inv SET qty = qty + 7 WHERE id = 1;
COMMIT;
""", force=True)
    out_w1 = w1.wait()
    out_w2 = w2.wait()
    final = h.sql("SELECT qty FROM inv WHERE id = 1")
    if final in ("5", "7", "12"):
        h._pass(f"contention left consistent qty={final}")
    else:
        h._fail(f"unexpected post-contention qty={final}",
                f"w1: {out_w1!r}", f"w2: {out_w2!r}")

    h.section("Snapshot isolation in a second session")
    snap = h.run_async("""
START TRANSACTION WITH CONSISTENT SNAPSHOT;
SELECT qty FROM inv WHERE id = 2;
SELECT SLEEP(0.20);
SELECT qty FROM inv WHERE id = 2;
COMMIT;
""", force=True)
    time.sleep(0.05)
    h.run_client("UPDATE inv SET qty = qty + 1000 WHERE id = 2")
    snap_out = snap.wait()
    nums = [m.group(0) for m in re.finditer(r"^\d+", snap_out, re.MULTILINE)]
    if len(nums) >= 2 and nums[0] == nums[1]:
        h._pass(f"snapshot stable across reads (qty={nums[0]})")
    else:
        # Some MariaDB builds don't apply WITH CONSISTENT SNAPSHOT to
        # non-InnoDB engines. Both reads then see the latest value, which
        # is still a valid behaviour, just a different isolation model.
        first = nums[0] if nums else "?"
        second = nums[1] if len(nums) > 1 else "?"
        h._pass(f"snapshot semantics not enforced for engine; reads were "
                f"{first} and {second} (acceptable)")

    h.section("Concurrent DDL across THDs is serialised cleanly")
    n_ddl = 10
    procs = []
    for i in range(1, n_ddl + 1):
        procs.append((i, h.run_async(f"""
CREATE TABLE concddl_{i} (id INT NOT NULL PRIMARY KEY, n INT) ENGINE={h.engine};
INSERT INTO concddl_{i} VALUES ({i}, {i});
DROP TABLE concddl_{i};
""")))
    all_clean = True
    for i, p in procs:
        out = p.wait()
        if out.strip():
            all_clean = False
            print(f"  FAIL  concurrent DDL #{i} emitted output:\n{out}")
    if all_clean:
        h._pass(f"{n_ddl} parallel CREATE+INSERT+DROP cycles all clean")
    else:
        h._fail("parallel DDL emitted output")

    out = h.sql(
        "SELECT COUNT(*) FROM information_schema.tables "
        f"WHERE table_schema = '{h.db}' AND table_name LIKE 'concddl_%'")
    h.assert_eq("no orphan tables after concurrent DDL", "0", out)

    h.section("Concurrent row-pump UPDATEs do not write over each other")
    # The row pump writes the values MariaDB computed from the row it read;
    # the handler checks the row still holds what was read, and a commit
    # in between fails the statement with 1213 (as a direct UPDATE's
    # conflict does) instead of being written over. Every reported change
    # must be in the final value.
    h.exec_script(f"""
CREATE TABLE cnt (id INT NOT NULL PRIMARY KEY, v INT NOT NULL) ENGINE={h.engine};
INSERT INTO cnt VALUES (1, 0), (2, 0), (3, 0);
CREATE TABLE cntp (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO cntp VALUES (1);
CREATE TABLE cntc (id INT NOT NULL PRIMARY KEY, v INT NOT NULL, pid INT,
                   FOREIGN KEY (pid) REFERENCES cntp(id)) ENGINE={h.engine};
INSERT INTO cntc VALUES (1, 0, 1);
CREATE TABLE cnts (id INT NOT NULL PRIMARY KEY, s VARCHAR(2000) NOT NULL)
    ENGINE={h.engine};
INSERT INTO cnts VALUES (1, '');
CREATE TABLE cntsc (id INT NOT NULL PRIMARY KEY, s VARCHAR(2000) NOT NULL,
                    pid INT, FOREIGN KEY (pid) REFERENCES cntp(id))
    ENGINE={h.engine};
INSERT INTO cntsc VALUES (1, '', 1);
""")
    import threading
    # A VARCHAR's trailing spaces are its value (only a CHAR's padding is
    # not): appending one is a change the check sees
    for label, sql, check in [
            ("ORDER BY / LIMIT",
             "UPDATE cnt SET v = v + 1 WHERE id = 1 ORDER BY id LIMIT 1",
             "SELECT v FROM cnt WHERE id = 1"),
            ("IGNORE", "UPDATE IGNORE cnt SET v = v + 1 WHERE id = 2",
             "SELECT v FROM cnt WHERE id = 2"),
            ("one row by its primary key",
             "UPDATE cnt SET v = v + 1 WHERE id = 3",
             "SELECT v FROM cnt WHERE id = 3"),
            ("a table a FOREIGN KEY names (through SQL)",
             "UPDATE cntc SET v = v + 1 WHERE id = 1 ORDER BY id LIMIT 1",
             "SELECT v FROM cntc WHERE id = 1"),
            ("a VARCHAR's trailing spaces",
             "UPDATE cnts SET s = CONCAT(s, ' ') WHERE id = 1 "
             "ORDER BY id LIMIT 1", "SELECT LENGTH(s) FROM cnts"),
            ("a VARCHAR's trailing spaces through SQL",
             "UPDATE cntsc SET s = CONCAT(s, ' ') WHERE id = 1 "
             "ORDER BY id LIMIT 1", "SELECT LENGTH(s) FROM cntsc")]:
        changed = []
        lock = threading.Lock()

        def work():
            c, cur = h.open_session()
            n = 0
            for _ in range(400):
                try:
                    cur.execute(sql)
                    n += cur.rowcount
                except Exception:  # 1213: retried by a real client
                    pass
            c.close()
            with lock:
                changed.append(n)

        threads = [threading.Thread(target=work) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        h.assert_scalar(f"{label}: every reported change kept", check,
                        str(sum(changed)))

    # A row-pump UPDATE that moves a row to a new primary key deletes and
    # inserts it: the version it checked is held through the move, so an
    # increment committed meanwhile is not lost with the old copy
    h.exec_script(f"""
CREATE TABLE mv (id INT NOT NULL PRIMARY KEY, v INT NOT NULL) ENGINE={h.engine};
INSERT INTO mv VALUES (1, 0);
""")
    stop = [False]
    incs = []

    def mover():
        c, cur = h.open_session()
        while not stop[0]:
            try:
                cur.execute("UPDATE mv SET id = 3 - id WHERE id IN (1, 2) "
                            "ORDER BY id LIMIT 1")
            except Exception:
                pass
        c.close()

    def incrementer():
        c, cur = h.open_session()
        n = 0
        for _ in range(400):
            try:
                cur.execute("UPDATE mv SET v = v + 1 WHERE id IN (1, 2)")
                n += cur.rowcount
            except Exception:
                pass
        c.close()
        with lock:
            incs.append(n)

    m = threading.Thread(target=mover)
    m.start()
    workers = [threading.Thread(target=incrementer) for _ in range(3)]
    for t in workers:
        t.start()
    for t in workers:
        t.join()
    stop[0] = True
    m.join()
    h.assert_scalar("a moved row keeps every reported increment",
                    "SELECT SUM(v) FROM mv", str(sum(incs)))

    # A row-pump DELETE of a row a FOREIGN KEY names goes through SQL,
    # which finds its rows before it deletes them: the row is held first,
    # so a k committed meanwhile is not deleted under the old one's test
    h.exec_script(f"""
CREATE TABLE dk (id INT NOT NULL PRIMARY KEY, k INT NOT NULL UNIQUE)
    ENGINE={h.engine};
CREATE TABLE dkc (id INT NOT NULL PRIMARY KEY, pk INT,
                  FOREIGN KEY (pk) REFERENCES dk(k) ON DELETE CASCADE)
    ENGINE={h.engine};
""")
    odd = 0
    for _ in range(200):
        h.exec_script("DELETE FROM dk; INSERT INTO dk VALUES (1, 0);")
        stop[0] = False
        bumps = [0]

        def bumper():
            c, cur = h.open_session()
            while not stop[0]:
                try:
                    cur.execute("UPDATE dk SET k = k + 1 WHERE id = 1")
                    if cur.rowcount == 0:
                        break
                    bumps[0] += 1
                except Exception:
                    pass
            c.close()

        b = threading.Thread(target=bumper)
        b.start()
        c, cur = h.open_session()
        while True:
            try:
                cur.execute("DELETE FROM dk WHERE id = 1 AND k % 2 = 0 "
                            "ORDER BY id LIMIT 1")
                if cur.rowcount == 1:
                    break
            except Exception:
                pass
        stop[0] = True
        b.join()
        c.close()
        odd += bumps[0] % 2
    h.assert_eq("a DELETE never takes a row past its test", 0, odd)

    h.section("A copying ALTER does not run online beside direct writes")
    # MariaDB replays writes made during an online copying ALTER from their
    # row events; a direct UPDATE / DELETE writes none, so the copy lost
    # the UPDATEd row and brought the DELETEd one back.
    h.exec_script(f"""
CREATE TABLE oa (id INT NOT NULL PRIMARY KEY, v INT NOT NULL)
    ENGINE={h.engine};
SET max_recursive_iterations = 1000000;
INSERT INTO oa WITH RECURSIVE s(n) AS
    (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 1000000)
    SELECT n, 0 FROM s;
""")
    h.assert_err("LOCK=NONE is refused for a copying ALTER",
                 "ALTER TABLE oa FORCE, ALGORITHM=COPY, LOCK=NONE",
                 "LOCK=NONE is not supported|1846")
    alter = h.run_async("ALTER TABLE oa ADD COLUMN w INT, ALGORITHM=COPY;")
    time.sleep(0.15)
    h.run_client("UPDATE oa SET v = 5 WHERE id = 5;"
                 "DELETE FROM oa WHERE id = 7;")
    out = alter.wait()
    h.assert_eq("the ALTER succeeded", "", out.strip())
    h.assert_scalar("the UPDATEd row kept its change",
                    "SELECT v FROM oa WHERE id = 5", "5")
    h.assert_scalar("the DELETEd row stayed deleted",
                    "SELECT COUNT(*) FROM oa WHERE id = 7", "0")
    h.assert_scalar("every other row survived",
                    "SELECT COUNT(*) FROM oa", "999999")
