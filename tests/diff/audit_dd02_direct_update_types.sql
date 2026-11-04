-- known-diff-file: direct UPDATE: no MariaDB range / length / rounding checks, matched counted as changed
CREATE TABLE t (id INT PRIMARY KEY, a TINYINT, s VARCHAR(3), d DATE, i INT) ENGINE={E};
INSERT INTO t VALUES (1, 1, 'x', '2020-01-05', 0);
UPDATE t SET a = 300;
UPDATE t SET s = 'abcdef';
UPDATE t SET i = 2.7;
UPDATE t SET d = '2020-1-6';
UPDATE t SET i = i;
SELECT id, a, s, LENGTH(s), CAST(d AS CHAR), i FROM t ORDER BY id;
SELECT COUNT(*) FROM t WHERE d = '2020-01-06';
