"""Numeric/math functions and operators: ABS, ROUND, FLOOR, CEILING/CEIL,
MOD, POWER, integer vs double promotion, division-by-zero handling."""


def run(h):
    h.exec_script(f"""
CREATE TABLE n (
    id INT NOT NULL PRIMARY KEY,
    i INT NOT NULL,
    d DOUBLE NOT NULL
) ENGINE={h.engine};
INSERT INTO n VALUES
  (1, -3, -3.7),
  (2,  0,  0.0),
  (3,  4,  4.49),
  (4,  7,  7.5),
  (5, 10, 10.999);
""")

    h.section("ABS")
    h.assert_scalar("ABS positive", "SELECT ABS(i) FROM n WHERE id = 1", "3")
    h.assert_scalar("ABS double",   "SELECT ABS(d) FROM n WHERE id = 1", "3.7")
    h.assert_scalar("ABS zero",     "SELECT ABS(d) FROM n WHERE id = 2", "0")

    h.section("ROUND / FLOOR / CEILING")
    h.assert_scalar("ROUND down",  "SELECT ROUND(d) FROM n WHERE id = 3", "4")
    h.assert_scalar("ROUND .5 up", "SELECT ROUND(d) FROM n WHERE id = 4", "8")
    h.assert_scalar("FLOOR",       "SELECT FLOOR(d) FROM n WHERE id = 5", "10")
    h.assert_scalar("CEILING",     "SELECT CEILING(d) FROM n WHERE id = 5", "11")

    h.section("MOD")
    h.assert_scalar("MOD operator", "SELECT i MOD 3 FROM n WHERE id = 5", "1")
    h.assert_scalar("MOD function", "SELECT MOD(i, 3) FROM n WHERE id = 5", "1")
    h.assert_scalar("MOD 0 (returns NULL)",
                    "SELECT IFNULL(MOD(i, 0), 'NIL') FROM n WHERE id = 5", "NIL")

    h.section("Arithmetic + type promotion")
    h.assert_scalar("int + int -> int", "SELECT i + 1 FROM n WHERE id = 5", "11")
    h.assert_scalar("int + double",     "SELECT i + d FROM n WHERE id = 1", "-6.7")
    h.assert_scalar("negate",           "SELECT -i FROM n WHERE id = 5", "-10")
    h.assert_scalar("multiplication",   "SELECT i * 100 FROM n WHERE id = 3", "400")

    h.section("Comparisons")
    h.assert_scalar("double > int",        "SELECT COUNT(*) FROM n WHERE d > 4", "3")
    h.assert_scalar("negative comparison", "SELECT COUNT(*) FROM n WHERE i < 0", "1")
    h.assert_scalar("equal across types",  "SELECT COUNT(*) FROM n WHERE i = 4.0", "1")

    h.section("Division-by-zero (NULL semantics)")
    h.assert_scalar("i / 0 is NULL",
                    "SELECT IFNULL(i / 0, 'NIL') FROM n WHERE id = 5", "NIL")

    h.section("Unsigned integer round-trip")
    h.exec_script(f"""
CREATE TABLE us (
    id INT NOT NULL PRIMARY KEY,
    t1 TINYINT UNSIGNED,
    s1 SMALLINT UNSIGNED,
    m1 MEDIUMINT UNSIGNED,
    i1 INT UNSIGNED,
    b1 BIGINT UNSIGNED
) ENGINE={h.engine};
INSERT INTO us VALUES
  (1, 255, 65535, 16777215, 4294967295, 9223372036854775807);
""")
    h.assert_scalar("TINYINT UNSIGNED max",   "SELECT t1 FROM us WHERE id=1", "255")
    h.assert_scalar("SMALLINT UNSIGNED max",  "SELECT s1 FROM us WHERE id=1", "65535")
    h.assert_scalar("MEDIUMINT UNSIGNED max", "SELECT m1 FROM us WHERE id=1", "16777215")
    h.assert_scalar("INT UNSIGNED max",       "SELECT i1 FROM us WHERE id=1", "4294967295")
    h.assert_scalar("BIGINT UNSIGNED at INT64_MAX",
                    "SELECT b1 FROM us WHERE id=1", "9223372036854775807")

    h.exec_script(f"""
CREATE TABLE ur (id INT UNSIGNED NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO ur VALUES (1), (3000000000), (4000000000);
""")
    h.assert_scalar("INT UNSIGNED rangecount",
                    "SELECT COUNT(*) FROM ur WHERE id > 2000000000", "2")
    h.assert_scalar("INT UNSIGNED max stored",
                    "SELECT MAX(id) FROM ur", "4000000000")
    h.assert_scalar("INT UNSIGNED scalar above INT32_MAX",
                    "SELECT id FROM ur WHERE id = 4000000000", "4000000000")

    h.assert_err("BIGINT UNSIGNED INT64_MAX+1 rejected",
                 "INSERT INTO us VALUES (2,0,0,0,0,9223372036854775808)",
                 r"extension that doesn't exist")
    h.assert_err("BIGINT UNSIGNED UINT64_MAX rejected",
                 "INSERT INTO us VALUES (3,0,0,0,0,18446744073709551615)",
                 r"extension that doesn't exist")
    h.assert_scalar("rejected rows did not land", "SELECT COUNT(*) FROM us", "1")

    h.exec_script(f"""
CREATE TABLE us_stream (id INT UNSIGNED NOT NULL PRIMARY KEY) ENGINE={h.engine};
INSERT INTO us_stream VALUES (3000000000), (4000000000), (1);
""")
    out = h.sql("SELECT id FROM us_stream ORDER BY id LIMIT 5")
    out_csv = ",".join(out.split("\n"))
    h.assert_eq("streaming INT UNSIGNED above INT32_MAX",
                "1,3000000000,4000000000", out_csv)
