-- known-diff-file: AUTO_INCREMENT on a column other than the PRIMARY KEY is recomputed from MAX(col) + 1 after a copying ALTER, a RENAME or an index change, so the ids of deleted top rows are handed out again
CREATE TABLE t (id INT PRIMARY KEY, a INT NOT NULL AUTO_INCREMENT, KEY (a)) ENGINE={E};
INSERT INTO t (id) VALUES (1), (2), (3);
DELETE FROM t WHERE id = 3;
ALTER TABLE t FORCE;
INSERT INTO t (id) VALUES (4);
RENAME TABLE t TO t2;
INSERT INTO t2 (id) VALUES (5);
DELETE FROM t2 WHERE id = 5;
ALTER TABLE t2 ADD KEY k2 (id, a);
INSERT INTO t2 (id) VALUES (6);
SELECT id, a FROM t2 ORDER BY id;
