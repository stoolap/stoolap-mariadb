-- MIN / MAX on a nullable indexed column skip its NULLs, which the
-- index's order puts first.
CREATE TABLE t (id INT PRIMARY KEY, b INT NULL, KEY kb (b)) ENGINE={E};
INSERT INTO t VALUES (1,5),(2,NULL),(3,2);
CREATE TABLE i (k INT PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO i VALUES (1);
SELECT MIN(t.b), MAX(t.b) FROM t, i;
