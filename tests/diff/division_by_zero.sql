-- Under strict ERROR_FOR_DIVISION_BY_ZERO (the default sql_mode) a
-- division by zero fails an UPDATE that computes from it, in its SET or
-- its WHERE (1365); DELETE and SELECT give NULL
CREATE TABLE z (id INT PRIMARY KEY, d DOUBLE, e DOUBLE, n INT) ENGINE={E};
INSERT INTO z VALUES (1, 10, 0, 0), (2, 20, 2, 0);
UPDATE z SET d = d / e;
UPDATE z SET n = 1 WHERE d / e > 1;
UPDATE z SET d = d / 2;
UPDATE z SET d = d / 0.5 WHERE id = 2;
UPDATE IGNORE z SET d = d / e;
SELECT id, d / e FROM z ORDER BY id;
DELETE FROM z WHERE d / e > 100;
SET SESSION sql_mode = 'STRICT_TRANS_TABLES';
UPDATE z SET d = d / e;
SET SESSION sql_mode = DEFAULT;
