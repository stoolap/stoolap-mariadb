-- Direct UPDATE / DELETE generated from the Item tree: MariaDB's SET
-- order, store rules (a row that would not fit runs row by row, with
-- MariaDB's error) and changed-row counts.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, n INT NOT NULL, m INT, s VARCHAR(4) COLLATE utf8mb4_bin,
                f DOUBLE, d DATE, KEY kd (d)) ENGINE={E};
-- known-diff-final: the row pump keeps rows changed before the failing one (no statement rollback, as audit_wr01; stoolap/stoolap#884)
INSERT INTO t VALUES (1, 2147483646, 1, 'ab', 1.5, '2024-01-10'), (2, 5, NULL, 'abcd', NULL, '2023-12-31'),
                     (3, 7, 3, NULL, 2.0, '2024-02-01');
-- In range: direct. The next one overflows INT for id 1 only.
UPDATE t SET n = n + 1 WHERE id = 1;
UPDATE t SET n = n + 1;
SELECT id, n FROM t ORDER BY id;
UPDATE t SET n = n + 1 WHERE id <> 1;
-- NULL into NOT NULL, from a nullable expression.
UPDATE t SET n = m WHERE id = 2;
UPDATE t SET n = m WHERE id = 3;
-- Too long for VARCHAR(4) only on some rows.
UPDATE t SET s = CONCAT(s, 'x') WHERE id = 1;
UPDATE t SET s = CONCAT(s, 'x');
-- known-diff: the row pump keeps rows changed before the failing one (no statement rollback, as audit_wr01; stoolap/stoolap#884)
SELECT id, n, s FROM t ORDER BY id;
-- MariaDB assigns left to right: a swap is not a swap.
UPDATE t SET n = m, m = n WHERE id = 3;
SELECT id, n, m FROM t ORDER BY id;
-- A multiple equality in WHERE, an integer into DOUBLE, unchanged rows.
UPDATE t SET f = n WHERE m = id AND id = 3;
UPDATE t SET f = 2.0 WHERE id = 3;
UPDATE t SET f = f WHERE f IS NOT NULL;
SELECT id, f FROM t ORDER BY id;
-- Date conditions push; date SET values run row by row.
DELETE FROM t WHERE d < '2024-01-01';
UPDATE t SET d = '2024-03-01' WHERE d >= '2024-02-01';
UPDATE t SET m = 9 WHERE d BETWEEN '2024-01-01' AND '2024-01-31';
SELECT id, m, d FROM t ORDER BY id;
