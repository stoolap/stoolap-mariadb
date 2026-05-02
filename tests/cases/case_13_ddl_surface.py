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
