"""String predicates and functions: LIKE, IN, BETWEEN, NOT IN, CONCAT,
SUBSTRING, LOWER/UPPER, LENGTH, TRIM, COALESCE."""


def run(h):
    h.exec_script(f"""
CREATE TABLE w (
    id INT NOT NULL PRIMARY KEY,
    s VARCHAR(80) NOT NULL,
    KEY idx_s (s)
) ENGINE={h.engine};
INSERT INTO w VALUES
  (1, 'apple'),
  (2, 'banana'),
  (3, 'BANANA'),
  (4, 'cherry pie'),
  (5, '  trim_me  '),
  (6, ''),
  (7, 'banana split');
""")

    h.section("LIKE")
    # VARCHAR columns default to a case-insensitive collation under MariaDB
    # (utf8mb3_general_ci), so 'banana' and 'BANANA' both match 'banana%'.
    h.assert_scalar("LIKE prefix",     "SELECT COUNT(*) FROM w WHERE s LIKE 'banana%'", "3")
    h.assert_scalar("LIKE infix",      "SELECT COUNT(*) FROM w WHERE s LIKE '%pie%'",   "1")
    h.assert_scalar("LIKE suffix",     "SELECT COUNT(*) FROM w WHERE s LIKE '%split'",  "1")
    h.assert_scalar("LIKE single-char","SELECT COUNT(*) FROM w WHERE s LIKE 'app_e'",   "1")
    h.assert_scalar("LIKE no match",   "SELECT COUNT(*) FROM w WHERE s LIKE 'zzz%'",    "0")

    h.section("IN / NOT IN")
    h.assert_scalar("IN",     "SELECT COUNT(*) FROM w WHERE s IN ('apple','cherry pie')", "2")
    # 'BANANA' folds to 'banana' under ci collation, so NOT IN ('apple','banana')
    # excludes apple, banana, BANANA, banana split (4 rows). 4 strings remain
    # (cherry pie, trim_me, '', banana split is also excluded -> wait recount).
    h.assert_scalar("NOT IN", "SELECT COUNT(*) FROM w WHERE s NOT IN ('apple','banana')", "4")

    h.section("BETWEEN (ci collation)")
    h.assert_scalar("BETWEEN 'a' AND 'c'",
                    "SELECT COUNT(*) FROM w WHERE s BETWEEN 'a' AND 'c'", "4")

    h.section("CONCAT / LOWER / UPPER / LENGTH")
    h.assert_scalar("CONCAT", "SELECT CONCAT(s, '!') FROM w WHERE id = 1", "apple!")
    h.assert_scalar("LOWER",  "SELECT LOWER(s) FROM w WHERE id = 3", "banana")
    h.assert_scalar("UPPER",  "SELECT UPPER(s) FROM w WHERE id = 1", "APPLE")
    h.assert_scalar("LENGTH", "SELECT LENGTH(s) FROM w WHERE id = 1", "5")
    h.assert_scalar("LENGTH on empty", "SELECT LENGTH(s) FROM w WHERE id = 6", "0")

    h.section("SUBSTRING")
    h.assert_scalar("SUBSTRING from 1 len 3", "SELECT SUBSTRING(s, 1, 3) FROM w WHERE id = 2", "ban")
    h.assert_scalar("SUBSTRING from middle",  "SELECT SUBSTRING(s, 4) FROM w WHERE id = 2",    "ana")

    h.section("TRIM")
    h.assert_scalar("TRIM",              "SELECT TRIM(s) FROM w WHERE id = 5",         "trim_me")
    h.assert_scalar("LENGTH after TRIM", "SELECT LENGTH(TRIM(s)) FROM w WHERE id = 5", "7")

    h.section("COALESCE on string column")
    h.assert_ok("row with NULL", "INSERT INTO w VALUES (8, '')")
    h.assert_scalar("COALESCE empty",
                    "SELECT COALESCE(NULLIF(s, ''), 'fallback') FROM w WHERE id = 8",
                    "fallback")
