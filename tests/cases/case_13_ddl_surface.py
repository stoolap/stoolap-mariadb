"""DDL surface: column types, DEFAULT expressions, NOT NULL enforcement,
RENAME, CREATE TABLE LIKE, SHOW CREATE TABLE."""


def run(h):
    h.section("Column types")
    h.exec_script(f"""
CREATE TABLE types (
    id INT NOT NULL PRIMARY KEY,
    ti TINYINT,
    si SMALLINT,
    ii INT,
    bi BIGINT,
    f FLOAT,
    d DOUBLE,
    dec_v DECIMAL(10,2),
    s VARCHAR(40),
    b BOOLEAN,
    t TEXT,
    when_at DATETIME(6)
) ENGINE={h.engine};
INSERT INTO types VALUES
  (1, 1, 100, 1000, 1000000, 1.5, 1.5, 1.50, 'one', TRUE, 'long text here', '2026-01-01 00:00:00.123456');
""")
    h.assert_scalar("TINYINT",      "SELECT ti FROM types WHERE id = 1", "1")
    h.assert_scalar("SMALLINT",     "SELECT si FROM types WHERE id = 1", "100")
    h.assert_scalar("BIGINT",       "SELECT bi FROM types WHERE id = 1", "1000000")
    h.assert_scalar("DECIMAL",      "SELECT dec_v FROM types WHERE id = 1", "1.50")
    h.assert_scalar("BOOLEAN",      "SELECT b FROM types WHERE id = 1", "1")
    h.assert_scalar("TEXT",         "SELECT t FROM types WHERE id = 1", "long text here")
    h.assert_scalar("DATETIME(6)",  "SELECT when_at FROM types WHERE id = 1",
                    "2026-01-01 00:00:00.123456")

    h.section("DEFAULT expressions")
    h.exec_script(f"""
CREATE TABLE defs (
    id INT NOT NULL PRIMARY KEY,
    s VARCHAR(40) DEFAULT 'auto',
    n INT DEFAULT 42,
    flag BOOLEAN DEFAULT FALSE
) ENGINE={h.engine};
INSERT INTO defs (id) VALUES (1);
INSERT INTO defs VALUES (2, 'manual', 7, TRUE);
""")
    h.assert_scalar("string DEFAULT", "SELECT s FROM defs WHERE id = 1",     "auto")
    h.assert_scalar("int DEFAULT",    "SELECT n FROM defs WHERE id = 1",     "42")
    h.assert_scalar("bool DEFAULT",   "SELECT flag FROM defs WHERE id = 1",  "0")
    h.assert_scalar("manual override", "SELECT s FROM defs WHERE id = 2",    "manual")

    h.section("NOT NULL enforcement")
    h.exec_script(f"""
CREATE TABLE nn (
    id INT NOT NULL PRIMARY KEY,
    must VARCHAR(20) NOT NULL
) ENGINE={h.engine};
""")
    h.assert_err("NOT NULL violated",
                 "INSERT INTO nn VALUES (1, NULL)",
                 r"Got error|cannot be null|NULL|ERROR")
    h.assert_ok("NOT NULL satisfied", "INSERT INTO nn VALUES (1, 'set')")

    h.section("CREATE TABLE LIKE")
    h.exec_script("CREATE TABLE clone_of_t LIKE types;")
    h.assert_ok("insert into clone", "INSERT INTO clone_of_t (id) VALUES (99)")
    h.assert_scalar("clone count",   "SELECT COUNT(*) FROM clone_of_t", "1")

    h.section("RENAME TABLE")
    h.assert_ok("rename",        "RENAME TABLE clone_of_t TO clone_renamed")
    h.assert_scalar("renamed read", "SELECT COUNT(*) FROM clone_renamed", "1")
    h.assert_err("old name gone", "SELECT * FROM clone_of_t",
                 r"Unknown|doesn't exist|not exist|no such|ERROR")

    h.section("SHOW CREATE TABLE")
    out = h.sql("SHOW CREATE TABLE defs")
    if "ENGINE=STOOLAP" in out.upper().replace(" ", ""):
        h._pass("SHOW CREATE TABLE includes ENGINE=STOOLAP")
    else:
        h._fail("SHOW CREATE TABLE missing engine", f"out: {out}")

    h.section("Database/table name underscores don't alias")
    # The naive `<db>__<tbl>` flat name would alias `p__q` / `r` and
    # `p` / `q__r`. Verify both legal (db, tbl) pairs round-trip to
    # distinct stoolap-side tables. Encoding escapes `_` to `_0` and
    # uses `_1` between db and tbl, so:
    #   p__q.r  -> p_0_0q_1r
    #   p.q__r  -> p_1q_0_0r
    h.exec_script("CREATE DATABASE IF NOT EXISTS p__q")
    h.exec_script("CREATE DATABASE IF NOT EXISTS p")
    h.exec_script(f"CREATE TABLE p__q.r (id INT PRIMARY KEY, v INT) ENGINE={h.engine}")
    h.assert_ok("colliding-pair second CREATE accepted",
                f"CREATE TABLE p.q__r (id INT PRIMARY KEY, v INT) ENGINE={h.engine}")
    h.exec_stmt("INSERT INTO p__q.r VALUES (1, 100)")
    h.exec_stmt("INSERT INTO p.q__r  VALUES (1, 200)")
    h.assert_scalar("p__q.r kept its own row",
                    "SELECT v FROM p__q.r WHERE id = 1", "100")
    h.assert_scalar("p.q__r kept its own row",
                    "SELECT v FROM p.q__r WHERE id = 1", "200")
    h.exec_script("DROP TABLE p__q.r")
    h.exec_script("DROP TABLE p.q__r")
    h.exec_script("DROP DATABASE p__q")
    h.exec_script("DROP DATABASE p")

    h.section("Names past stoolap's 255 bytes are shortened")
    # 64 underscores each escape to 128 bytes, 258 with the separator:
    # past the limit stoolap puts on a table name, so the plugin keeps
    # the first bytes and a hash. Tables one underscore apart stay apart,
    # and every path (DDL, reads, pushdown, FK parent, rename) agrees.
    u = "_" * 64
    h.exec_script(f"CREATE DATABASE IF NOT EXISTS `{u}`")
    h.assert_ok("CREATE with 64-byte db and table names",
                f"CREATE TABLE `{u}`.`{u}` (id INT PRIMARY KEY, v INT) "
                f"ENGINE={h.engine}")
    h.assert_ok("CREATE one underscore shorter",
                f"CREATE TABLE `{u}`.`{u[1:]}` (id INT PRIMARY KEY, v INT) "
                f"ENGINE={h.engine}")
    h.assert_ok("CREATE a child of the long parent",
                f"CREATE TABLE `{u}`.c (id INT PRIMARY KEY, pid INT, "
                f"FOREIGN KEY (pid) REFERENCES `{u}` (id)) ENGINE={h.engine}")
    # stoolap matches table names lowercased: a parent named in other
    # letter case, one name shortened, is still found
    pa = "_" * 63 + "a"
    h.exec_script(f"CREATE TABLE `{u}`.`{pa}` (id INT PRIMARY KEY) "
                  f"ENGINE={h.engine}")
    h.assert_ok("CREATE a child naming the long parent in upper case",
                f"CREATE TABLE `{u}`.c2 (id INT PRIMARY KEY, pid INT, "
                f"FOREIGN KEY (pid) REFERENCES `{pa.upper()}` (id)) "
                f"ENGINE={h.engine}")
    h.exec_stmt(f"INSERT INTO `{u}`.`{pa}` VALUES (1)")
    h.assert_ok("child row of the parent named in upper case",
                f"INSERT INTO `{u}`.c2 VALUES (1, 1)")
    h.exec_stmt(f"INSERT INTO `{u}`.`{u}` VALUES (1, 100)")
    h.exec_stmt(f"INSERT INTO `{u}`.`{u[1:]}` VALUES (1, 200)")
    h.assert_ok("child row of an existing parent",
                f"INSERT INTO `{u}`.c VALUES (1, 1)")
    h.assert_err("child row of a missing parent",
                 f"INSERT INTO `{u}`.c VALUES (2, 9)", "foreign key|1452")
    h.assert_scalar("long table kept its row",
                    f"SELECT v FROM `{u}`.`{u}` WHERE id = 1", "100")
    h.assert_scalar("shorter table kept its row",
                    f"SELECT v FROM `{u}`.`{u[1:]}` WHERE id = 1", "200")
    h.assert_pushed("pushed join of both",
                    f"SELECT a.v + b.v FROM `{u}`.`{u}` a "
                    f"JOIN `{u}`.`{u[1:]}` b ON a.id = b.id")
    h.assert_scalar("pushed join reads both",
                    f"SELECT a.v + b.v FROM `{u}`.`{u}` a "
                    f"JOIN `{u}`.`{u[1:]}` b ON a.id = b.id", "300")
    h.assert_ok("direct UPDATE", f"UPDATE `{u}`.`{u}` SET v = v + 1")
    h.assert_scalar("updated", f"SELECT v FROM `{u}`.`{u}`", "101")
    h.assert_ok("RENAME to another long name",
                f"RENAME TABLE `{u}`.`{u[1:]}` TO `{u}`.`{u[2:]}x`")
    h.assert_scalar("renamed read",
                    f"SELECT v FROM `{u}`.`{u[2:]}x` WHERE id = 1", "200")
    h.exec_script(f"DROP DATABASE `{u}`")
