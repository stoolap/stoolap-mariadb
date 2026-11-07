-- known-diff-file: write path: the row pump cannot name a row of a table a FOREIGN KEY names that has no primary key (an UPDATE with ORDER BY / LIMIT fails with 1296)
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
INSERT INTO p VALUES (1);
CREATE TABLE d (u INT NULL, pid INT, UNIQUE KEY du (u), FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO d VALUES (10, 1), (20, 1);
UPDATE d SET pid = 7 ORDER BY u LIMIT 2;
UPDATE d SET u = 15 ORDER BY u LIMIT 1;
SELECT u, pid FROM d ORDER BY u;
