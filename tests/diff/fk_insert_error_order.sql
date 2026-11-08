-- A multi-row INSERT into a table a FOREIGN KEY names fails with its first
-- failing row's error: an earlier row's missing parent (1452) before a
-- later row's duplicate (1062), and the other way round; IGNORE skips
-- each failing row.
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, u INT, UNIQUE KEY cu (u), FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO p VALUES (1);
INSERT INTO c VALUES (1, 1, 10);
INSERT INTO c VALUES (2, 999, 20), (3, 1, 10);
INSERT INTO c VALUES (2, 1, 10), (3, 999, 30);
INSERT INTO c VALUES (2, 1, 20), (3, 1, 20);
INSERT INTO c VALUES (2, 1, 20), (3, 1, 30), (4, 1, 30);
INSERT IGNORE INTO c VALUES (5, 1, 50), (6, 1, 50), (7, 1, 10), (8, 1, 80);
SELECT id, pid, u FROM c ORDER BY id;
