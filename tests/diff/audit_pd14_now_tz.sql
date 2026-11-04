-- A fixed non-UTC session zone keeps the result independent of the host's.
SET time_zone = '+05:00';
CREATE TABLE t (id INT PRIMARY KEY, ts DATETIME) ENGINE={E};
INSERT INTO t VALUES (1, NOW() - INTERVAL 1 HOUR);
SELECT id FROM t WHERE ts < NOW() ORDER BY id;
-- The two engines' clock values differ by when each INSERT/UPDATE ran;
-- the checks above are clock-free, so the table is dropped before the
-- final comparison.
DROP TABLE t;
