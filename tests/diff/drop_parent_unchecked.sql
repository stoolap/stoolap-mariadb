-- known-diff-file: under foreign_key_checks = 0 a parent whose child stays is not dropped (1451): InnoDB drops it and keeps the child's key naming no table, which stoolap cannot keep (a DROP ... CASCADE would take the key for good); listed with its children in one DROP TABLE, it is dropped
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
INSERT INTO p VALUES (1);
INSERT INTO c VALUES (1, 1);
SET foreign_key_checks = 0;
DROP TABLE p;
SET foreign_key_checks = 1;
INSERT INTO c VALUES (2, 99);
CREATE TABLE IF NOT EXISTS p (id INT PRIMARY KEY) ENGINE={E};
INSERT INTO p VALUES (2);
INSERT INTO c VALUES (3, 2);
INSERT INTO c VALUES (4, 98);
SELECT id, pid FROM c ORDER BY id;
