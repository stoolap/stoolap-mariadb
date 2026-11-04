-- known-diff-file: types: utf8mb4_bin UNIQUE ignores PAD SPACE ('a' and 'a ' both accepted)
CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_bin, UNIQUE KEY (s)) ENGINE={E};
INSERT INTO t VALUES (1, 'a');
INSERT INTO t VALUES (2, 'a ');
SELECT id, HEX(s) FROM t ORDER BY id;
