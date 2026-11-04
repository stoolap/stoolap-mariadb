-- known-diff-file: write path: a duplicate in a multi-row INSERT reports 1030, not 1062
CREATE TABLE t (id INT NOT NULL PRIMARY KEY) ENGINE={E};
INSERT INTO t VALUES (1);
INSERT INTO t VALUES (2), (1);
SELECT id FROM t ORDER BY id;
