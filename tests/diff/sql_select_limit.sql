-- sql_select_limit caps the statement's outermost SELECT, not a subquery,
-- a derived table or an explicit LIMIT
CREATE TABLE t (id INT PRIMARY KEY, g INT) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 1), (3, 2), (4, 2), (5, 3);
SET SESSION sql_select_limit = 2;
SELECT id FROM t ORDER BY id;
SELECT id FROM t ORDER BY id DESC;
SELECT g, COUNT(*) FROM t GROUP BY g;
SELECT id FROM t UNION ALL SELECT id FROM t ORDER BY 1;
SELECT id FROM t UNION SELECT g FROM t ORDER BY 1 DESC;
SELECT id FROM t ORDER BY id LIMIT 3;
SELECT id FROM t ORDER BY id LIMIT 1, 3;
SELECT d.id FROM (SELECT id FROM t ORDER BY id) d ORDER BY d.id DESC;
SELECT id FROM t WHERE g IN (SELECT g FROM t WHERE id >= 2) ORDER BY id;
SELECT id FROM t a WHERE EXISTS (SELECT 1 FROM t b WHERE b.g = a.g AND b.id > a.id) ORDER BY id;
SELECT (SELECT COUNT(*) FROM t) AS n, id FROM t ORDER BY id;
UPDATE t SET g = g + 10 WHERE id > 0;
SELECT COUNT(*) FROM t WHERE g > 10;
SET SESSION sql_select_limit = DEFAULT;
SELECT id FROM t ORDER BY id;
