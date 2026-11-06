CREATE TABLE t (id INT NOT NULL PRIMARY KEY, a INT, b INT) ENGINE={E};
INSERT INTO t VALUES (1, 1, 0), (2, 7, 0), (3, 9, 0);
UPDATE t SET a = a + 1, b = a WHERE id = 1;
UPDATE t SET b = a / 2 WHERE id = 2;
SELECT id, a, b FROM t ORDER BY id;
DELETE FROM t WHERE id = 1 || id = 3;
SELECT id, a, b FROM t ORDER BY id;
