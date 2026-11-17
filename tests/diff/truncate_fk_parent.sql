-- known-diff-file: TRUNCATE of a table a FOREIGN KEY names is refused (1451) only while a child row names it; InnoDB refuses it always (1701)
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
INSERT INTO p VALUES (1), (2);
TRUNCATE TABLE p;
INSERT INTO p VALUES (1), (2);
INSERT INTO c VALUES (1, 1);
TRUNCATE TABLE p;
SELECT id FROM p ORDER BY id;
