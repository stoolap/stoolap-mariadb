-- known-diff-file: write path: on a table a FOREIGN KEY names, an UPDATE of a UNIQUE column is checked as a set (u = u + 10 passes where MariaDB meets the next row's value), in a transaction only at COMMIT; and the row pump cannot name a row of such a table with no primary key
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
INSERT INTO p VALUES (1);
CREATE TABLE c (id INT PRIMARY KEY, pid INT, u INT, UNIQUE KEY cu (u), FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO c VALUES (1, 1, 10), (2, 1, 20);
UPDATE c SET u = u + 10;
BEGIN;
UPDATE c SET u = 0;
COMMIT;
SELECT id, u FROM c ORDER BY id;
CREATE TABLE d (u INT NULL, pid INT, UNIQUE KEY du (u), FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO d VALUES (10, 1), (20, 1);
UPDATE d SET pid = 7 ORDER BY u LIMIT 2;
SELECT u, pid FROM d ORDER BY u;
