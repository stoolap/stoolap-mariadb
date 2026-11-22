-- DOUBLE arithmetic past the largest DOUBLE fails the statement (1690),
-- in the SELECT list, a WHERE and a direct UPDATE (stoolap#1102)
CREATE TABLE o (id INT PRIMARY KEY, d DOUBLE) ENGINE={E};
INSERT INTO o VALUES (1, 1e308), (2, 1);
SELECT id, d * 10 FROM o ORDER BY id;
SELECT id FROM o WHERE d * 10 > 0 ORDER BY id;
UPDATE o SET d = d * 10;
SELECT id, d FROM o ORDER BY id;
