"""Aggregations, GROUP BY/HAVING, subqueries, CTEs, JOINs (INNER/LEFT/CROSS),
and UNION ALL. Mirrors 07_aggregations_joins.sh: same code path as 06_pushdown
but focused on result correctness rather than EXPLAIN classification."""


def run(h):
    h.exec_script(f"""
CREATE TABLE orders (
    id INT NOT NULL PRIMARY KEY,
    cust INT NOT NULL,
    amt INT NOT NULL,
    region VARCHAR(8) NOT NULL,
    KEY idx_cust (cust),
    KEY idx_region (region)
) ENGINE={h.engine};
CREATE TABLE customers (
    id INT NOT NULL PRIMARY KEY,
    name VARCHAR(40) NOT NULL,
    tier VARCHAR(8) NOT NULL
) ENGINE={h.engine};
INSERT INTO customers VALUES (1,'Alice','gold'),(2,'Bob','silver'),(3,'Carol','gold'),(4,'Dave','bronze');
INSERT INTO orders VALUES
  (1,1,100,'NA'),(2,1,200,'NA'),(3,2,50,'EU'),(4,2,150,'EU'),
  (5,3,300,'NA'),(6,3,400,'NA'),(7,3,500,'EU'),(8,1,250,'EU');
""")

    h.section("Aggregates")
    h.assert_scalar("COUNT(*)",        "SELECT COUNT(*) FROM orders",           "8")
    h.assert_scalar("SUM",             "SELECT SUM(amt) FROM orders",          "1950")
    h.assert_scalar("AVG (rounded)",   "SELECT ROUND(AVG(amt)) FROM orders",   "244")
    h.assert_scalar("MIN",             "SELECT MIN(amt) FROM orders",          "50")
    h.assert_scalar("MAX",             "SELECT MAX(amt) FROM orders",          "500")
    h.assert_scalar("COUNT DISTINCT",  "SELECT COUNT(DISTINCT cust) FROM orders", "3")

    h.section("GROUP BY / HAVING")
    out = h.sql("SELECT cust, SUM(amt) FROM orders GROUP BY cust ORDER BY cust")
    h.assert_eq("GROUP BY", "1\t550\n2\t200\n3\t1200", out)
    h.assert_scalar("HAVING > threshold",
                    "SELECT COUNT(*) FROM (SELECT cust FROM orders GROUP BY cust HAVING SUM(amt) > 400) z",
                    "2")

    h.section("Subqueries")
    h.assert_scalar("scalar subquery",
                    "SELECT (SELECT MAX(amt) FROM orders) - (SELECT MIN(amt) FROM orders)", "450")
    h.assert_scalar("IN subquery",
                    "SELECT COUNT(*) FROM customers WHERE id IN (SELECT cust FROM orders WHERE amt > 250)",
                    "1")
    h.assert_scalar("EXISTS",
                    "SELECT COUNT(*) FROM customers c WHERE EXISTS "
                    "(SELECT 1 FROM orders o WHERE o.cust = c.id AND o.amt > 100)", "3")
    h.assert_scalar("derived table",
                    "SELECT MAX(s) FROM (SELECT cust, SUM(amt) AS s FROM orders GROUP BY cust) z",
                    "1200")

    h.section("CTE")
    h.assert_scalar("CTE basic",
                    "WITH big AS (SELECT cust FROM orders WHERE amt > 200) SELECT COUNT(*) FROM big",
                    "4")
    h.assert_scalar("CTE + join",
                    "WITH big AS (SELECT cust FROM orders WHERE amt > 200) "
                    "SELECT COUNT(DISTINCT c.tier) FROM big JOIN customers c ON c.id = big.cust",
                    "1")

    h.section("JOIN")
    h.assert_scalar("INNER JOIN count",
                    "SELECT COUNT(*) FROM orders o JOIN customers c ON c.id = o.cust", "8")
    h.assert_scalar("INNER JOIN tier sum",
                    "SELECT SUM(o.amt) FROM orders o JOIN customers c ON c.id = o.cust "
                    "WHERE c.tier = 'gold'", "1750")

    # LEFT JOIN: customer 4 (Dave) has no orders. Putting c.id in the SELECT
    # list ensures the ORDER BY column is visible to stoolap's planner -- a
    # bare ORDER BY c.id with c.id only in GROUP BY is currently dropped by
    # stoolap's pushed sort (tracked separately as a stoolap-side bug).
    out = h.sql(
        "SELECT c.id, c.name, IFNULL(SUM(o.amt), 0) FROM customers c "
        "LEFT JOIN orders o ON o.cust = c.id GROUP BY c.id, c.name ORDER BY c.id")
    h.assert_eq("LEFT JOIN with NULL",
                "1\tAlice\t550\n2\tBob\t200\n3\tCarol\t1200\n4\tDave\t0", out)

    # Self-join: pairs of orders for the same customer (id1 < id2).
    h.assert_scalar("self-join pairs",
                    "SELECT COUNT(*) FROM orders a JOIN orders b "
                    "ON a.cust = b.cust AND a.id < b.id", "7")

    h.section("UNION ALL")
    h.assert_scalar("UNION ALL row count",
                    "SELECT COUNT(*) FROM (SELECT id FROM orders "
                    "UNION ALL SELECT id FROM customers) z", "12")
    h.assert_scalar("UNION dedup",
                    "SELECT COUNT(*) FROM (SELECT cust AS x FROM orders "
                    "UNION SELECT id FROM customers) z", "4")
