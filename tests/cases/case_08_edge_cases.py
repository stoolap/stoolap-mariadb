"""Edge cases: NULLs everywhere, empty strings, boundary numerics, dates
inside the supported range, and the BLOB/TEXT pipeline."""


def run(h):
    h.exec_script(f"""
CREATE TABLE n (
    id INT NOT NULL PRIMARY KEY,
    s VARCHAR(40),
    i INT,
    d DOUBLE,
    b BOOLEAN
) ENGINE={h.engine};
INSERT INTO n VALUES
  (1, NULL, NULL, NULL, NULL),
  (2, '', 0, 0.0, FALSE),
  (3, 'value', 42, 3.14, TRUE);
""")

    h.section("NULL handling")
    h.assert_scalar("IS NULL count",     "SELECT COUNT(*) FROM n WHERE s IS NULL",     "1")
    h.assert_scalar("IS NOT NULL count", "SELECT COUNT(*) FROM n WHERE s IS NOT NULL", "2")
    h.assert_scalar("IFNULL",            "SELECT IFNULL(s, 'fallback') FROM n WHERE id = 1", "fallback")
    h.assert_scalar("NULL + arithmetic", "SELECT IFNULL(i + 1, -1) FROM n WHERE id = 1", "-1")
    h.assert_scalar("NULL in agg ignored", "SELECT COUNT(i) FROM n", "2")
    h.assert_scalar("SUM ignores NULL",  "SELECT SUM(i) FROM n", "42")

    h.section("Empty string vs NULL")
    h.assert_scalar("empty string",      "SELECT LENGTH(s) FROM n WHERE id = 2", "0")
    h.assert_scalar("non-NULL is selectable", "SELECT COUNT(*) FROM n WHERE s = ''", "1")

    h.section("Numeric boundaries")
    h.exec_script(f"""
CREATE TABLE nn (
    id INT NOT NULL PRIMARY KEY,
    i32 INT,
    i64 BIGINT,
    f DOUBLE
) ENGINE={h.engine};
INSERT INTO nn VALUES
  (1, -2147483648, -9223372036854775807, -1.7976931348623157e308),
  (2,  2147483647,  9223372036854775807,  1.7976931348623157e308),
  (3, 0, 0, 0);
""")
    h.assert_scalar("int min",       "SELECT i32 FROM nn WHERE id = 1", "-2147483648")
    h.assert_scalar("int max",       "SELECT i32 FROM nn WHERE id = 2", "2147483647")
    h.assert_scalar("bigint min+1",  "SELECT i64 FROM nn WHERE id = 1", "-9223372036854775807")
    h.assert_scalar("bigint max",    "SELECT i64 FROM nn WHERE id = 2", "9223372036854775807")
    h.assert_scalar("negative double", "SELECT f < 0 FROM nn WHERE id = 1", "1")
    h.assert_scalar("positive double", "SELECT f > 0 FROM nn WHERE id = 2", "1")

    h.section("Dates inside supported range")
    # Stoolap's nanosecond timestamp format covers ~1678-2262.
    h.exec_script(f"""
CREATE TABLE d (
    id INT NOT NULL PRIMARY KEY,
    when_at DATETIME(6) NOT NULL,
    label VARCHAR(20)
) ENGINE={h.engine};
INSERT INTO d VALUES
  (1, '1970-01-01 00:00:00.000000', 'epoch'),
  (2, '2026-04-28 12:34:56.654321', 'today'),
  (3, '2200-12-31 23:59:59.999999', 'far');
""")
    h.assert_scalar("epoch round-trip",
                    "SELECT label FROM d WHERE when_at = '1970-01-01 00:00:00.000000'", "epoch")
    h.assert_scalar("microsecond preserved",
                    "SELECT label FROM d WHERE when_at = '2026-04-28 12:34:56.654321'", "today")
    h.assert_scalar("ORDER by date",
                    "SELECT label FROM d ORDER BY when_at LIMIT 1", "epoch")

    h.section("TEXT / BLOB")
    h.exec_script(f"""
CREATE TABLE big (
    id INT NOT NULL PRIMARY KEY,
    body TEXT
) ENGINE={h.engine};
INSERT INTO big VALUES (1, REPEAT('a', 1024));
""")
    h.assert_scalar("1KB text length", "SELECT LENGTH(body) FROM big", "1024")
    h.assert_scalar("TEXT prefix",     "SELECT LEFT(body, 5) FROM big", "aaaaa")

    h.section("rnd_pos hazard on BLOB/TEXT tables (regression)")
    # See 08_edge_cases.sh comment: rnd_pos must refuse on BLOB/TEXT tables
    # so ORDER BY + LIMIT UPDATE surfaces a clean error rather than silently
    # returning corrupt data when the value-store gets reused.
    h.exec_script(f"""
CREATE TABLE rp (id INT NOT NULL PRIMARY KEY, body TEXT, n INT) ENGINE={h.engine};
INSERT INTO rp VALUES (1,'a',10),(2,'b',20),(3,'c',30),(4,'d',5);
""")
    h.assert_ok("plain UPDATE on BLOB table", "UPDATE rp SET n = n + 100 WHERE id = 2")
    h.assert_scalar("plain UPDATE applied",   "SELECT n FROM rp WHERE id = 2", "120")
    h.assert_err("rnd_pos refuses on BLOB UPDATE ORDER BY LIMIT",
                 "UPDATE rp SET n = n + 1 ORDER BY n LIMIT 2",
                 r"rnd_pos / re-read by position is not supported on tables with BLOB/TEXT")

    h.exec_script(f"""
CREATE TABLE rp_plain (id INT NOT NULL PRIMARY KEY, n INT) ENGINE={h.engine};
INSERT INTO rp_plain VALUES (1,10),(2,20),(3,30),(4,5);
""")
    h.assert_ok("ORDER BY LIMIT UPDATE on plain table",
                "UPDATE rp_plain SET n = n + 1 ORDER BY n LIMIT 2")
    h.assert_scalar("smallest n bumped",    "SELECT n FROM rp_plain WHERE id = 4", "6")
    h.assert_scalar("next-smallest bumped", "SELECT n FROM rp_plain WHERE id = 1", "11")
    h.assert_scalar("untouched row id=2",   "SELECT n FROM rp_plain WHERE id = 2", "20")
    h.assert_scalar("untouched row id=3",   "SELECT n FROM rp_plain WHERE id = 3", "30")

    h.section("Default values")
    h.exec_script(f"""
CREATE TABLE def (
    id INT NOT NULL PRIMARY KEY,
    s VARCHAR(20) DEFAULT 'hello',
    i INT DEFAULT 7,
    flag BOOLEAN DEFAULT TRUE
) ENGINE={h.engine};
INSERT INTO def (id) VALUES (1);
INSERT INTO def VALUES (2, 'world', 14, FALSE);
""")
    h.assert_scalar("string default",   "SELECT s FROM def WHERE id = 1", "hello")
    h.assert_scalar("int default",      "SELECT i FROM def WHERE id = 1", "7")
    h.assert_scalar("bool default",     "SELECT flag FROM def WHERE id = 1", "1")
    h.assert_scalar("explicit override", "SELECT s FROM def WHERE id = 2", "world")
