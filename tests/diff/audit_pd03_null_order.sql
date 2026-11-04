-- known-diff-file: pushdown: stoolap sorts NULLs last ASC / first DESC, MariaDB the opposite
CREATE TABLE t (id INT PRIMARY KEY, v INT) ENGINE={E};
INSERT INTO t VALUES (1,5),(2,NULL),(3,1);
SELECT id FROM t ORDER BY v, id;
SELECT id FROM t ORDER BY v DESC, id;
