-- known-diff-file: pushdown: '||' is concatenation in stoolap, OR in MariaDB's default sql_mode
CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT) ENGINE={E};
INSERT INTO t VALUES (1,1,2),(2,0,5),(3,0,0);
SELECT id, a || b FROM t ORDER BY id;
SELECT id FROM t WHERE a = 1 || b = 5 ORDER BY id;
