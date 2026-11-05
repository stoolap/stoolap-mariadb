-- Ordered index scans put NULLs first, so ORDER BY LIMIT DML hits the
-- row MariaDB's order names.
CREATE TABLE t (id INT PRIMARY KEY, b INT NULL, KEY kb (b)) ENGINE={E};
INSERT INTO t VALUES (1,5),(2,NULL),(3,2);
DELETE FROM t ORDER BY b LIMIT 1;
SELECT id FROM t ORDER BY id;
