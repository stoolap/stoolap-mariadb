-- known-diff-file: DDL: RENAME of a parent leaves its children's FKs naming the old name (stoolap's rename keeps them), so the parent is dropped and the child refuses every row
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO p VALUES (1);
RENAME TABLE p TO p2;
INSERT INTO c VALUES (1, 1);
DROP TABLE p2;
INSERT INTO c VALUES (2, 77);
SELECT id, pid FROM c ORDER BY id;
