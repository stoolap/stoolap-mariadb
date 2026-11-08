-- UPDATEs of a UNIQUE column on tables a FOREIGN KEY names (P with
-- upper-case letters, as Linux keeps them). The row pump writes such a
-- table through SQL in the statement's own transaction, checking each
-- row's UNIQUE keys first: a failed statement leaves nothing, IGNORE skips
-- the row, and in a transaction the duplicate fails the statement, not the
-- COMMIT. A table with no primary key stays a direct UPDATE.
CREATE TABLE P (id INT PRIMARY KEY, u INT, UNIQUE KEY pu (u)) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, u INT, UNIQUE KEY cu (u), FOREIGN KEY (pid) REFERENCES P(id)) ENGINE={E};
INSERT INTO P VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO c VALUES (1, 1, 10), (2, 1, 20);
UPDATE P SET u = 0 ORDER BY id LIMIT 2;
UPDATE IGNORE P SET u = 30 ORDER BY id LIMIT 3;
UPDATE P SET u = u + 10 ORDER BY id DESC LIMIT 3;
SELECT id, u FROM P ORDER BY id;
UPDATE c SET u = 0;
UPDATE c SET u = u + 10;
SELECT id, u FROM c ORDER BY id;
INSERT INTO c VALUES (3, 2, 5), (4, 9, 6);
INSERT IGNORE INTO P VALUES (4, 41), (5, 41), (6, 60);
SELECT id, u FROM P ORDER BY id;
BEGIN;
UPDATE c SET u = 0;
UPDATE P SET u = 0 ORDER BY id LIMIT 2;
UPDATE P SET u = 99 ORDER BY id LIMIT 1;
COMMIT;
SELECT id, u FROM P ORDER BY id;
SELECT id, pid, u FROM c ORDER BY id;
CREATE TABLE d (u INT NULL, pid INT, UNIQUE KEY du (u), FOREIGN KEY (pid) REFERENCES P(id)) ENGINE={E};
INSERT INTO d VALUES (10, 1), (20, 1);
UPDATE d SET u = 30 WHERE u = 10;
UPDATE d SET u = 20 WHERE u = 30;
SELECT u, pid FROM d ORDER BY u;
