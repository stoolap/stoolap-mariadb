-- known-diff-file: pushdown: utf8mb4_bin is PAD SPACE in MariaDB, stoolap compares bytes
CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_bin) ENGINE={E};
INSERT INTO t VALUES (1,'a'),(2,'a ');
SELECT id FROM t WHERE s = 'a' ORDER BY id;
SELECT COUNT(DISTINCT s) FROM t;
