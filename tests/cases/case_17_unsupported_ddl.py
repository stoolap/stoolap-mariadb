"""Schema constructs stoolap can't enforce must be rejected at CREATE
TABLE / ALTER TABLE time, not silently accepted (which would let the
user think the constraint was in place while duplicate / unreferenced
rows quietly went in). Plus: direct UPDATE/DELETE constraint errors
must map to MariaDB's specific error codes, not the generic 1296."""


def run(h):
    h.section("PRIMARY KEY / UNIQUE over a column prefix refused")
    # Stoolap would enforce uniqueness on the whole column, so UNIQUE
    # (a(3), b) would accept ('abc1', 1) and ('abc2', 1), which collide
    # on the declared prefix. Refuse rather than weaken the constraint.
    for name, ddl in (
        ("prefix_uq_comp",
         "id INT NOT NULL PRIMARY KEY, a VARCHAR(20) COLLATE utf8mb4_bin, "
         "b INT, UNIQUE KEY uq (a(3), b)"),
        ("prefix_uq",
         "id INT NOT NULL PRIMARY KEY, a VARCHAR(20) COLLATE utf8mb4_bin, "
         "UNIQUE KEY uq (a(3))"),
        ("prefix_pk",
         "a VARCHAR(20) COLLATE utf8mb4_bin NOT NULL, b INT NOT NULL, "
         "PRIMARY KEY (a(3), b)"),
    ):
        h.assert_err(f"{name} rejected at CREATE",
                     f"CREATE TABLE {name} ({ddl}) ENGINE={h.engine}",
                     r"column prefix")
        h.assert_scalar(f"{name} table not present",
                        "SELECT COUNT(*) FROM information_schema.tables "
                        f"WHERE table_schema = '{h.db}' "
                        f"AND table_name = '{name}'", "0")
    h.exec_script(f"""
CREATE TABLE prefix_alter (id INT NOT NULL PRIMARY KEY,
                           a VARCHAR(20) COLLATE utf8mb4_bin) ENGINE={h.engine};
""")
    h.assert_err("ALTER ADD UNIQUE over a prefix rejected",
                 "ALTER TABLE prefix_alter ADD UNIQUE KEY uq (a(3))",
                 r"column prefix")

    h.section("Composite FOREIGN KEY refused")
    h.exec_script(f"""
CREATE TABLE pp (a INT NOT NULL, b INT NOT NULL,
                 PRIMARY KEY (a)) ENGINE={h.engine};
""")
    h.assert_err("composite FK rejected at CREATE",
                 f"""CREATE TABLE cc (
                       id INT NOT NULL PRIMARY KEY,
                       a INT NOT NULL, b INT NOT NULL,
                       FOREIGN KEY (a, b) REFERENCES pp (a, b)
                     ) ENGINE={h.engine}""",
                 r"composite FOREIGN KEY")
    h.assert_scalar("composite FK table not present",
                    "SELECT COUNT(*) FROM information_schema.tables "
                    f"WHERE table_schema = '{h.db}' AND table_name = 'cc'",
                    "0")

    h.section("COMPRESSED BLOB / TEXT refused")
    # Its Field::store writes the field's own buffer, which the row MariaDB
    # builds shares; a row read next to it (REPLACE's duplicate) would
    # overwrite that row's value. COMPRESSED VARCHAR stores in the record.
    h.assert_err("TEXT COMPRESSED rejected at CREATE",
                 f"CREATE TABLE tcomp (id INT NOT NULL PRIMARY KEY, c TEXT "
                 f"COMPRESSED) ENGINE={h.engine}",
                 r"COMPRESSED BLOB / TEXT column 'c'")
    h.assert_ok("VARCHAR COMPRESSED accepted",
                f"CREATE TABLE vcomp (id INT NOT NULL PRIMARY KEY, c "
                f"VARCHAR(100) COMPRESSED) ENGINE={h.engine}")

    h.section("Self-referencing FOREIGN KEY refused")
    h.assert_err("self-ref FK rejected at CREATE",
                 f"""CREATE TABLE selfref (
                       id INT NOT NULL PRIMARY KEY,
                       parent_id INT,
                       FOREIGN KEY (parent_id) REFERENCES selfref (id)
                     ) ENGINE={h.engine}""",
                 r"self-referencing FOREIGN KEY")
    h.assert_scalar("self-ref table not present",
                    "SELECT COUNT(*) FROM information_schema.tables "
                    f"WHERE table_schema = '{h.db}' AND table_name = 'selfref'",
                    "0")

    h.section("Direct UPDATE: UNIQUE violation -> ER_DUP_ENTRY (1062)")
    h.exec_script(f"""
CREATE TABLE du (id INT NOT NULL PRIMARY KEY,
                 u INT NOT NULL,
                 UNIQUE (u)) ENGINE={h.engine};
INSERT INTO du VALUES (1, 10), (2, 20);
""")
    # The shape (single-table UPDATE on a binary column with no SP / no
    # SELECT subquery) is the direct-DML pushdown path. Stoolap raises
    # "unique constraint failed ... on column u with value 10". Pre-fix
    # this surfaced as 1296 (ER_GET_ERRMSG); now it must surface as
    # 1062 (ER_DUP_ENTRY) so the SQLSTATE 23000 / ON DUPLICATE KEY
    # routing works.
    _assert_errno(h, "UNIQUE violation surfaces as 1062",
                  "UPDATE du SET u = 10 WHERE id = 2", 1062)
    h.assert_scalar("UPDATE rolled back: row 2 still 20",
                    "SELECT u FROM du WHERE id = 2", "20")

    h.section("Direct UPDATE: FK violation -> ER_NO_REFERENCED_ROW_2 (1452)")
    h.exec_script(f"""
CREATE TABLE pfk (id INT NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO pfk VALUES (1), (2);
CREATE TABLE cfk (id INT NOT NULL PRIMARY KEY,
                  pid INT NOT NULL,
                  FOREIGN KEY (pid) REFERENCES pfk (id)) ENGINE={h.engine};
INSERT INTO cfk VALUES (10, 1);
""")
    _assert_errno(h, "FK violation surfaces as 1452",
                  "UPDATE cfk SET pid = 999 WHERE id = 10", 1452)
    h.assert_scalar("UPDATE rolled back: pid still 1",
                    "SELECT pid FROM cfk WHERE id = 10", "1")


def _assert_errno(h, label, sql, expected_errno):
    """Run sql and assert the resulting MySQL/MariaDB errno equals
    expected_errno exactly. Tighter than assert_err's regex match,
    which lets the test pass on the wrong error code as long as some
    text matches. Used for the direct-DML constraint-mapping tests
    where we care that the user gets 1062 / 1452 specifically and
    not 1296 (ER_GET_ERRMSG)."""
    import mysql.connector
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
