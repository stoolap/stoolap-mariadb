-- known-diff-file: pushdown: "x" is an identifier in stoolap, a string literal in MariaDB
CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(5) COLLATE utf8mb4_bin, b VARCHAR(5) COLLATE utf8mb4_bin) ENGINE={E};
INSERT INTO t VALUES (1,'b','x'),(2,'y','y');
SELECT id FROM t WHERE a = "b" ORDER BY id;
