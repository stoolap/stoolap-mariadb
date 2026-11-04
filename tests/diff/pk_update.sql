-- Stoolap's single-column INTEGER PRIMARY KEY is its row id and refuses
-- UPDATE ("cannot UPDATE primary key column ... Use DELETE + INSERT").
-- Direct DML runs that SQL; the row pump moves the row by its id.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, n INT NOT NULL) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 2);
-- known-diff: stoolap refuses UPDATE of a single-column PRIMARY KEY
UPDATE t SET id = 10 WHERE id = 1;
UPDATE t SET id = id + 100 WHERE id = 2 LIMIT 1;
-- known-diff-final: the refused direct UPDATE leaves id 1
