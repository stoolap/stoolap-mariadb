-- An UPDATE of a UNIQUE column on a table a FOREIGN KEY names stays a
-- direct UPDATE: the row pump would write it through SQL one statement per
-- row (in autocommit, a failed row would keep the rows before it) and
-- cannot name a row of a table with no primary key.
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
INSERT INTO p VALUES (1);
CREATE TABLE c (id INT PRIMARY KEY, pid INT, u INT, UNIQUE KEY cu (u), FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO c VALUES (1, 1, 10), (2, 1, 20);
UPDATE c SET u = 0;
SELECT id, u FROM c ORDER BY id;
CREATE TABLE d (u INT NULL, pid INT, UNIQUE KEY du (u), FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO d VALUES (10, 1), (20, 1);
UPDATE d SET u = 30 WHERE u = 10;
SELECT u, pid FROM d ORDER BY u;
