-- A boolean expression used as a value is the integer 1 or 0 (NULL for
-- unknown), as MariaDB has it: in arithmetic, comparisons, IN, CASE
-- branches, aggregates and the select list of a pushed query.
-- case_06_pushdown checks that the select-list shape pushes.
CREATE TABLE bv (id INT NOT NULL PRIMARY KEY, v INT, w INT) ENGINE={E};
INSERT INTO bv VALUES (1, 5, 1), (2, 0, 7), (3, NULL, 2), (4, 2, 2);
SELECT id, (v > 1) + 1, (v > 1) * 10, v > 1, (v > 1) = (w < 3), (v > 1) IN (1, 2), -(v = w) FROM bv ORDER BY id;
SELECT id FROM bv WHERE (v > 1) = 1 ORDER BY id;
SELECT id FROM bv WHERE (v > 1) + (w > 1) = 1 ORDER BY id;
SELECT DISTINCT CASE WHEN v > 1 THEN v > 1 ELSE 1 END FROM bv;
SELECT SUM(v > 1), SUM(v > 1 AND w > 1), AVG(v = w), MAX(v > 1), COUNT(v > 1) FROM bv;
SELECT w > 1, COUNT(*) FROM bv GROUP BY w > 1 ORDER BY 1;
SELECT id, COALESCE(v > 1, 9), IF(v > 1, w > 1, 7), (v BETWEEN 1 AND 3) + (w LIKE '2') FROM bv ORDER BY id;
SELECT TRUE + 1, FALSE * 3, (EXISTS (SELECT 1 FROM bv WHERE v > 4)) + 1 FROM bv WHERE id = 1;
