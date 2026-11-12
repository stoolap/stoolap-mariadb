"""A UNIQUE key over a column of a collation that does not compare its
bytes (a `_ci` one) holds as MariaDB's collation says: stoolap keeps the
key on a column of each value's collation key (its weights, as
WEIGHT_STRING gives them), equal for the values the collation holds
equal. Inserts, updates, the row pump and the SQL write path compute it;
an exact lookup by the key reads it.
"""

import random

import mysql.connector

DUP = 1062


def _errno(h, sql):
    """The errno `sql` fails with, or 0"""
    try:
        h.cur.execute(sql)
        try:
            h.cur.fetchall()
        except mysql.connector.errors.InterfaceError:
            pass
        return 0
    except mysql.connector.Error as e:
        return e.errno


def _assert_dup(h, label, sql, key):
    try:
        h.cur.execute(sql)
        h._fail(label, f"sql:    {sql}", "server: <no error>")
    except mysql.connector.Error as e:
        if e.errno == DUP and f"'{key}'" in str(e):
            h._pass(label)
        else:
            h._fail(label, f"sql:    {sql}", f"server: {e}")


def run(h):
    h.section("A _ci UNIQUE key refuses the values its collation holds equal")
    h.exec_script(f"""
CREATE TABLE cu (id INT NOT NULL PRIMARY KEY, email VARCHAR(100) UNIQUE,
  n INT) ENGINE={h.engine};
INSERT INTO cu VALUES (1, 'Ali@x.com', 0);
""")
    for i, v in enumerate(["ali@x.com", "ALI@X.COM", "Ali@x.com  ",
                           "Àli@x.com"]):
        _assert_dup(h, f"{v!r} is a duplicate",
                    f"INSERT INTO cu VALUES ({10 + i}, '{v}', 0)", "email")
    h.assert_ok("a different value", "INSERT INTO cu VALUES (2, 'Ali@x.co', 0)")
    h.assert_ok("NULLs never collide",
                "INSERT INTO cu VALUES (3, NULL, 0), (4, NULL, 0)")
    _assert_dup(h, "a duplicate within one multi-row INSERT",
                "INSERT INTO cu VALUES (5, 'zed', 0), (6, 'ZED', 0)", "email")
    h.assert_scalar("the statement left no row", "SELECT COUNT(*) FROM cu "
                    "WHERE id IN (5, 6)", "0")
    h.assert_ok("INSERT IGNORE skips the duplicate",
                "INSERT IGNORE INTO cu VALUES (5, 'zed', 0), (6, 'ZED', 0)")
    h.assert_scalar("and keeps the first", "SELECT GROUP_CONCAT(id) FROM cu "
                    "WHERE id IN (5, 6)", "5")
    h.assert_ok("ON DUPLICATE KEY UPDATE finds the row by the key",
                "INSERT INTO cu VALUES (7, 'ALI@x.com', 0) "
                "ON DUPLICATE KEY UPDATE n = n + 1")
    h.assert_scalar("the row it updated", "SELECT n FROM cu WHERE id = 1", "1")
    h.assert_ok("REPLACE replaces the row the key names",
                "REPLACE INTO cu VALUES (8, 'ali@X.com', 9)")
    h.assert_scalar("the row replaced", "SELECT GROUP_CONCAT(id) FROM cu "
                    "WHERE id IN (1, 8)", "8")

    h.section("Updates")
    _assert_dup(h, "an UPDATE into another row's value",
                "UPDATE cu SET email = 'ALI@X.COM' WHERE id = 2", "email")
    h.assert_ok("an UPDATE to its own value in another case",
                "UPDATE cu SET email = 'ALI@X.CO' WHERE id = 2")
    h.assert_ok("an UPDATE computed from the column (the row pump)",
                "UPDATE cu SET email = LOWER(email) WHERE id = 2")
    h.assert_scalar("updated", "SELECT email FROM cu WHERE id = 2", "ali@x.co")
    h.assert_ok("an UPDATE of another column", "UPDATE cu SET n = n + 1")
    h.assert_ok("the key moves with an UPDATE",
                "UPDATE cu SET email = 'moved' WHERE id = 2")
    h.assert_ok("so its old value is free", "INSERT INTO cu VALUES "
                "(9, 'ALI@X.CO', 0)")
    _assert_dup(h, "and its new one taken",
                "INSERT INTO cu VALUES (11, 'MOVED', 0)", "email")

    h.section("Lookups by the key")
    for v in ["ali@x.com", "ALI@X.COM", "ali@x.com  ", "àli@x.com"]:
        h.assert_scalar(f"WHERE email = {v!r}",
                        f"SELECT id FROM cu WHERE email = '{v}'", "8")
    h.assert_scalar("WHERE email IS NULL",
                    "SELECT GROUP_CONCAT(id ORDER BY id) FROM cu "
                    "WHERE email IS NULL", "3,4")
    h.assert_scalar("a missing value", "SELECT COUNT(*) FROM cu "
                    "WHERE email = 'nobody'", "0")

    h.section("Transactions")
    h.exec_stmt("BEGIN")
    h.assert_ok("a row in a transaction", "INSERT INTO cu VALUES (20, 'tx', 0)")
    _assert_dup(h, "its duplicate in the same transaction",
                "INSERT INTO cu VALUES (21, 'TX', 0)", "email")
    h.exec_stmt("ROLLBACK")
    h.assert_ok("the rolled back value is free",
                "INSERT INTO cu VALUES (21, 'TX', 0)")

    h.section("Character types and collations")
    h.exec_stmt(f"""
CREATE TABLE ck (id INT NOT NULL PRIMARY KEY, c CHAR(10) UNIQUE,
  l VARCHAR(10) CHARACTER SET latin1 UNIQUE,
  u VARCHAR(10) COLLATE utf8mb4_uca1400_ai_ci UNIQUE,
  np VARCHAR(10) COLLATE utf8mb4_unicode_nopad_ci UNIQUE) ENGINE={h.engine}""")
    h.exec_stmt("INSERT INTO ck VALUES (1, 'ab', 'Ärger', 'Straße', 'a')")
    _assert_dup(h, "CHAR", "INSERT INTO ck (id, c) VALUES (2, 'AB')", "c")
    _assert_dup(h, "latin1_swedish_ci",
                "INSERT INTO ck (id, l) VALUES (3, 'ärger')", "l")
    _assert_dup(h, "uca1400_ai_ci expands ß",
                "INSERT INTO ck (id, u) VALUES (4, 'STRASSE')", "u")
    h.assert_ok("NO PAD keeps a trailing space",
                "INSERT INTO ck (id, np) VALUES (5, 'a ')")
    _assert_dup(h, "NO PAD still ignores case",
                "INSERT INTO ck (id, np) VALUES (6, 'A')", "np")

    h.section("Composite and added keys")
    h.exec_stmt(f"CREATE TABLE cc (id INT NOT NULL PRIMARY KEY, a VARCHAR(20), "
                f"b INT, UNIQUE KEY ab (a, b)) ENGINE={h.engine}")
    h.assert_ok("composite rows", "INSERT INTO cc VALUES (1, 'x', 1), (2, 'X', 2)")
    _assert_dup(h, "a composite duplicate",
                "INSERT INTO cc VALUES (3, 'X', 1)", "ab")
    h.exec_script(f"""
CREATE TABLE ca (id INT NOT NULL PRIMARY KEY, s VARCHAR(10)) ENGINE={h.engine};
INSERT INTO ca VALUES (1, 'q'), (2, 'Q');
""")
    _assert_dup(h, "ADD UNIQUE over duplicates",
                "ALTER TABLE ca ADD UNIQUE KEY s (s)", "s")
    h.exec_stmt("DELETE FROM ca WHERE id = 2")
    h.assert_ok("ADD UNIQUE", "ALTER TABLE ca ADD UNIQUE KEY s (s)")
    _assert_dup(h, "the added key", "INSERT INTO ca VALUES (2, 'Q')", "s")
    h.assert_ok("a copying ALTER of the table",
                "ALTER TABLE ca ADD COLUMN z INT FIRST, ALGORITHM=COPY")
    _assert_dup(h, "keeps the key", "INSERT INTO ca VALUES (0, 3, 'Q')", "s")
    h.assert_err("a column change in place is refused (the key columns "
                 "follow the table's)",
                 "ALTER TABLE ca ADD COLUMN w INT, ALGORITHM=INPLACE",
                 r"\b1845\b")
    h.assert_ok("and copies without ALGORITHM", "ALTER TABLE ca ADD COLUMN w INT")
    h.assert_ok("an index added in place",
                "ALTER TABLE ca ADD INDEX kz (z), ALGORITHM=INPLACE")
    _assert_dup(h, "keeps the key too", "INSERT INTO ca VALUES (0, 3, 'Q', 0)",
                "s")
    h.assert_ok("DROP the key", "ALTER TABLE ca DROP INDEX s")
    h.assert_ok("the column takes a repeat", "INSERT INTO ca VALUES (0, 3, 'Q', 0)")

    h.section("A table a FOREIGN KEY names, written through SQL")
    h.exec_script(f"""
CREATE TABLE cp (id INT NOT NULL PRIMARY KEY, code VARCHAR(10) UNIQUE)
  ENGINE={h.engine};
CREATE TABLE cch (id INT NOT NULL PRIMARY KEY, pid INT,
  FOREIGN KEY (pid) REFERENCES cp (id)) ENGINE={h.engine};
INSERT INTO cp VALUES (1, 'a');
""")
    _assert_dup(h, "an INSERT", "INSERT INTO cp VALUES (2, 'A')", "code")
    h.assert_ok("an UPDATE moves the key",
                "UPDATE cp SET code = 'b' WHERE id = 1")
    h.assert_ok("freeing the old value", "INSERT INTO cp VALUES (2, 'A')")
    _assert_dup(h, "an UPDATE into a taken value",
                "UPDATE cp SET code = 'B' WHERE id = 2", "code")

    h.section("Accepted and refused as InnoDB does, row by row")
    random.seed(24)
    alphabet = (list("aAbBeE éÉßıiIİåÅ") +
                ["ss", "SS", "ae", "æ", "　", "́", "K",
                 "k", "σ", "ς", "Σ", "\U0001F600"])
    values = ["".join(random.choice(alphabet)
                      for _ in range(random.randint(0, 4)))
              for _ in range(300)]
    collations = ["utf8mb4_general_ci", "utf8mb4_unicode_ci",
                  "utf8mb4_uca1400_ai_ci", "utf8mb4_uca1400_as_ci",
                  "utf8mb4_unicode_520_nopad_ci", "utf8mb4_turkish_ci",
                  "utf8mb4_german2_ci", "utf8mb4_0900_ai_ci"]
    for coll in collations:
        tables = {}
        for engine in (h.engine, "InnoDB"):
            t = f"cd_{engine.lower()}_{coll}"
            h.exec_stmt(f"CREATE TABLE {t} (id INT NOT NULL PRIMARY KEY, "
                        f"v VARCHAR(10) COLLATE {coll} UNIQUE) ENGINE={engine}")
            tables[engine] = t
        diffs = []
        for i, v in enumerate(values):
            lit = v.replace("\\", "\\\\").replace("'", "''")
            got = {e: _errno(h, f"INSERT INTO {t} VALUES ({i}, '{lit}')")
                   for e, t in tables.items()}
            if got[h.engine] != got["InnoDB"]:
                diffs.append((v, got))
        if diffs:
            h._fail(f"{coll}: {len(diffs)} rows differ from InnoDB",
                    *[f"{v!r}: {g}" for v, g in diffs[:5]])
        else:
            h._pass(f"{coll}: every row as in InnoDB")
