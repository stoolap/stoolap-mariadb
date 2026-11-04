-- Stoolap's single-column INTEGER PRIMARY KEY is its row id and refuses
-- UPDATE ("cannot UPDATE primary key column ... Use DELETE + INSERT").
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 2);
-- known-diff: stoolap refuses UPDATE of a single-column PRIMARY KEY
UPDATE t SET id = 10 WHERE id = 1;
-- known-diff: stoolap refuses UPDATE of a single-column PRIMARY KEY
UPDATE t SET id = id + 100 WHERE id = 2 LIMIT 1;
-- known-diff-final: the refused PRIMARY KEY updates leave ids 1 and 2
