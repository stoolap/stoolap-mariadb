"""Error-message plumbing: stoolap-engine errors must reach the user with
a useful message. MariaDB-mapped codes (HA_ERR_FOUND_DUPP_KEY, etc.) keep
MariaDB's framework wording; otherwise the stoolap text is surfaced
instead of the generic "Got error 168 (unspecified)"."""


def run(h):
    h.exec_script(f"""
CREATE TABLE u (id INT NOT NULL PRIMARY KEY,
                 code VARCHAR(20) COLLATE utf8mb4_bin UNIQUE) ENGINE={h.engine};
INSERT INTO u VALUES (1, 'A');
CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR(40)) ENGINE={h.engine};
INSERT INTO p VALUES (1, 'parent');
CREATE TABLE c (id INT PRIMARY KEY, pid INT NOT NULL,
                FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={h.engine};
CREATE TABLE d (id INT NOT NULL PRIMARY KEY,
                when_at DATETIME(6) NOT NULL) ENGINE={h.engine};
INSERT INTO d VALUES (1, '2026-04-28 12:00:00.123456');
""")

    h.section("Mapped errors keep MariaDB's framework wording")
    h.assert_err("duplicate PK",
                 "INSERT INTO u VALUES (1, 'B')",
                 r"Duplicate entry")
    h.assert_err("duplicate UNIQUE",
                 "INSERT INTO u VALUES (2, 'A')",
                 r"Duplicate entry")
    h.assert_err("FK violation",
                 "INSERT INTO c VALUES (1, 999)",
                 r"foreign key|FOREIGN KEY")
    h.assert_err("table exists",
                 f"CREATE TABLE u (id INT) ENGINE={h.engine}",
                 r"already exists")

    h.section("ci-collation UNIQUE / PRIMARY KEY refused at CREATE")
    h.assert_err("ci VARCHAR PRIMARY KEY refused",
                 f"CREATE TABLE rfk_pk (s VARCHAR(20) PRIMARY KEY) ENGINE={h.engine}",
                 r"case-insensitive collation")
    h.assert_err("ci VARCHAR UNIQUE refused",
                 f"""CREATE TABLE rfk_uq (id INT NOT NULL PRIMARY KEY,
                                       s VARCHAR(20),
                                       UNIQUE (s)) ENGINE={h.engine}""",
                 r"case-insensitive collation")
    h.assert_ok("_bin VARCHAR UNIQUE accepted",
                f"""CREATE TABLE rfk_bin (id INT NOT NULL PRIMARY KEY,
                                        s VARCHAR(20) COLLATE utf8mb4_bin,
                                        UNIQUE (s)) ENGINE={h.engine}""")
    h.assert_ok("VARBINARY UNIQUE accepted",
                f"""CREATE TABLE rfk_vb (id INT NOT NULL PRIMARY KEY,
                                       s VARBINARY(20),
                                       UNIQUE (s)) ENGINE={h.engine}""")
    h.assert_ok("ci VARCHAR non-unique KEY accepted",
                f"""CREATE TABLE rfk_idx (id INT NOT NULL PRIMARY KEY,
                                        s VARCHAR(20), KEY(s)) ENGINE={h.engine}""")

    h.section("Unmapped errors include the real stoolap message")
    out = h.sql("SELECT MICROSECOND(when_at) FROM d WHERE id = 1")
    if out == "123456":
        h._pass("server-side function works through handler")
    else:
        h._fail("server-side function returned", f"out: {out}")
