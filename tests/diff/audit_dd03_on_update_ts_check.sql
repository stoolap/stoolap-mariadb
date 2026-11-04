CREATE TABLE t (id INT PRIMARY KEY, a INT CHECK (a > 0), ts TIMESTAMP NULL DEFAULT NULL ON UPDATE CURRENT_TIMESTAMP) ENGINE={E};
INSERT INTO t VALUES (1, 1, '2000-01-01 00:00:00');
UPDATE t SET a = 2;
SELECT id, a, ts = '2000-01-01 00:00:00' FROM t ORDER BY id;
UPDATE t SET a = -1;
SELECT id, a FROM t ORDER BY id;
-- The two engines' clock values differ by when each INSERT/UPDATE ran;
-- the checks above are clock-free, so the table is dropped before the
-- final comparison.
DROP TABLE t;
