"""ALTER TABLE in place: one column added at the end, dropped or renamed,
and non-unique indexes added and dropped, change the stoolap table
without copying its rows. Any other ALTER copies (ALGORITHM=INPLACE is
refused, 1845, as InnoDB refuses what it cannot do in place), and so
does one changing more than one column: stoolap's DDL statements are not
atomic together.
"""

import mysql.connector

NO_INPLACE = "1845"


def run(h):
    h.section("ADD COLUMN at the end, its rows taking the default")
    h.exec_script(f"""
CREATE TABLE ia (id INT NOT NULL PRIMARY KEY, a INT, b VARCHAR(20),
  KEY ka (a)) ENGINE={h.engine};
INSERT INTO ia VALUES (1, 10, 'x'), (2, 20, 'y');
""")
    h.assert_ok("a text default with a quote and a backslash",
                "ALTER TABLE ia ADD COLUMN c VARCHAR(10) NOT NULL "
                "DEFAULT 'it''s \\\\ ok', ALGORITHM=INPLACE")
    h.assert_ok("a DOUBLE default",
                "ALTER TABLE ia ADD COLUMN d DOUBLE DEFAULT 1.5, "
                "ALGORITHM=INPLACE")
    h.assert_ok("a NOT NULL DATE, with no default",
                "ALTER TABLE ia ADD COLUMN e DATE NOT NULL, ALGORITHM=INPLACE")
    h.assert_ok("a DECIMAL default",
                "ALTER TABLE ia ADD COLUMN f DECIMAL(10,2) DEFAULT 3.25, "
                "ALGORITHM=INPLACE")
    h.assert_ok("a nullable column",
                "ALTER TABLE ia ADD COLUMN g INT, ALGORITHM=INPLACE")
    h.assert_scalar("the rows there hold the defaults",
                    "SELECT id, a, b, c, d, CAST(e AS CHAR), f, g FROM ia "
                    "WHERE id = 2",
                    "2\t20\ty\tit's \\ ok\t1.5\t0000-00-00\t3.25\tNULL")
    h.assert_ok("a new row", "INSERT INTO ia VALUES "
                "(3, 30, 'z', 'c3', 2.5, '2026-01-02', 1.10, 7)")
    h.assert_scalar("reads as written",
                    "SELECT c, d, e, f, g FROM ia WHERE id = 3",
                    "c3\t2.5\t2026-01-02\t1.10\t7")
    h.assert_scalar("by the key over a column there before",
                    "SELECT id, c FROM ia FORCE INDEX (ka) WHERE a = 10",
                    "1\tit's \\ ok")

    h.section("What copies")
    for label, sql in [
        ("a column not at the end", "ADD COLUMN h INT FIRST"),
        ("two columns", "ADD COLUMN h INT, ADD COLUMN i INT"),
        ("a default evaluated per row",
         "ADD COLUMN h TIMESTAMP DEFAULT CURRENT_TIMESTAMP"),
        ("an AUTO_INCREMENT column", "ADD COLUMN h INT AUTO_INCREMENT, "
         "ADD KEY (h)"),
        ("a type change", "MODIFY d INT"),
        ("a NULL change", "MODIFY g INT NOT NULL"),
        ("a UNIQUE key", "ADD UNIQUE (c)"),
        ("a column moved", "MODIFY g INT AFTER a"),
        ("a column dropped and one added", "DROP COLUMN g, ADD COLUMN h INT"),
    ]:
        h.assert_err(f"{label} is not in place",
                     f"ALTER TABLE ia {sql}, ALGORITHM=INPLACE", NO_INPLACE)
    h.assert_ok("and copies", "ALTER TABLE ia ADD COLUMN h INT, "
                "ADD COLUMN i INT")
    h.assert_scalar("keeping the rows", "SELECT COUNT(*), SUM(h IS NULL) "
                    "FROM ia", "3\t3")

    h.section("DROP COLUMN")
    h.assert_ok("a column with a key",
                "ALTER TABLE ia DROP COLUMN a, ALGORITHM=INPLACE")
    h.assert_scalar("the columns after it move up",
                    "SELECT id, b, c, d, CAST(e AS CHAR), f, g, h, i FROM ia "
                    "WHERE id = 1",
                    "1\tx\tit's \\ ok\t1.5\t0000-00-00\t3.25\tNULL\tNULL\tNULL")
    h.assert_ok("a row of the new shape",
                "INSERT INTO ia (id, b, c, e) VALUES (4, 'w', 'c4', "
                "'2026-03-04')")
    h.assert_scalar("reads back", "SELECT b, c, e FROM ia WHERE id = 4",
                    "w\tc4\t2026-03-04")
    h.assert_ok("the last column", "ALTER TABLE ia DROP COLUMN i, "
                "ALGORITHM=INPLACE")

    h.section("RENAME COLUMN and CHANGE to the same type")
    h.assert_ok("RENAME COLUMN",
                "ALTER TABLE ia RENAME COLUMN b TO bb, ALGORITHM=INPLACE")
    h.assert_ok("CHANGE", "ALTER TABLE ia CHANGE c cc VARCHAR(10) NOT NULL "
                "DEFAULT 'it''s \\\\ ok', ALGORITHM=INPLACE")
    h.assert_ok("in other letter case",
                "ALTER TABLE ia RENAME COLUMN bb TO BB, ALGORITHM=INPLACE")
    h.assert_scalar("the renamed columns read",
                    "SELECT BB, cc FROM ia WHERE id = 2", "y\tit's \\ ok")
    h.assert_scalar("and filter", "SELECT id FROM ia WHERE bb = 'w'", "4")
    h.assert_ok("and take writes",
                "UPDATE ia SET BB = 'v', cc = 'c5' WHERE id = 4")
    h.assert_scalar("updated", "SELECT BB, cc FROM ia WHERE id = 4",
                    "v\tc5")

    h.section("Non-unique indexes and defaults")
    h.assert_ok("ADD INDEX", "ALTER TABLE ia ADD INDEX kb (BB), "
                "ALGORITHM=INPLACE")
    h.assert_ok("CREATE INDEX", "CREATE INDEX kd ON ia (d, cc) "
                "ALGORITHM=INPLACE")
    h.assert_scalar("by the added index",
                    "SELECT id FROM ia FORCE INDEX (kb) WHERE BB = 'y'", "2")
    h.assert_scalar("by the added composite index",
                    "SELECT id FROM ia FORCE INDEX (kd) WHERE d = 2.5", "3")
    h.assert_ok("DROP INDEX and ADD INDEX of its name",
                "ALTER TABLE ia DROP INDEX kb, ADD INDEX kb (BB, cc), "
                "ALGORITHM=INPLACE")
    h.assert_scalar("by the index of the new shape",
                    "SELECT id FROM ia FORCE INDEX (kb) WHERE BB = 'v' "
                    "AND cc = 'c5'", "4")
    h.assert_ok("DROP INDEX", "ALTER TABLE ia DROP INDEX kd, "
                "ALGORITHM=INPLACE")
    h.assert_ok("a default changes at once",
                "ALTER TABLE ia ALTER COLUMN d SET DEFAULT 9, "
                "ALGORITHM=INSTANT")
    h.assert_ok("an insert takes it", "INSERT INTO ia (id, e) VALUES "
                "(5, '2026-05-06')")
    h.assert_scalar("as MariaDB writes it", "SELECT d FROM ia WHERE id = 5",
                    "9")

    h.section("Statements across an in-place ALTER")
    # Prepared and pushed statements of the old shape read the new one
    c, cur = h.open_session()
    try:
        cur.execute("PREPARE s FROM 'SELECT * FROM ia WHERE id = ?'")
        cur.execute("SET @i = 2")
        cur.execute("EXECUTE s USING @i")
        before = cur.fetchall()
        cur.execute("SELECT COUNT(*), MAX(d) FROM ia")
        cur.fetchall()
        h.exec_stmt("ALTER TABLE ia ADD COLUMN j INT DEFAULT 42, "
                    "ALGORITHM=INPLACE")
        cur.execute("EXECUTE s USING @i")
        after = cur.fetchall()
        h.assert_eq("a prepared SELECT * reads the added column",
                    list(before[0]) + [42], list(after[0]))
        h.exec_stmt("ALTER TABLE ia DROP COLUMN bb, ALGORITHM=INPLACE")
        cur.execute("EXECUTE s USING @i")
        h.assert_eq("and goes without the dropped one",
                    [list(before[0])[0]] + list(before[0])[2:] + [42],
                    list(cur.fetchall()[0]))
        cur.execute("SELECT COUNT(*), MAX(d) FROM ia")
        h.assert_eq("a pushed SELECT of the session", [(5, 9.0)],
                    cur.fetchall())
    except mysql.connector.Error as e:
        h._fail("statements across an in-place ALTER", str(e))
    finally:
        c.close()
    h.assert_pushed("pushed after the ALTER",
                    "SELECT COUNT(*), MAX(d) FROM ia")

    h.section("An open transaction holds the table")
    w, wcur = h.open_session(autocommit=False)
    try:
        wcur.execute("SELECT COUNT(*) FROM ia")
        wcur.fetchall()
        h.exec_stmt("SET SESSION lock_wait_timeout = 1")
        h.assert_err("the ALTER waits for it",
                     "ALTER TABLE ia ADD COLUMN k INT, ALGORITHM=INPLACE",
                     "1205")
    finally:
        h.exec_stmt("SET SESSION lock_wait_timeout = DEFAULT")
        w.rollback()
        w.close()
    h.assert_ok("and goes ahead once it ends",
                "ALTER TABLE ia ADD COLUMN k INT, ALGORITHM=INPLACE")

    h.section("What the copy refuses")
    h.assert_err("a COMPRESSED column is not added in place",
                 "ALTER TABLE ia ADD COLUMN m TEXT COMPRESSED, "
                 "ALGORITHM=INPLACE", NO_INPLACE)
    h.assert_err("and is refused", "ALTER TABLE ia ADD COLUMN m TEXT "
                 "COMPRESSED", "COMPRESSED BLOB / TEXT")
    c, cur = h.open_session()
    try:
        cur.execute("SET SESSION sql_mode = 'STRICT_ALL_TABLES,NO_ZERO_DATE'")
        try:
            cur.execute("ALTER TABLE ia ADD COLUMN m DATE NOT NULL")
            h._fail("a NOT NULL DATE under NO_ZERO_DATE is refused (1292)",
                    "no error")
        except mysql.connector.Error as e:
            h.assert_eq("a NOT NULL DATE under NO_ZERO_DATE is refused "
                        "(1292), as InnoDB refuses it", 1292, e.errno)
    finally:
        c.close()

    h.section("With a rename, and a TEMPORARY table")
    h.assert_ok("a column added and the table renamed",
                "ALTER TABLE ia ADD COLUMN l INT DEFAULT 6, RENAME TO ib, "
                "ALGORITHM=INPLACE")
    h.assert_scalar("the renamed table holds both",
                    "SELECT COUNT(*), SUM(l) FROM ib", "5\t30")
    # Its indexes keep the names they were created with
    h.assert_ok("an index of the renamed table dropped",
                "ALTER TABLE ib DROP INDEX kb, ALGORITHM=INPLACE")
    h.assert_ok("and added again",
                "ALTER TABLE ib ADD INDEX kb (cc), ALGORITHM=INPLACE")
    h.assert_scalar("reads by it",
                    "SELECT id FROM ib FORCE INDEX (kb) WHERE cc = 'c5'", "4")
    # MariaDB copies a TEMPORARY table, as it does for InnoDB
    h.assert_ok("a TEMPORARY table shadowing it",
                "CREATE TEMPORARY TABLE ib (id INT NOT NULL PRIMARY KEY, "
                f"a INT) ENGINE={h.engine}")
    h.assert_err("is not altered in place",
                 "ALTER TABLE ib ADD COLUMN b INT, ALGORITHM=INPLACE",
                 NO_INPLACE)
    h.assert_ok("but copied", "ALTER TABLE ib ADD COLUMN b INT DEFAULT 8")
    h.assert_ok("a row", "INSERT INTO ib (id, a) VALUES (1, 1)")
    h.assert_scalar("of the TEMPORARY table", "SELECT a, b FROM ib", "1\t8")
    h.assert_ok("dropped", "DROP TEMPORARY TABLE ib")
    h.assert_scalar("the permanent one is as it was",
                    "SELECT COUNT(*), SUM(l) FROM ib", "5\t30")

    h.section("FOREIGN KEYs")
    h.exec_script(f"""
CREATE TABLE ip (id INT NOT NULL PRIMARY KEY, v INT) ENGINE={h.engine};
CREATE TABLE ic (id INT NOT NULL PRIMARY KEY, pid INT,
  FOREIGN KEY (pid) REFERENCES ip (id)) ENGINE={h.engine};
INSERT INTO ip VALUES (1, 1), (2, 2);
INSERT INTO ic VALUES (1, 1);
""")
    # In place, the parent keeps its name: no copy for the keys to miss
    h.assert_ok("a referenced parent takes a column",
                "ALTER TABLE ip ADD COLUMN w INT DEFAULT 3, ALGORITHM=INPLACE")
    h.assert_ok("and drops one",
                "ALTER TABLE ip DROP COLUMN v, ALGORITHM=INPLACE")
    h.assert_err("its referenced column stays",
                 "ALTER TABLE ip DROP COLUMN id, ALGORITHM=INPLACE",
                 "references it|1845|primary")
    h.assert_ok("the child's column renamed",
                "ALTER TABLE ic RENAME COLUMN pid TO qid, ALGORITHM=INPLACE")
    h.assert_ok("the parent's too",
                "ALTER TABLE ip RENAME COLUMN id TO pk, ALGORITHM=INPLACE")
    h.assert_ok("the key holds a child of a parent row",
                "INSERT INTO ic VALUES (2, 2)")
    h.assert_err("and refuses an orphan", "INSERT INTO ic VALUES (3, 9)",
                 "1452")
    h.assert_err("and a parent row's delete",
                 "DELETE FROM ip WHERE pk = 1", "1451")
    h.assert_err("the key's column is not dropped",
                 "ALTER TABLE ic DROP COLUMN qid, ALGORITHM=INPLACE",
                 "used by a FOREIGN KEY")
    h.assert_err("the key still holds after the refusal",
                 "INSERT INTO ic VALUES (4, 9)", "1452")
    h.exec_script("DROP TABLE ic; DROP TABLE ip")

    h.section("A key's index, not another's of its name")
    # `_d`.`t`'s first UNIQUE index is stoolap's unique__0d_1t_0, named as
    # the index of a key `0d_1t_0` would be; dropping that key keeps it
    h.exec_script(f"""
DROP DATABASE IF EXISTS `_d`;
CREATE DATABASE `_d`;
CREATE TABLE `_d`.t (id INT NOT NULL PRIMARY KEY, a INT UNIQUE, b INT,
  KEY `0d_1t_0` (b)) ENGINE={h.engine};
INSERT INTO `_d`.t VALUES (1, 1, 1);
""", use_db=False)
    h.assert_ok("the key dropped", "ALTER TABLE `_d`.t DROP INDEX `0d_1t_0`, "
                "ALGORITHM=INPLACE")
    h.assert_err("the UNIQUE key still holds",
                 "INSERT INTO `_d`.t VALUES (2, 1, 2)", "1062")
    h.exec_script("DROP DATABASE `_d`", use_db=False)

    h.section("Keys over the same columns, and a key changed with a rename")
    # Stoolap keeps one index per column list: the keys over it share it,
    # and it goes with the last of them
    h.exec_script(f"""
CREATE TABLE iq (id INT NOT NULL PRIMARY KEY, a INT, b INT,
  KEY p__k (a, b), KEY k (a, b)) ENGINE={h.engine};
INSERT INTO iq VALUES (1, 1, 1), (2, 1, 2);
CREATE TABLE ir (id INT NOT NULL PRIMARY KEY, a INT, b INT, KEY k (a))
  ENGINE={h.engine};
INSERT INTO ir VALUES (1, 1, 2), (2, 2, 1);
""")
    h.assert_ok("a key over another's columns dropped",
                "ALTER TABLE iq DROP INDEX k, ALGORITHM=INPLACE")
    h.assert_scalar("the other reads by it",
                    "SELECT id FROM iq FORCE INDEX (p__k) WHERE a = 1 "
                    "AND b = 2", "2")
    h.assert_ok("then the other",
                "ALTER TABLE iq DROP INDEX p__k, ALGORITHM=INPLACE")
    h.assert_ok("a column renamed, and its key moved to another",
                "ALTER TABLE ir RENAME COLUMN a TO aa, DROP INDEX k, "
                "ADD INDEX k (b), ALGORITHM=INPLACE")
    h.assert_scalar("reads by the moved key",
                    "SELECT id FROM ir FORCE INDEX (k) WHERE b = 1", "2")
    # j shares k's index; k moved to other columns takes a name of its own
    h.assert_ok("a key over a kept key's columns",
                "ALTER TABLE ir ADD INDEX j (b), ALGORITHM=INPLACE")
    h.assert_ok("the kept key moved",
                "ALTER TABLE ir DROP INDEX k, ADD INDEX k (aa), "
                "ALGORITHM=INPLACE")
    h.assert_scalar("reads by the moved key",
                    "SELECT id FROM ir FORCE INDEX (k) WHERE aa = 2", "2")
    h.assert_scalar("and by the other",
                    "SELECT id FROM ir FORCE INDEX (j) WHERE b = 2", "1")
    h.exec_script("DROP TABLE iq; DROP TABLE ir")
