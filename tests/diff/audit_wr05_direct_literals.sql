-- known-diff-file: direct DML: "x" read as a column; utf8mb4_bin PAD SPACE ignored
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, a VARCHAR(10) COLLATE utf8mb4_bin, b VARCHAR(10) COLLATE utf8mb4_bin) ENGINE={E};
INSERT INTO t VALUES (1, 'x', 'y'), (2, 'b', 'b'), (3, 'q', 'z');
UPDATE t SET a = "b" WHERE id = 1;
DELETE FROM t WHERE a = 'q ';
SELECT id, a, b FROM t ORDER BY id;
