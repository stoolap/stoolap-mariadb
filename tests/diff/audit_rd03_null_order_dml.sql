-- known-diff-file: read path: ordered index scans put NULLs last; ORDER BY LIMIT DML hits the wrong row
CREATE TABLE t (id INT PRIMARY KEY, b INT NULL, KEY kb (b)) ENGINE={E};
INSERT INTO t VALUES (1,5),(2,NULL),(3,2);
DELETE FROM t ORDER BY b LIMIT 1;
SELECT id FROM t ORDER BY id;
