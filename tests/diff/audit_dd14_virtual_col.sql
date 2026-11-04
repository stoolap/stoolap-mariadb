-- known-diff-file: DDL: VIRTUAL generated columns are refused
CREATE TABLE t (id INT PRIMARY KEY, a INT, v INT AS (a * 2) VIRTUAL) ENGINE={E};
INSERT INTO t (id, a) VALUES (1, 1);
UPDATE t SET a = 5;
SELECT id, v FROM t ORDER BY id;
SELECT id FROM t WHERE v = 10 ORDER BY id;
