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
