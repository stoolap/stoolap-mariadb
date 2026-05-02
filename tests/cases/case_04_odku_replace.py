"""ON DUPLICATE KEY UPDATE / REPLACE INTO / AUTO_INCREMENT generation.

Includes regressions:
  - lazy db_ + bulk AI primer (CTE-driven 200-row insert)
  - cross-connection cache invalidation on TRUNCATE / DROP / RENAME
  - cross-connection allocator safety (explicit + auto, racing inserts)
  - auto_increment_offset / auto_increment_increment cursor advance
  - bulk INSERT partial-row rollback alignment
"""

import time


def run(h):
    h.exec_script(f"""
CREATE TABLE k (
    id INT NOT NULL PRIMARY KEY,
    code VARCHAR(20) COLLATE utf8mb4_bin NOT NULL UNIQUE,
    n INT NOT NULL DEFAULT 0
) ENGINE={h.engine};
INSERT INTO k VALUES (1, 'A', 10), (2, 'B', 20);
""")

    h.section("ON DUPLICATE KEY UPDATE")
    h.assert_ok("ODKU on PK match",
                "INSERT INTO k VALUES (1, 'A', 99) ON DUPLICATE KEY UPDATE n = VALUES(n)")
    h.assert_scalar("ODKU updated", "SELECT n FROM k WHERE id = 1", "99")
    h.assert_ok("ODKU on UNIQUE match",
                "INSERT INTO k VALUES (3, 'B', 77) ON DUPLICATE KEY UPDATE n = VALUES(n)")
    h.assert_scalar("UNIQUE row updated", "SELECT n FROM k WHERE code = 'B'", "77")
    h.assert_scalar("no new row added", "SELECT COUNT(*) FROM k", "2")
    h.assert_ok("ODKU as fresh INSERT",
                "INSERT INTO k VALUES (3, 'C', 33) ON DUPLICATE KEY UPDATE n = VALUES(n)")
    h.assert_scalar("fresh row inserted", "SELECT COUNT(*) FROM k", "3")
    h.assert_scalar("fresh row content",  "SELECT n FROM k WHERE id = 3", "33")

    h.section("REPLACE INTO")
    h.assert_ok("REPLACE on PK match", "REPLACE INTO k VALUES (1, 'A', 1)")
    h.assert_scalar("REPLACE updated", "SELECT n FROM k WHERE id = 1", "1")
    h.assert_ok("REPLACE as fresh",    "REPLACE INTO k VALUES (4, 'D', 44)")
    h.assert_scalar("REPLACE fresh row", "SELECT n FROM k WHERE id = 4", "44")

    h.section("Multi-row IGNORE / REPLACE / ODKU (bulk-batch dup recovery)")
    # start_bulk_insert used to enable bulk batching unconditionally,
    # so a multi-row INSERT IGNORE / REPLACE / ODKU that hit a dup
    # aborted the whole batch with ERROR 1030 and silently dropped
    # every non-conflicting row. The fix opts out of bulk batching
    # for these shapes so MariaDB drives per-row dup recovery.
    h.exec_script(f"""
CREATE TABLE bulk_dup (id INT NOT NULL PRIMARY KEY,
                      v INT NOT NULL) ENGINE={h.engine};
INSERT INTO bulk_dup VALUES (1, 1), (2, 2);
""")
    h.assert_ok("INSERT IGNORE multi-row with dups",
                "INSERT IGNORE INTO bulk_dup "
                "VALUES (1, 99), (3, 3), (2, 99), (4, 4)")
    h.assert_scalar("IGNORE: row 1 untouched",
                    "SELECT v FROM bulk_dup WHERE id = 1", "1")
    h.assert_scalar("IGNORE: row 2 untouched",
                    "SELECT v FROM bulk_dup WHERE id = 2", "2")
    h.assert_scalar("IGNORE: non-conflicting rows kept",
                    "SELECT GROUP_CONCAT(id ORDER BY id) FROM bulk_dup",
                    "1,2,3,4")

    h.exec_stmt("DELETE FROM bulk_dup")
    h.exec_stmt("INSERT INTO bulk_dup VALUES (1, 1), (2, 2)")
    h.assert_ok("REPLACE multi-row with dups",
                "REPLACE INTO bulk_dup "
                "VALUES (1, 99), (3, 3), (2, 99), (4, 4)")
    h.assert_scalar("REPLACE: row 1 replaced",
                    "SELECT v FROM bulk_dup WHERE id = 1", "99")
    h.assert_scalar("REPLACE: row 2 replaced",
                    "SELECT v FROM bulk_dup WHERE id = 2", "99")
    h.assert_scalar("REPLACE: rows 3 and 4 inserted",
                    "SELECT GROUP_CONCAT(id ORDER BY id) FROM bulk_dup",
                    "1,2,3,4")

    h.exec_stmt("DELETE FROM bulk_dup")
    h.exec_stmt("INSERT INTO bulk_dup VALUES (1, 1), (2, 2)")
    h.assert_ok("ODKU multi-row mixed dup + new",
                "INSERT INTO bulk_dup VALUES (1, 77), (5, 5) "
                "ON DUPLICATE KEY UPDATE v = VALUES(v)")
    h.assert_scalar("ODKU: row 1 updated",
                    "SELECT v FROM bulk_dup WHERE id = 1", "77")
    h.assert_scalar("ODKU: row 2 untouched",
                    "SELECT v FROM bulk_dup WHERE id = 2", "2")
    h.assert_scalar("ODKU: row 5 inserted",
                    "SELECT v FROM bulk_dup WHERE id = 5", "5")

    h.section("AUTO_INCREMENT bulk INSERT (regression: lazy db_ + bulk AI primer)")
    h.exec_script(f"""
SET SESSION max_recursive_iterations = 1000000;
CREATE TABLE bulk_ai (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, name VARCHAR(40)) ENGINE={h.engine};
INSERT INTO bulk_ai (name) WITH RECURSIVE seq AS (SELECT 1 AS i UNION ALL SELECT i+1 FROM seq WHERE i < 200) SELECT CONCAT('u_',i) FROM seq;
""")
    h.assert_scalar("200 rows inserted", "SELECT COUNT(*) FROM bulk_ai", "200")
    h.assert_scalar("200 distinct ids",  "SELECT COUNT(DISTINCT id) FROM bulk_ai", "200")
    h.assert_scalar("ids start at 1",    "SELECT MIN(id) FROM bulk_ai", "1")
    h.assert_scalar("ids end at 200",    "SELECT MAX(id) FROM bulk_ai", "200")

    h.section("AUTO_INCREMENT")
    h.exec_script(f"""
CREATE TABLE ai (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    label VARCHAR(20) NOT NULL
) ENGINE={h.engine};
""")
    h.assert_ok("AI insert 1", "INSERT INTO ai (label) VALUES ('first')")
    h.assert_ok("AI insert 2", "INSERT INTO ai (label) VALUES ('second')")
    h.assert_scalar("AI generated id 1", "SELECT id FROM ai WHERE label = 'first'",  "1")
    h.assert_scalar("AI generated id 2", "SELECT id FROM ai WHERE label = 'second'", "2")
    h.assert_ok("explicit AI value", "INSERT INTO ai VALUES (10, 'jump')")
    h.assert_ok("AI continues past explicit",
                "INSERT INTO ai (label) VALUES ('after')")
    h.assert_scalar("AI after explicit",
                    "SELECT id FROM ai WHERE label = 'after'", "11")

    h.section("AUTO_INCREMENT skips non-positive explicit values")
    # write_row used to cast val_int() to ulonglong before checking
    # sign. -2 became ULLONG_MAX-1, then either ai_note_explicit OR
    # the SELECT MAX() recovery path in get_auto_increment poisoned
    # the cache, and the next generated insert failed
    # ER_AUTOINC_READ_FAILED (ER 1467). The fix has two halves: (a)
    # skip ai_note_explicit for non-positive values on signed columns;
    # (b) clamp negative MAX() results to 0 in get_auto_increment.
    h.exec_script(f"""
CREATE TABLE ai_neg (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
                    lbl VARCHAR(20)) ENGINE={h.engine};
""")
    h.assert_ok("explicit -2 accepted (signed col)",
                "INSERT INTO ai_neg VALUES (-2, 'neg')")
    # No further inserts in between; next auto-gen should be id=1
    # (matches InnoDB control). Without the fix this fails ER 1467.
    h.assert_ok("auto after negative still works",
                "INSERT INTO ai_neg (lbl) VALUES ('after_neg')")
    h.assert_scalar("auto after negative gets id=1",
                    "SELECT id FROM ai_neg WHERE lbl = 'after_neg'", "1")

    h.section("AUTO_INCREMENT cache invalidates on TRUNCATE / DROP / RENAME")
    h.exec_script(f"""
CREATE TABLE ai_inv (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, lbl VARCHAR(20)) ENGINE={h.engine};
""")
    # Prime cache via chained statements in one connection.
    rc, out = h.run_client("""
INSERT INTO ai_inv (lbl) VALUES ('p1'),('p2'),('p3');
SELECT MAX(id) FROM ai_inv;""")
    h.assert_eq("primed AI to id=3", "3", out.strip().splitlines()[-1])

    h.run_client("TRUNCATE TABLE ai_inv")

    rc, out = h.run_client("""
INSERT INTO ai_inv (lbl) VALUES ('after');
SELECT id FROM ai_inv WHERE lbl='after';""")
    h.assert_eq("post-truncate id starts at 1", "1", out.strip().splitlines()[-1])

    h.run_client("INSERT INTO ai_inv (lbl) VALUES ('q1'),('q2'),('q3'),('q4'),('q5');")
    h.run_client(
        f"DROP TABLE ai_inv; "
        f"CREATE TABLE ai_inv (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, "
        f"lbl VARCHAR(20)) ENGINE={h.engine};")
    rc, out = h.run_client("""
INSERT INTO ai_inv (lbl) VALUES ('fresh');
SELECT id FROM ai_inv WHERE lbl='fresh';""")
    h.assert_eq("post-drop+create id starts at 1", "1",
                out.strip().splitlines()[-1])

    h.section("AUTO_INCREMENT allocator is cross-connection safe")
    # Connection A primes and stays open. Connection B inserts an
    # explicit high id while A sleeps. A's next generated id must jump
    # past B's insert, not reuse a stale per-connection counter.
    h.exec_script(f"""
DROP TABLE IF EXISTS ai_cross;
CREATE TABLE ai_cross (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, lbl VARCHAR(20)) ENGINE={h.engine};
""")
    a = h.run_async("""
INSERT INTO ai_cross (lbl) VALUES ('a1');
SELECT SLEEP(1);
INSERT INTO ai_cross (lbl) VALUES ('a_after');
SELECT id FROM ai_cross WHERE lbl='a_after';""")
    time.sleep(0.3)
    h.run_client("INSERT INTO ai_cross VALUES (100, 'jump')")
    out = a.wait()
    last = out.strip().splitlines()[-1] if out.strip() else ""
    h.assert_eq("cross-connection explicit jump", "101", last)

    # Same shape but second session generates id automatically.
    h.exec_script(f"""
DROP TABLE IF EXISTS ai_cross_auto;
CREATE TABLE ai_cross_auto (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, lbl VARCHAR(20)) ENGINE={h.engine};
""")
    a = h.run_async("""
INSERT INTO ai_cross_auto (lbl) VALUES ('a1');
SELECT SLEEP(1);
INSERT INTO ai_cross_auto (lbl) VALUES ('a_after');
SELECT id FROM ai_cross_auto WHERE lbl='a_after';""")
    time.sleep(0.3)
    h.run_client("INSERT INTO ai_cross_auto (lbl) VALUES ('b1')")
    out = a.wait()
    last = out.strip().splitlines()[-1] if out.strip() else ""
    h.assert_eq("cross-connection generated ids", "3", last)

    h.section("AUTO_INCREMENT respects auto_increment_offset / auto_increment_increment")
    h.exec_script(f"""
DROP TABLE IF EXISTS ai_inc_off;
CREATE TABLE ai_inc_off (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
                          lbl VARCHAR(20)) ENGINE={h.engine};
""")
    h.run_client("""
SET SESSION auto_increment_increment = 4;
SET SESSION auto_increment_offset    = 2;
INSERT INTO ai_inc_off (lbl) VALUES ('a'),('b'),('c'),('d');""")
    h.assert_scalar("inc=4/off=2 first id",
                    "SELECT id FROM ai_inc_off WHERE lbl='a'", "2")
    h.assert_scalar("inc=4/off=2 second id",
                    "SELECT id FROM ai_inc_off WHERE lbl='b'", "6")
    h.assert_scalar("inc=4/off=2 third id",
                    "SELECT id FROM ai_inc_off WHERE lbl='c'", "10")
    h.assert_scalar("inc=4/off=2 fourth id",
                    "SELECT id FROM ai_inc_off WHERE lbl='d'", "14")

    rc, out = h.run_client("""
SET SESSION auto_increment_increment = 1;
SET SESSION auto_increment_offset    = 1;
INSERT INTO ai_inc_off (lbl) VALUES ('after');
SELECT id FROM ai_inc_off WHERE lbl='after';""")
    last = out.strip().splitlines()[-1] if out.strip() else ""
    try:
        if int(last) > 14:
            h._pass(f"cross-session id past inc/off batch: {last}")
        else:
            h._fail(f"cross-session id <= 14 (engine cursor stale): {last}")
    except ValueError:
        h._fail("cross-session id parse failed", f"got: {last!r}")

    h.run_client("""
SET SESSION auto_increment_increment = 4;
SET SESSION auto_increment_offset    = 2;
INSERT INTO ai_inc_off (id, lbl) VALUES (100, 'big');
INSERT INTO ai_inc_off (lbl) VALUES ('past_big');""")
    h.assert_scalar("post-explicit auto with inc=4/off=2",
                    "SELECT id FROM ai_inc_off WHERE lbl='past_big'", "102")

    h.section("Bulk INSERT partial-row rollback (regression)")
    h.exec_script(f"""
CREATE TABLE br (id INT NOT NULL PRIMARY KEY, ts DATETIME NOT NULL, n INT NOT NULL) ENGINE={h.engine};
""")
    # Row 2's DATETIME is outside the int64-nanos range. extract_field
    # fails, partial row 2 must roll back cleanly.
    h.run_client(
        "INSERT INTO br VALUES (1,'2024-01-01',10),(2,'9999-01-01',20),(3,'2024-01-01',30);",
        force=True)

    h.assert_ok("follow-up batch after failed bulk",
                "INSERT INTO br VALUES (10,'2024-01-01',1000),"
                "(20,'2024-01-01',2000),(30,'2024-01-01',3000)")
    h.assert_scalar("follow-up row 10 aligned", "SELECT n FROM br WHERE id = 10", "1000")
    h.assert_scalar("follow-up row 20 aligned", "SELECT n FROM br WHERE id = 20", "2000")
    h.assert_scalar("follow-up row 30 aligned", "SELECT n FROM br WHERE id = 30", "3000")
