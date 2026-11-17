-- known-diff-file: DOUBLE arithmetic past the largest DOUBLE gives infinity in Stoolap, which reads back as 1.7976931348623157e308, matches in a WHERE and is stored by a direct UPDATE; InnoDB fails the statement (1690) (stoolap#1102)
CREATE TABLE o (id INT PRIMARY KEY, d DOUBLE) ENGINE={E};
INSERT INTO o VALUES (1, 1e308), (2, 1);
SELECT id, d * 10 FROM o ORDER BY id;
SELECT id FROM o WHERE d * 10 > 0 ORDER BY id;
UPDATE o SET d = d * 10;
SELECT id, d FROM o ORDER BY id;
