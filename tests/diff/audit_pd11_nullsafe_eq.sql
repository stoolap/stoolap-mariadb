-- known-diff-file: pushdown: '<=>' is vector distance in stoolap, null-safe equality in MariaDB
CREATE TABLE t (id INT PRIMARY KEY, a INT) ENGINE={E};
INSERT INTO t VALUES (1,NULL),(2,3);
SELECT id FROM t WHERE a <=> NULL ORDER BY id;
SELECT id FROM t WHERE a <=> 3 ORDER BY id;
