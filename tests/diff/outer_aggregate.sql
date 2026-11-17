-- An aggregate of an outer query's column inside a subquery belongs to
-- the outer query: one group over its rows
CREATE TABLE t1 (id INT PRIMARY KEY, a INT) ENGINE={E};
CREATE TABLE t2 (id INT PRIMARY KEY, b INT) ENGINE={E};
INSERT INTO t1 VALUES (1, 10), (2, 20);
INSERT INTO t2 VALUES (1, 1);
SELECT (SELECT SUM(t1.a) FROM t2) FROM t1;
SELECT (SELECT MAX(t1.a) + COUNT(*) FROM t2) FROM t1;
SELECT COUNT(*) FROM t1 WHERE a > 5 HAVING (SELECT SUM(t1.a) FROM t2) > 25;
SELECT id, (SELECT SUM(t2.b + t1.a) FROM t2) FROM t1 ORDER BY id;
SELECT id, (SELECT SUM(b) FROM t2 WHERE t2.id <= t1.id) FROM t1 ORDER BY id;
