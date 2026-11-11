"""Foreign key enforcement: CREATE-time, ALTER ADD CONSTRAINT, INSERT/UPDATE/
DELETE-time validation. Stoolap supports single-column FKs only."""

import mysql.connector


def run(h):
    h.exec_script(f"""
CREATE TABLE p (
    id INT NOT NULL PRIMARY KEY,
    name VARCHAR(40) NOT NULL
) ENGINE={h.engine};
INSERT INTO p VALUES (1,'parent-1'), (2,'parent-2');
CREATE TABLE c (
    id INT NOT NULL PRIMARY KEY,
    pid INT NOT NULL,
    note VARCHAR(40),
    CONSTRAINT fk_c_p FOREIGN KEY (pid) REFERENCES p(id)
) ENGINE={h.engine};
""")

    h.section("FK enforcement on INSERT")
    h.assert_ok("valid child insert", "INSERT INTO c VALUES (10, 1, 'ok')")
    h.assert_err("invalid parent",
                 "INSERT INTO c VALUES (11, 999, 'bad')",
                 r"REFERENCED|FOREIGN|foreign|reference|Got error|ERROR")
    h.assert_scalar("no row created", "SELECT COUNT(*) FROM c WHERE id = 11", "0")

    h.section("FK enforcement on UPDATE")
    h.assert_ok("valid update", "UPDATE c SET pid = 2 WHERE id = 10")
    h.assert_err("update to bad parent",
                 "UPDATE c SET pid = 999 WHERE id = 10",
                 r"REFERENCED|FOREIGN|foreign|reference|Got error|ERROR")
    h.assert_scalar("still pointing at 2", "SELECT pid FROM c WHERE id = 10", "2")

    h.section("ALTER ADD CONSTRAINT")
    h.exec_script(f"""
CREATE TABLE c2 (
    id INT NOT NULL PRIMARY KEY,
    pid INT NOT NULL
) ENGINE={h.engine};
""")
    h.assert_ok("ALTER ADD FK",
                "ALTER TABLE c2 ADD CONSTRAINT fk_c2 FOREIGN KEY (pid) REFERENCES p(id)")
    h.assert_ok("valid c2 insert", "INSERT INTO c2 VALUES (1, 1)")
    h.assert_err("invalid c2 insert",
                 "INSERT INTO c2 VALUES (2, 999)",
                 r"REFERENCED|FOREIGN|foreign|reference|Got error|ERROR")

    h.section("Failed TRUNCATE does not poison row-count cache (regression)")
    h.exec_script(f"""
CREATE TABLE tparent (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
CREATE TABLE tchild (id INT NOT NULL PRIMARY KEY, pid INT NOT NULL,
                       FOREIGN KEY (pid) REFERENCES tparent(id)) ENGINE={h.engine};
INSERT INTO tparent VALUES (1,10),(2,20),(3,30);
INSERT INTO tchild  VALUES (10,1),(20,2);
""")
    h.assert_err("TRUNCATE rejected by FK",
                 "TRUNCATE TABLE tparent",
                 r"foreign|Got error|ERROR")
    h.assert_scalar("post-failed-TRUNCATE COUNT", "SELECT COUNT(*) FROM tparent", "3")
    h.assert_scalar("rows are still queryable",
                    "SELECT id FROM tparent WHERE id = 2", "2")

    h.assert_ok("TRUNCATE child",           "TRUNCATE TABLE tchild")
    h.assert_ok("TRUNCATE parent (now ok)", "TRUNCATE TABLE tparent")
    h.assert_scalar("post-success TRUNCATE COUNT", "SELECT COUNT(*) FROM tparent", "0")

    # The FOREIGN KEYs come from MariaDB's parser: a NUL inside a literal
    # before the clause, or a client in another character set, changes
    # nothing.
    h.section("FOREIGN KEY next to a NUL literal and non-UTF-8 text")
    h.exec_script(f"""
CREATE TABLE np (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO np VALUES (1);
""")
    h.cur.execute(
        "CREATE TABLE nc (id INT NOT NULL PRIMARY KEY, b VARBINARY(4) "
        "DEFAULT 'a\x00b', pid INT, FOREIGN KEY (pid) REFERENCES np (id)) "
        f"ENGINE={h.engine}")
    h.assert_err("FK after a NUL literal is enforced",
                 "INSERT INTO nc (id, pid) VALUES (1, 99)",
                 r"foreign key constraint fails")
    h.assert_ok("FK after a NUL literal accepts a parent row",
                "INSERT INTO nc (id, pid) VALUES (2, 1)")
    c = mysql.connector.connect(user="root", unix_socket=h.socket,
                                database=h.db, autocommit=True,
                                charset="latin1", use_pure=True)
    try:
        c.cursor().execute(
            "CREATE TABLE nl (id INT NOT NULL PRIMARY KEY, pid INT, "
            "FOREIGN KEY (pid) REFERENCES np (id)) "
            f"ENGINE={h.engine} COMMENT='café'")
        h._pass("CREATE from a latin1 connection")
    except mysql.connector.Error as e:
        h._fail("CREATE from a latin1 connection", str(e))
    finally:
        c.close()
    h.assert_err("FK of the latin1 CREATE is enforced",
                 "INSERT INTO nl VALUES (1, 99)",
                 r"foreign key constraint fails")
    # A latin1 name binds to its own column, not to U+FFFD, which is
    # another column's name: the parser converts it.
    h.exec_stmt("CREATE TABLE nq (`\u00e9` INT NOT NULL PRIMARY KEY, "
                f"`\ufffd` INT, UNIQUE KEY u (`\ufffd`)) ENGINE={h.engine}")
    h.exec_stmt("INSERT INTO nq VALUES (1, 99)")
    c = mysql.connector.connect(user="root", unix_socket=h.socket,
                                database=h.db, autocommit=True,
                                charset="latin1", use_pure=True)
    try:
        c.cursor().execute(
            "CREATE TABLE nr (id INT NOT NULL PRIMARY KEY, pid INT, "
            "FOREIGN KEY (pid) REFERENCES nq (`\u00e9`)) "
            f"ENGINE={h.engine}")
        h._pass("latin1 FK names a column")
    except mysql.connector.Error as e:
        h._fail("latin1 FK names a column", str(e))
    finally:
        c.close()
    h.assert_ok("latin1 FK accepts the parent's key",
                "INSERT INTO nr VALUES (1, 1)")
    h.assert_err("latin1 FK refuses the other column's value",
                 "INSERT INTO nr VALUES (2, 99)",
                 r"foreign key constraint fails")

    h.section("A row-pump DELETE runs the parent's ON DELETE action")
    # The delete holds its row first under the check that it is unchanged;
    # that must not write a column a key names, whose ON UPDATE action
    # (RESTRICT here) is not the statement's
    h.exec_script(f"""
CREATE TABLE odp (id INT NOT NULL PRIMARY KEY, k INT NOT NULL UNIQUE)
    ENGINE={h.engine};
INSERT INTO odp VALUES (1, 10);
CREATE TABLE odc (id INT NOT NULL PRIMARY KEY, pk INT,
                  FOREIGN KEY (pk) REFERENCES odp(k)
                  ON DELETE CASCADE ON UPDATE RESTRICT) ENGINE={h.engine};
INSERT INTO odc VALUES (1, 10);
""")
    h.assert_ok("DELETE of a referenced row through the row pump",
                "DELETE FROM odp WHERE id = 1 AND k = 10 ORDER BY id LIMIT 1")
    h.assert_scalar("the child row cascaded", "SELECT COUNT(*) FROM odc", "0")

    h.section("DROP DATABASE takes a chain of keys with rows")
    # Stoolap refuses to drop a table whose rows another table's rows
    # reference: each child goes first, down the chain
    h.exec_script("DROP DATABASE IF EXISTS fk_chain", use_db=False, ignore_errors=True)
    h.exec_script(f"""
CREATE DATABASE fk_chain;
CREATE TABLE fk_chain.a (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
CREATE TABLE fk_chain.b (id INT NOT NULL PRIMARY KEY, x INT,
                         FOREIGN KEY (x) REFERENCES fk_chain.a (id)) ENGINE={h.engine};
CREATE TABLE fk_chain.c (id INT NOT NULL PRIMARY KEY, x INT, y INT,
                         FOREIGN KEY (x) REFERENCES fk_chain.b (id),
                         FOREIGN KEY (y) REFERENCES fk_chain.a (id)) ENGINE={h.engine};
CREATE TABLE fk_chain.d (id INT NOT NULL PRIMARY KEY, x INT,
                         FOREIGN KEY (x) REFERENCES fk_chain.c (id)) ENGINE={h.engine};
INSERT INTO fk_chain.a VALUES (1);
INSERT INTO fk_chain.b VALUES (1, 1);
INSERT INTO fk_chain.c VALUES (1, 1, 1);
INSERT INTO fk_chain.d VALUES (1, 1);
""", use_db=False)
    h.assert_err("a referenced parent alone is still refused",
                 "DROP TABLE fk_chain.b", "1451")
    # A copying ALTER of a table other tables' keys name is refused: the
    # copy is swapped in by renames, which take those keys to the old table
    # (stoolap#987). The keys keep naming the table, and the database drops
    h.assert_err("a copying ALTER of a referenced table is refused",
                 "ALTER TABLE fk_chain.b ADD COLUMN w INT, ALGORITHM=COPY",
                 r"FOREIGN KEY of .* names it")
    h.assert_ok("a row of the table takes a new child",
                "INSERT INTO fk_chain.b VALUES (2, 1)")
    h.assert_ok("which names it", "INSERT INTO fk_chain.c VALUES (2, 2, 1)")
    h.assert_err("the key still checks the table",
                 "INSERT INTO fk_chain.c VALUES (3, 9, 1)", "1452")
    h.assert_ok("the database drops", "DROP DATABASE fk_chain")
    # A key's CREATE locks its parent against a copying ALTER of it, which
    # would leave the key with the old table: it waits for a statement that
    # holds the parent, as an ALTER does, here a write in an open transaction
    h.exec_stmt(f"CREATE TABLE lp (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine}")
    w = mysql.connector.connect(user="root", unix_socket=h.socket,
                                database=h.db, autocommit=False)
    try:
        w.cursor().execute("INSERT INTO lp VALUES (1)")
        h.exec_stmt("SET SESSION lock_wait_timeout = 1")
        h.assert_err("a key's CREATE waits for a statement holding its parent",
                     "CREATE TABLE lc (id INT NOT NULL PRIMARY KEY, pid INT, "
                     f"FOREIGN KEY (pid) REFERENCES lp (id)) ENGINE={h.engine}",
                     "1205")
        # Reading the parent too, which takes a weaker lock on it
        h.assert_err("so does a CREATE ... SELECT reading its parent",
                     "CREATE TABLE ls (id INT NOT NULL PRIMARY KEY, pid INT, "
                     f"FOREIGN KEY (pid) REFERENCES lp (id)) ENGINE={h.engine} "
                     "AS SELECT id, id AS pid FROM lp WHERE 0",
                     "1205")
    finally:
        h.exec_stmt("SET SESSION lock_wait_timeout = DEFAULT")
        w.rollback()
        w.close()
    h.assert_ok("and goes ahead once it ends",
                "CREATE TABLE lc (id INT NOT NULL PRIMARY KEY, pid INT, "
                f"FOREIGN KEY (pid) REFERENCES lp (id)) ENGINE={h.engine}")
    # The parent is the table MariaDB finds by that name: by its letter case
    # under lower_case_table_names = 0, where InnoDB refuses the key too
    if h.sql("SELECT @@lower_case_table_names").strip() == "0":
        h.assert_err("a parent named in other letter case is no parent",
                     "CREATE TABLE lu (id INT NOT NULL PRIMARY KEY, pid INT, "
                     f"FOREIGN KEY (pid) REFERENCES LP (id)) ENGINE={h.engine}",
                     r"non-existent table")
    h.assert_scalar("and is gone", "SELECT COUNT(*) FROM information_schema.schemata "
                    "WHERE schema_name = 'fk_chain'", "0")

    # Every spelling of a parent the clause reader takes, and refuses
    # rather than drop or misname: an FK it cannot read would leave the
    # table without the key.
    h.section("FOREIGN KEY clause spellings")
    h.exec_script(f"""
CREATE TABLE sp (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO sp VALUES (1);
""")
    c, cur = h.open_session()
    try:
        cur.execute("SET SESSION sql_mode = 'ANSI_QUOTES'")
        cur.execute("CREATE TABLE sa (pid INT, FOREIGN KEY (pid) REFERENCES "
                    f"\"sp\" (id)) ENGINE={h.engine}")
        h._pass("FK to a \"quoted\" parent under ANSI_QUOTES")
    except mysql.connector.Error as e:
        h._fail("FK to a \"quoted\" parent under ANSI_QUOTES", str(e))
    finally:
        c.close()
    h.assert_err("the ANSI_QUOTES FK is enforced",
                 "INSERT INTO sa VALUES (99)", r"foreign key constraint fails")
    h.assert_ok("FOREIGN inside a column name is not a clause",
                f"CREATE TABLE sx (x$FOREIGN INT) ENGINE={h.engine}")
    h.assert_ok("a table named foreign is not a clause",
                f"CREATE TABLE {h.db}.foreign (id INT) ENGINE={h.engine}")
    # MariaDB's lexer skips comments between tokens (this connector sends
    # them; the mariadb client strips them).
    h.cur.execute("CREATE TABLE sk (pid INT, FOREIGN /* a */ KEY (pid) -- b\n "
                  f"REFERENCES sp (id)) ENGINE={h.engine}")
    h.assert_err("an FK with comments between its tokens is enforced",
                 "INSERT INTO sk VALUES (99)", r"foreign key constraint fails")
    h.assert_ok("a comment naming FOREIGN KEY is not a clause",
                "CREATE TABLE sm (pid INT /* FOREIGN KEY later */, "
                f"x INT) ENGINE={h.engine}")
    # A parent whose file name MariaDB encodes (sp$1 is sp@00241), in
    # this database and another one whose name it encodes too
    h.exec_script(f"""
CREATE TABLE `sp$1` (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO `sp$1` VALUES (1);
CREATE DATABASE IF NOT EXISTS `my-db`;
CREATE TABLE `my-db`.`P\u00e9` (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO `my-db`.`P\u00e9` VALUES (1);
""")
    h.assert_ok("FK to a parent with an encoded file name",
                "CREATE TABLE sd (pid INT, FOREIGN KEY (pid) REFERENCES "
                f"`sp$1` (id)) ENGINE={h.engine}")
    h.assert_ok("encoded parent's key accepted", "INSERT INTO sd VALUES (1)")
    h.assert_err("the encoded parent's FK is enforced",
                 "INSERT INTO sd VALUES (99)", r"foreign key constraint fails")
    h.assert_ok("FK to an encoded parent in another database",
                "CREATE TABLE `my-db`.c (pid INT, FOREIGN KEY (pid) REFERENCES "
                f"`P\u00e9` (id) ON DELETE CASCADE) ENGINE={h.engine}")
    h.assert_ok("child row", "INSERT INTO `my-db`.c VALUES (1)")
    h.assert_err("the other database's FK is enforced",
                 "INSERT INTO `my-db`.c VALUES (99)",
                 r"foreign key constraint fails")
    h.exec_script("DROP TABLE `my-db`.c; DROP DATABASE `my-db`")
    # A TEMPORARY child's parent is in its database, not the tmpdir its
    # file sits in
    h.assert_ok("FK of a TEMPORARY child",
                "CREATE TEMPORARY TABLE tc (pid INT, FOREIGN KEY (pid) "
                f"REFERENCES sp (id)) ENGINE={h.engine}")
    h.assert_ok("TEMPORARY child row", "INSERT INTO tc VALUES (1)")
    h.assert_err("the TEMPORARY child's FK is enforced",
                 "INSERT INTO tc VALUES (99)", r"foreign key constraint fails")
    h.exec_stmt("DROP TEMPORARY TABLE tc")
    # A copying ALTER keeps the keys (tests/diff/alter_keeps_fks.sql);
    # dropping a column one uses is refused, as InnoDB refuses it (1828
    # there)
    h.exec_stmt("CREATE TABLE sz (id INT NOT NULL PRIMARY KEY, pid INT, "
                f"FOREIGN KEY (pid) REFERENCES sp (id)) ENGINE={h.engine}")
    h.assert_err("dropping a FOREIGN KEY's column is refused",
                 "ALTER TABLE sz DROP COLUMN pid",
                 r"used by a FOREIGN KEY")
    h.assert_err("the key survives the refused ALTER",
                 "INSERT INTO sz VALUES (1, 98)",
                 r"foreign key constraint fails")
    # A dropped column's key does not move to the column renamed into its
    # place
    h.exec_stmt("CREATE TABLE sw (id INT NOT NULL PRIMARY KEY, pid INT, "
                f"qid INT, FOREIGN KEY (pid) REFERENCES sp (id)) "
                f"ENGINE={h.engine}")
    h.assert_err("a dropped key column renamed over is refused",
                 "ALTER TABLE sw DROP COLUMN pid, CHANGE qid pid INT",
                 r"used by a FOREIGN KEY")
    # Nor does a key naming its own table move to the column renamed into
    # its parent column's place (1829 in InnoDB)
    h.exec_script(f"""
CREATE TABLE sv (id INT NOT NULL PRIMARY KEY, u INT, x INT, pid INT,
  UNIQUE KEY (u), UNIQUE KEY (x), FOREIGN KEY (pid) REFERENCES sv (u))
  ENGINE={h.engine};
INSERT INTO sv VALUES (1, 10, 20, NULL), (2, 20, 10, 10);
""")
    h.assert_err("a dropped own parent column renamed over is refused",
                 "ALTER TABLE sv DROP COLUMN u, CHANGE COLUMN x u INT",
                 r"used by a FOREIGN KEY")
    h.assert_err("the key still names the parent column",
                 "INSERT INTO sv VALUES (3, 30, 40, 40)",
                 r"foreign key constraint fails")
    # SET NULL on a NOT NULL column is refused, for a key naming its own
    # table too, and when a later ALTER makes the column NOT NULL (1005 /
    # 1830 in InnoDB)
    h.assert_err("own key ON UPDATE SET NULL on NOT NULL refused",
                 "CREATE TABLE sx1 (id INT NOT NULL PRIMARY KEY, pid INT NOT "
                 "NULL, FOREIGN KEY (pid) REFERENCES sx1 (id) ON UPDATE SET "
                 f"NULL) ENGINE={h.engine}",
                 r"SET NULL but is NOT NULL")
    h.exec_stmt("CREATE TABLE sx2 (id INT NOT NULL PRIMARY KEY, pid INT, "
                "FOREIGN KEY (pid) REFERENCES sx2 (id) ON UPDATE SET NULL) "
                f"ENGINE={h.engine}")
    h.assert_err("making its column NOT NULL is refused",
                 "ALTER TABLE sx2 MODIFY pid INT NOT NULL",
                 r"SET NULL but is NOT NULL")
    # A TEMPORARY table keeps its own keys, not those of the permanent
    # table its name shadows
    h.assert_ok("ALTER of a TEMPORARY table shadowing a child",
                f"CREATE TEMPORARY TABLE sw (id INT) ENGINE={h.engine}")
    h.assert_ok("copying ALTER of the TEMPORARY table",
                "ALTER TABLE sw ADD COLUMN z INT, ALGORITHM=COPY")
    h.exec_stmt("DROP TEMPORARY TABLE sw")
    # A key naming its column in other letter case, past ASCII
    h.assert_ok("FK naming its column in other case",
                "CREATE TABLE su (`\u00e9` INT, FOREIGN KEY (`\u00c9`) "
                f"REFERENCES sp (id)) ENGINE={h.engine}")
    h.assert_ok("copying ALTER keeps it", "ALTER TABLE su ADD COLUMN z INT")
    h.assert_err("the key is enforced after the ALTER",
                 "INSERT INTO su VALUES (97, NULL)",
                 r"foreign key constraint fails")
    h.assert_ok("renaming its column keeps it",
                "ALTER TABLE su CHANGE `\u00c9` `\u00f6` INT")
    h.assert_err("the key follows the rename",
                 "INSERT INTO su VALUES (96, NULL)",
                 r"foreign key constraint fails")
    # A table converted from another engine has no stoolap keys to keep
    h.exec_script("CREATE TABLE si (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;"
                  "INSERT INTO si VALUES (1)")
    h.assert_ok("ALTER ... ENGINE=STOOLAP", f"ALTER TABLE si ENGINE={h.engine}")
    h.assert_scalar("converted rows", "SELECT COUNT(*) FROM si", "1")
    # A value whose text reads like a parent-side error is still the
    # child's missing parent (1452), not a referenced parent (1451)
    h.exec_script(f"""
CREATE TABLE vp (id INT NOT NULL PRIMARY KEY,
  code VARCHAR(30) COLLATE utf8mb4_bin NOT NULL UNIQUE) ENGINE={h.engine};
CREATE TABLE vc (code VARCHAR(30) COLLATE utf8mb4_bin,
  FOREIGN KEY (code) REFERENCES vp (code))
  ENGINE={h.engine};
""")
    h.assert_err("a missing parent named 'still reference it'",
                 "INSERT INTO vc VALUES ('still reference it')",
                 r"add or update a child row")
    h.assert_err("a missing parent named 'still referenced by table'",
                 "INSERT INTO vc VALUES ('still referenced by table')",
                 r"add or update a child row")
    # A NUL in the key keeps the error's own class: the message the plugin
    # reads escapes it, and a column naming the detail's separator does
    # not mislead the wording fallback
    h.exec_script(f"""
CREATE TABLE np2 (id INT NOT NULL PRIMARY KEY,
  code VARCHAR(30) COLLATE utf8mb4_bin UNIQUE) ENGINE={h.engine};
CREATE TABLE nc2 (`x)' \u2014 y` VARCHAR(30) COLLATE utf8mb4_bin,
  FOREIGN KEY (`x)' \u2014 y`) REFERENCES np2 (code)) ENGINE={h.engine};
INSERT INTO np2 VALUES (1, CHAR(0));
INSERT INTO nc2 VALUES (CHAR(0));
""")
    h.assert_err("deleting a parent row with a NUL key is 1451",
                 "DELETE FROM np2 WHERE id = 1", r"\b1451\b")
    # DROP DATABASE takes its own children with it, though their stoolap
    # names are shortened past its prefix
    longdb = "$" * 36 + "_" * 28
    h.exec_script(f"""
CREATE DATABASE `{longdb}`;
CREATE TABLE `{longdb}`.a (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
CREATE TABLE `{longdb}`.`{"z" * 20}` (aid INT, FOREIGN KEY (aid) REFERENCES a (id))
  ENGINE={h.engine};
""")
    h.assert_ok("DROP DATABASE with a shortened child",
                f"DROP DATABASE `{longdb}`")
    # The actions come from the parser too
    h.exec_script(f"""
CREATE TABLE ap (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO ap VALUES (1), (2);
CREATE TABLE ac (id INT NOT NULL PRIMARY KEY, pid INT, FOREIGN KEY (pid)
  REFERENCES ap (id) ON UPDATE CASCADE ON DELETE SET NULL) ENGINE={h.engine};
INSERT INTO ac VALUES (1, 1), (2, 2);
""")
    h.assert_ok("delete a parent row", "DELETE FROM ap WHERE id = 1")
    h.assert_scalar("ON DELETE SET NULL ran",
                    "SELECT COUNT(*) FROM ac WHERE pid IS NULL", "1")
