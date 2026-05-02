"""Foreign key enforcement: CREATE-time, ALTER ADD CONSTRAINT, INSERT/UPDATE/
DELETE-time validation. Stoolap supports single-column FKs only."""


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
