-- known-diff-file: pushdown: arithmetic on TEXT / DATE gives NULL in stoolap
CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_bin, d DATE) ENGINE={E};
INSERT INTO t VALUES (1,'5','2024-01-31');
SELECT id, s + 1, d + 1 FROM t ORDER BY id;
