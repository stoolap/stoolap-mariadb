-- known-diff-file: write path: a failed multi-row INSERT keeps its earlier rows (no statement rollback)
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, s VARCHAR(3) COLLATE utf8mb4_bin) ENGINE={E};
INSERT INTO t VALUES (1, 'a');
INSERT INTO t VALUES (2, 'b'), (3, 'toolong'), (4, 'd');
INSERT INTO t VALUES (5, 'e'), (6, 'toolong') ON DUPLICATE KEY UPDATE s = 'x';
BEGIN;
INSERT INTO t VALUES (7, 'g'), (1, 'dup'), (8, 'h');
COMMIT;
SELECT id, s FROM t ORDER BY id;
