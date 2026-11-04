-- known-diff-file: pushdown: stoolap '/' on integers truncates (MariaDB returns DECIMAL)
CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT) ENGINE={E};
INSERT INTO t VALUES (1,7,2),(2,9,3);
SELECT id, a/b FROM t ORDER BY id;
SELECT id FROM t WHERE a/b > 3 ORDER BY id;
