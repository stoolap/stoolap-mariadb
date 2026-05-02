"""Index coverage: PRIMARY KEY, UNIQUE, secondary single/composite, range
scans, prefix indexes, ORDER BY iteration, ci-string ref folding,
reconcile re-creating secondary indexes."""


def run(h):
    h.exec_script(f"""
CREATE TABLE u (
    id INT NOT NULL PRIMARY KEY,
    email VARCHAR(80) COLLATE utf8mb4_bin NOT NULL,
    name VARCHAR(40) NOT NULL,
    age INT NOT NULL,
    UNIQUE KEY uq_email (email),
    KEY idx_age (age),
    KEY idx_name_age (name, age)
) ENGINE={h.engine};
INSERT INTO u VALUES
  (1, 'a@x', 'Alice', 30),
  (2, 'b@x', 'Bob',   25),
  (3, 'c@x', 'Carol', 30),
  (4, 'd@x', 'Dave',  40),
  (5, 'e@x', 'Eve',   25),
  (6, 'f@x', 'Alice', 35);
""")

    h.section("PRIMARY KEY")
    h.assert_scalar("PK lookup", "SELECT name FROM u WHERE id = 4", "Dave")
    h.assert_err("PK violation",
                 "INSERT INTO u VALUES (1, 'z@x', 'Z', 1)",
                 r"Duplicate|Got error|ERROR")

    h.section("UNIQUE KEY")
    h.assert_scalar("unique lookup", "SELECT id FROM u WHERE email = 'c@x'", "3")
    h.assert_err("unique violation",
                 "INSERT INTO u VALUES (99, 'a@x', 'X', 1)",
                 r"Duplicate|Got error|ERROR")

    h.section("Secondary KEY (range / iteration)")
    h.assert_scalar("exact age",         "SELECT COUNT(*) FROM u WHERE age = 25", "2")
    h.assert_scalar("range age >=",      "SELECT COUNT(*) FROM u WHERE age >= 30", "4")
    h.assert_scalar("range age between", "SELECT COUNT(*) FROM u WHERE age BETWEEN 25 AND 35", "5")
    h.assert_scalar("ORDER BY age ASC",  "SELECT id FROM u ORDER BY age ASC, id ASC LIMIT 1", "2")
    h.assert_scalar("ORDER BY age DESC", "SELECT id FROM u ORDER BY age DESC, id ASC LIMIT 1", "4")

    h.section("Composite KEY (name, age)")
    h.assert_scalar("name prefix",     "SELECT COUNT(*) FROM u WHERE name = 'Alice'", "2")
    h.assert_scalar("name + age",      "SELECT id FROM u WHERE name = 'Alice' AND age = 30", "1")
    h.assert_scalar("name range scan", "SELECT COUNT(*) FROM u WHERE name >= 'B' AND name < 'D'", "2")

    h.section("Composite range bounds use lexicographic SQL")
    # read_range_first used to emit per-part ANDs which broke tuple-lex
    # semantics; must now emit (a > X) OR (a = X AND b >= Y) etc.
    h.exec_script(f"""
CREATE TABLE crk (a INT NOT NULL, b INT NOT NULL, KEY ix_ab (a, b)) ENGINE={h.engine};
INSERT INTO crk VALUES (1,5),(1,6),(1,10),(2,2),(2,3),(2,9),(3,1);
""")
    out = h.sql(
        "SELECT a, b FROM crk FORCE INDEX (ix_ab) "
        "WHERE a >= 1 AND a <= 3 AND b BETWEEN 2 AND 9 ORDER BY a, b")
    out_csv = "|".join(",".join(r.split("\t")) for r in out.split("\n"))
    h.assert_eq("composite (a,b) range hits inner rows",
                "1,5|1,6|2,2|2,3|2,9", out_csv)

    out = h.sql(
        "SELECT a, b FROM crk FORCE INDEX (ix_ab) "
        "WHERE a >= 1 AND a <= 3 AND b BETWEEN 2 AND 9 ORDER BY a DESC, b DESC")
    out_csv = "|".join(",".join(r.split("\t")) for r in out.split("\n"))
    h.assert_eq("composite range DESC same row set",
                "2,9|2,3|2,2|1,6|1,5", out_csv)

    h.section("DELETE / UPDATE on indexed cols")
    h.assert_ok("update indexed col", "UPDATE u SET age = 31 WHERE id = 1")
    h.assert_scalar("post-update lookup", "SELECT age FROM u WHERE id = 1", "31")
    h.assert_scalar("old age excluded",   "SELECT COUNT(*) FROM u WHERE age = 30", "1")
    h.assert_ok("delete by indexed",      "DELETE FROM u WHERE age = 25")
    h.assert_scalar("post-delete count",  "SELECT COUNT(*) FROM u", "4")
    h.assert_scalar("deleted absent",     "SELECT COUNT(*) FROM u WHERE name = 'Bob'", "0")

    h.section("Ref access on ci-string indexed column ci-folds (ASCII)")
    # index_read_map for ref equality on a ci-collated VARCHAR wraps
    # both sides in LOWER() so stoolap's bytewise compare folds case
    # the same way utf8mb4_general_ci does. ASCII-only datasets get
    # the right answer this way.
    h.exec_script(f"""
CREATE TABLE refci (id INT NOT NULL PRIMARY KEY,
                     name VARCHAR(20) NOT NULL,
                     KEY ix_name (name)) ENGINE={h.engine};
INSERT INTO refci VALUES (1,'Bob'),(2,'bob'),(3,'BOb'),(4,'Alice');
""")
    h.assert_scalar("ci ref count",
                    "SELECT COUNT(*) FROM refci FORCE INDEX(ix_name) WHERE name = 'bob'", "3")
    h.assert_scalar("ci ref includes upper",
                    "SELECT id FROM refci FORCE INDEX(ix_name) "
                    "WHERE name = 'BOB' ORDER BY id LIMIT 1", "1")

    h.exec_script(f"""
CREATE TABLE refci_in (id INT NOT NULL PRIMARY KEY,
                       s VARCHAR(20) NOT NULL,
                       KEY ix_s (s)) ENGINE={h.engine};
INSERT INTO refci_in VALUES (1,'apple'),(2,'banana'),(3,'BANANA'),(4,'cherry pie');
""")
    h.assert_scalar("IN-via-ref ci-folded",
                    "SELECT COUNT(*) FROM refci_in FORCE INDEX(ix_s) "
                    "WHERE s IN ('APPLE','Banana')", "3")

    # _bin VARCHAR keeps byte-exact ref semantics.
    h.exec_script(f"""
CREATE TABLE refbin (id INT NOT NULL PRIMARY KEY,
                     s VARCHAR(20) COLLATE utf8mb4_bin NOT NULL,
                     KEY ix_s (s)) ENGINE={h.engine};
INSERT INTO refbin VALUES (1,'Bob'),(2,'bob'),(3,'BOb');
""")
    h.assert_scalar("_bin ref byte-exact",
                    "SELECT id FROM refbin FORCE INDEX(ix_s) WHERE s = 'bob'", "2")

    h.section("ci ref accent-fold via collation-correct lookup")
    # Stoolap compares strings byte-wise. MariaDB's default ci
    # collations (utf8mb4_general_ci, ...) case-fold AND accent-fold:
    # 'é' = 'e', 'ß' = 'ss'. To get MariaDB-correct counts on accented
    # data, ha_stoolap.cc::index_read_map drops the engine-side WHERE
    # for ci-leading-key ref access and ci-filters every fetched row
    # in our overridden index_next via the field's CHARSET_INFO
    # comparator (Field::cmp). That makes single-table SELECTs, JOIN
    # ON refs, and IN-list ref iterations all return the same matches
    # MariaDB's collation would on a byte-aware engine.
    h.exec_script(f"""
CREATE TABLE accent_ci (id INT NOT NULL PRIMARY KEY,
                        s VARCHAR(20) NOT NULL,
                        KEY ix_s (s)) ENGINE={h.engine};
INSERT INTO accent_ci VALUES (1,'e'),(2,'E'),(3,unhex('C3A9'));
""")
    # Single-table SELECT: ci ref returns all 3 ci-equivalent rows
    # (e, E, é).
    h.assert_scalar("ci ref accent-aware count",
                    "SELECT COUNT(*) FROM accent_ci WHERE s = 'e'", "3")

    # FORCE INDEX: same correctness.
    h.assert_scalar("ci ref accent-aware count (FORCE INDEX)",
                    "SELECT COUNT(*) FROM accent_ci FORCE INDEX(ix_s) "
                    "WHERE s = 'e'", "3")

    # JOIN ON ci ref: each outer row's value drives a fresh ref into
    # the indexed inner; the filter fires per outer row.
    h.exec_script(f"""
CREATE TABLE accent_outer (id INT NOT NULL PRIMARY KEY,
                          s VARCHAR(20) NOT NULL) ENGINE={h.engine};
INSERT INTO accent_outer VALUES (1,'e');
""")
    h.assert_scalar("ci JOIN ref accent-aware count",
                    "SELECT COUNT(*) FROM accent_outer o JOIN accent_ci i "
                    "FORCE INDEX(ix_s) ON i.s = o.s", "3")

    # IGNORE INDEX still works (full-scan + Using-where with MariaDB's
    # collation server-side).
    h.assert_scalar("IGNORE INDEX still gives ci-correct count",
                    "SELECT COUNT(*) FROM accent_ci IGNORE INDEX(ix_s) "
                    "WHERE s = 'e'", "3")

    # trust_binary_strings opts into byte-exact semantics; the ci-
    # filter is skipped so the engine compares bytes directly. Row 1
    # is the only byte-exact match for 'e'.
    h.exec_stmt("SET stoolap_trust_binary_strings = 1")
    h.assert_scalar("trust_binary_strings=1 byte-exact",
                    "SELECT COUNT(*) FROM accent_ci WHERE s = 'e'", "1")
    h.exec_stmt("SET stoolap_trust_binary_strings = 0")

    # A _bin column also gives byte-exact semantics by definition.
    h.exec_script(f"""
CREATE TABLE accent_bin (id INT NOT NULL PRIMARY KEY,
                        s VARCHAR(20) COLLATE utf8mb4_bin NOT NULL,
                        KEY ix_s (s)) ENGINE={h.engine};
INSERT INTO accent_bin VALUES (1,'e'),(2,'E'),(3,unhex('C3A9'));
""")
    h.assert_scalar("_bin column byte-exact",
                    "SELECT COUNT(*) FROM accent_bin WHERE s = 'e'", "1")

    h.section("Composite ci ref filters every bound key part")
    # KEY(s, n): the ci filter must ci-compare the leading VARCHAR AND
    # byte-compare the trailing INT before emitting a row. Filtering
    # only the leading part would let JOIN ON st.s=so.s AND st.n=so.n
    # through with the wrong `n`, since the engine has no residual
    # WHERE at the join level (EXPLAIN shows ref + no Using where).
    h.exec_script(f"""
CREATE TABLE comp_ci (id INT NOT NULL PRIMARY KEY,
                      s VARCHAR(40) NOT NULL,
                      n INT NOT NULL,
                      KEY ix (s, n)) ENGINE={h.engine};
INSERT INTO comp_ci VALUES
  (1,'apple',1), (2,'apple',2), (3,'APPLE',2), (4,'apple',4), (5,'BERRY',5);
CREATE TABLE comp_so (s VARCHAR(40) NOT NULL, n INT NOT NULL) ENGINE=InnoDB;
INSERT INTO comp_so VALUES ('apple',2),('Apple',4),('berry',5);
""")
    out = h.sql(
        "SELECT comp_ci.s, comp_ci.n FROM comp_so "
        "JOIN comp_ci FORCE INDEX(ix) "
        "ON comp_ci.s = comp_so.s AND comp_ci.n = comp_so.n "
        # Add a binary DESC tiebreaker so ci-equivalent rows (apple
        # vs APPLE) land in a deterministic order across platforms.
        # Without it, macOS happens to surface lowercase first; Linux
        # MariaDB puts uppercase first. Both are SQL-correct under ci
        # ORDER BY, but the assertion below pins a literal sequence.
        "ORDER BY comp_ci.s, comp_ci.n, BINARY(comp_ci.s) DESC")
    # ci('apple') matches s in {apple, APPLE}; AND n filters:
    #   so('apple',2) -> {(apple,2),(APPLE,2)}
    #   so('Apple',4) -> {(apple,4)}
    #   so('berry',5) -> {(BERRY,5)}
    # Total 4 rows. Without per-part filtering STOOLAP returns 5 (an
    # extra (apple,1) leaks through).
    h.assert_eq("composite ci JOIN ON s+n filters n correctly",
                "apple\t2\nAPPLE\t2\napple\t4\nBERRY\t5", out)
    h.assert_scalar("composite ci JOIN row count matches InnoDB",
                    "SELECT COUNT(*) FROM comp_so JOIN comp_ci FORCE INDEX(ix) "
                    "ON comp_ci.s = comp_so.s AND comp_ci.n = comp_so.n", "4")

    h.section("Trailing ci key part triggers ci fallback (KEY(n, s))")
    # KEY(n, s): leading part `n` is byte-safe (INT) but the trailing
    # part `s` is ci-collated. The fallback must engage on ANY bound ci
    # key part, not just the leading one. Without that, the bytewise
    # `s = $` predicate would byte-match only 'e' and miss 'E'/'é'.
    h.exec_script(f"""
CREATE TABLE trail_ci (id INT NOT NULL PRIMARY KEY,
                      n INT NOT NULL,
                      s VARCHAR(40) NOT NULL,
                      KEY ix (n, s)) ENGINE={h.engine};
INSERT INTO trail_ci VALUES (1,1,'e'),(2,1,'E'),(3,1,unhex('C3A9'));
CREATE TABLE trail_so (id INT NOT NULL PRIMARY KEY,
                      n INT NOT NULL,
                      s VARCHAR(40) NOT NULL) ENGINE=InnoDB;
INSERT INTO trail_so VALUES (1,1,'e');
""")
    h.assert_scalar("trailing ci JOIN matches all ci-equiv rows",
                    "SELECT COUNT(*) FROM trail_so JOIN trail_ci "
                    "ON trail_ci.n = trail_so.n AND trail_ci.s = trail_so.s",
                    "3")
    out = h.sql(
        "SELECT trail_ci.id FROM trail_so JOIN trail_ci "
        "ON trail_ci.n = trail_so.n AND trail_ci.s = trail_so.s "
        "ORDER BY trail_ci.id")
    h.assert_eq("trailing ci JOIN row set matches InnoDB ci semantics",
                "1\n2\n3", out)

    h.section("trust_binary_strings round-trip is stateless")
    # An earlier round mutated KEY::rec_per_key in info() to poison
    # cardinality for ci-leading-key indexes. That mutation persisted
    # across queries, so a default-mode EXPLAIN before re-enabling
    # trust_binary_strings would leak the poisoned stats and cause
    # MariaDB to apply ci collation server-side under trust=1, where
    # users expect byte-exact results. The fix removes the persistent
    # mutation; cost gating lives entirely in per-call keyread_time
    # and records_in_range.
    h.exec_script(f"""
CREATE TABLE rt_st (id INT NOT NULL PRIMARY KEY,
                    s VARCHAR(40) NOT NULL, n INT NOT NULL,
                    KEY ix (s, n)) ENGINE={h.engine};
INSERT INTO rt_st VALUES (1,'e',1),(2,'E',1),(3,unhex('C3A9'),1);
CREATE TABLE rt_so (id INT NOT NULL PRIMARY KEY,
                    s VARCHAR(40) NOT NULL, n INT NOT NULL) ENGINE=InnoDB;
INSERT INTO rt_so VALUES (1,'e',1);
""")
    join_q = ("SELECT rt_st.id FROM rt_so JOIN rt_st "
              "ON rt_st.s = rt_so.s AND rt_st.n = rt_so.n "
              "ORDER BY rt_st.id")

    h.exec_stmt("SET stoolap_trust_binary_strings = 1")
    h.assert_eq("trust=1 fresh: byte-exact", "1", h.sql(join_q))

    # Default-mode probe in between (poison-bait for the old bug).
    h.exec_stmt("SET stoolap_trust_binary_strings = 0")
    h.assert_eq("trust=0: ci-correct (3 matches)",
                "1\n2\n3", h.sql(join_q))

    # Trust-on result MUST still be byte-exact, not the ci-leaked 3.
    h.exec_stmt("SET stoolap_trust_binary_strings = 1")
    h.assert_eq("trust=1 after toggle: still byte-exact", "1",
                h.sql(join_q))
    h.exec_stmt("SET stoolap_trust_binary_strings = 0")

    h.section("ci ref cost gate steers planner to BNL")
    # The handler's ci fallback runs a full table scan per ref probe,
    # so reporting a unique-1 point lookup would put a small outer
    # against a big ci-keyed inner under nested-loop and re-scan the
    # inner per outer row. keyread_time + scan_time + records_in_range
    # together signal the real per-probe cost; on a 1000-row inner the
    # planner switches from `ref, rows=1` to `ALL` + BNL join buffer
    # (one scan, hash-style probe).
    h.exec_script(f"""
CREATE TABLE big_ci (id INT NOT NULL PRIMARY KEY,
                    s VARCHAR(40) NOT NULL,
                    n INT NOT NULL,
                    KEY ix (s, n)) ENGINE={h.engine};
""")
    h.run_client(f"""
USE {h.db};
SET SESSION max_recursive_iterations = 2000000;
INSERT INTO big_ci (id, s, n) WITH RECURSIVE seq AS
    (SELECT 1 AS i UNION ALL SELECT i+1 FROM seq WHERE i < 1000)
    SELECT i, CONCAT('u_', i), i FROM seq;
""")
    h.exec_script("""
CREATE TABLE big_so (id INT NOT NULL PRIMARY KEY,
                    s VARCHAR(40) NOT NULL,
                    n INT NOT NULL) ENGINE=InnoDB;
INSERT INTO big_so VALUES (1,'u_5',5),(2,'u_500',500),(3,'u_999',999);
""")
    explain = h.sql(
        "EXPLAIN SELECT big_ci.id FROM big_so "
        "JOIN big_ci ON big_ci.s = big_so.s AND big_ci.n = big_so.n")
    if "join buffer" in explain.lower():
        h._pass("planner picks BNL join (one inner scan, no per-probe ref)")
    else:
        h._fail("planner still picks ci ref scan-per-probe",
                f"explain: {explain}")
    h.assert_scalar("ci JOIN result correct under cost-gated plan",
                    "SELECT COUNT(*) FROM big_so JOIN big_ci "
                    "ON big_ci.s = big_so.s AND big_ci.n = big_so.n", "3")

    # trust_binary_strings skips the cost gate; planner is free to use
    # ref because the engine then does a real byte-equal index lookup
    # (no per-probe scan).
    h.exec_stmt("SET stoolap_trust_binary_strings = 1")
    explain_trust = h.sql(
        "EXPLAIN SELECT big_ci.id FROM big_so "
        "JOIN big_ci ON big_ci.s = big_so.s AND big_ci.n = big_so.n")
    if "\tref\t" in explain_trust:
        h._pass("trust_binary_strings=1 lets planner pick ref")
    else:
        h._fail("trust_binary_strings=1 lost ref plan",
                f"explain: {explain_trust}")
    h.exec_stmt("SET stoolap_trust_binary_strings = 0")

    h.section("DROP INDEX")
    h.assert_ok("drop index", "ALTER TABLE u DROP INDEX idx_age")
    h.assert_scalar("select still works", "SELECT COUNT(*) FROM u WHERE age = 31", "1")

    h.section("Reconcile re-creates secondary indexes")
    h.exec_script(f"""
CREATE TABLE rec_u (
    id INT NOT NULL PRIMARY KEY,
    age INT NOT NULL,
    score INT NOT NULL,
    KEY idx_rec_age (age),
    KEY idx_rec_score (score)
) ENGINE={h.engine};
INSERT INTO rec_u VALUES (1,30,100),(2,25,200);
""")
    out = h.sql("SHOW INDEXES FROM rec_u")
    keys = sorted({line.split("\t")[2] for line in out.split("\n") if line})
    h.assert_eq("secondary indexes listed after CREATE",
                "PRIMARY,idx_rec_age,idx_rec_score",
                ",".join(keys))

    h.assert_ok("drop one secondary index",
                "ALTER TABLE rec_u DROP INDEX idx_rec_age")
    h.assert_ok("re-add secondary index",
                "ALTER TABLE rec_u ADD KEY idx_rec_age (age)")
