-- known-diff-file: pushdown: ROUND on DOUBLE rounds half away from zero, MariaDB half to even
CREATE TABLE t (id INT PRIMARY KEY, f DOUBLE) ENGINE={E};
INSERT INTO t VALUES (1,2.5),(2,3.5),(3,-2.5);
SELECT id, ROUND(f) FROM t ORDER BY id;
