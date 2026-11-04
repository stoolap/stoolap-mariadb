-- ORDER BY <pk> LIMIT n drives index_first + index_next on the handler.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 2), (3, 3), (4, 4), (5, 5);
UPDATE t SET n = n + 100 ORDER BY id LIMIT 3;
SELECT id, n FROM t ORDER BY id;
DELETE FROM t ORDER BY id LIMIT 2;
SELECT id FROM t ORDER BY id;
UPDATE t SET n = n + 1 WHERE id > 3 LIMIT 5;
SELECT MIN(id), MAX(id), SUM(n) FROM t;
