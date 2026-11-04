-- known-diff-file: types: ENUM stored as its label; MariaDB orders and compares by index
CREATE TABLE t (id INT PRIMARY KEY, e ENUM('small','medium','large') CHARACTER SET utf8mb4 COLLATE utf8mb4_bin) ENGINE={E};
INSERT INTO t VALUES (1,'large'),(2,'small'),(3,'medium');
SELECT id FROM t ORDER BY e;
SELECT id FROM t WHERE e = 2;
