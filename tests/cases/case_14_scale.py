"""Scale: load 50K rows via INSERT...SELECT, then verify aggregates and
indexed scans return correct results, and that the pushed result matches
an InnoDB control table populated identically."""


ROWS = 50000


def run(h):
    h.exec_script(f"""
SET SESSION max_recursive_iterations = 1000000;
CREATE TABLE big_st (
    id INT NOT NULL PRIMARY KEY,
    grp INT NOT NULL,
    bal DOUBLE NOT NULL,
    KEY idx_grp (grp)
) ENGINE={h.engine};
CREATE TABLE big_in LIKE big_st;
ALTER TABLE big_in ENGINE=InnoDB;

INSERT INTO big_st (id, grp, bal)
WITH RECURSIVE seq AS (SELECT 1 AS i UNION ALL SELECT i+1 FROM seq WHERE i < {ROWS})
SELECT i, (i % 100), (i * 1.5) FROM seq;
INSERT INTO big_in SELECT * FROM big_st;
""")

    h.assert_scalar(f"loaded {ROWS} rows", "SELECT COUNT(*) FROM big_st", str(ROWS))

    h.section("Aggregate result parity (stoolap vs InnoDB)")
    queries = [
        "SELECT COUNT(*) FROM %T",
        "SELECT SUM(id) FROM %T",
        "SELECT MIN(bal), MAX(bal) FROM %T",
        "SELECT COUNT(DISTINCT grp) FROM %T",
        "SELECT grp, COUNT(*) FROM %T WHERE grp < 5 GROUP BY grp ORDER BY grp",
        "SELECT id FROM %T WHERE id BETWEEN 100 AND 110 ORDER BY id",
        "SELECT MAX(bal) FROM %T WHERE grp = 7",
    ]
    for q in queries:
        q_st = q.replace("%T", "big_st")
        q_in = q.replace("%T", "big_in")
        out_st = h.sql(q_st)
        out_in = h.sql(q_in)
        label = " ".join(q_st.split())[:60]
        h.assert_eq(label, out_in, out_st)

    h.section("Bulk insert chunked flush (regression)")
    h.exec_script(f"""
SET SESSION max_recursive_iterations = 1000000;
CREATE TABLE chunked (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO chunked (id, n) WITH RECURSIVE seq AS
    (SELECT 1 AS i UNION ALL SELECT i+1 FROM seq WHERE i < 120000)
    SELECT i, i*2 FROM seq;
""")
    h.assert_scalar("chunked bulk INSERT row count",   "SELECT COUNT(*) FROM chunked", "120000")
    h.assert_scalar("chunked first row",                "SELECT n FROM chunked WHERE id=1",      "2")
    h.assert_scalar("chunked across-threshold row",     "SELECT n FROM chunked WHERE id=50001",  "100002")
    h.assert_scalar("chunked last row",                 "SELECT n FROM chunked WHERE id=120000", "240000")

    # Statement atomicity: pre-insert id=70000 (in second chunk's range)
    # so the bulk INSERT 1..120000 hits a PK duplicate after the first
    # chunk has executed its tx_stmt_exec calls. With chunked autocommit
    # wrapped in a single stoolap tx, the duplicate triggers a rollback
    # that wipes the first chunk too.
    h.exec_script(f"""
SET SESSION max_recursive_iterations = 1000000;
CREATE TABLE chunked_atomic (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={h.engine};
INSERT INTO chunked_atomic VALUES (70000, -1);
""")
    h.run_client("""
SET SESSION max_recursive_iterations = 1000000;
INSERT INTO chunked_atomic (id, n)
WITH RECURSIVE seq AS
    (SELECT 1 AS i UNION ALL SELECT i+1 FROM seq WHERE i < 120000)
SELECT i, i*2 FROM seq;
""", force=True)
    h.assert_scalar("chunked bulk INSERT atomicity",
                    "SELECT COUNT(*) FROM chunked_atomic", "1")
    h.assert_scalar("chunked atomicity first chunk rolled back",
                    "SELECT COUNT(*) FROM chunked_atomic WHERE id < 50000", "0")

    h.section("DELETE / UPDATE on large dataset")
    h.assert_ok("DELETE half",      "DELETE FROM big_st WHERE id % 2 = 0")
    h.assert_scalar("after DELETE", "SELECT COUNT(*) FROM big_st", str(ROWS // 2))
    h.assert_ok("UPDATE remaining", "UPDATE big_st SET bal = bal + 1 WHERE grp = 0")
    expected = h.sql("SELECT COUNT(*) FROM big_st WHERE grp = 0")
    h.assert_scalar("subset still queryable",
                    "SELECT COUNT(*) FROM big_st WHERE grp = 0", expected)
