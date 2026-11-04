-- known-diff-file: direct UPDATE: rows matched reported as rows changed
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, v INT) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 2);
UPDATE t SET v = v;
UPDATE t SET v = 2 WHERE id > 0;
SELECT id, v FROM t ORDER BY id;
