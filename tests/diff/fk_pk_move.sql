-- A primary key UPDATE on a table a FOREIGN KEY names (written through
-- SQL, where stoolap refuses an UPDATE of the key) moves the row on the
-- storage handle, with what the keys do around it.
CREATE TABLE p (id INT NOT NULL PRIMARY KEY, v INT) ENGINE={E};
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
CREATE TABLE cc (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (pid) REFERENCES p (id) ON UPDATE CASCADE) ENGINE={E};
CREATE TABLE cn (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (pid) REFERENCES p (id) ON UPDATE SET NULL) ENGINE={E};
CREATE TABLE x (id INT NOT NULL PRIMARY KEY,
                FOREIGN KEY (id) REFERENCES p (id)) ENGINE={E};
INSERT INTO p VALUES (1, 0), (2, 0), (3, 0), (4, 0), (5, 0), (6, 0), (7, 0);
INSERT INTO c VALUES (10, 2), (11, NULL);
INSERT INTO cc VALUES (20, 4);
INSERT INTO cn VALUES (30, 5);
INSERT INTO x VALUES (6);
-- a row no key names moves, with the rest of its SET
UPDATE p SET id = 30 WHERE id = 3;
UPDATE p SET id = id + 100, v = 7 WHERE id BETWEEN 1 AND 1;
-- a referenced row: RESTRICT refuses, CASCADE and SET NULL take the rows
UPDATE p SET id = 20 WHERE id = 2;
UPDATE p SET id = 40 WHERE id = 4;
UPDATE p SET id = 50 WHERE id = 5;
-- onto a key another row holds
UPDATE p SET id = 30 WHERE id = 7;
-- a child's key, and a key that is a foreign key itself
UPDATE c SET id = 12 WHERE id = 11;
UPDATE c SET id = id + 1 WHERE id BETWEEN 10 AND 10;
UPDATE x SET id = 999 WHERE id = 6;
-- several rows: one refused undoes the statement, in a transaction too
UPDATE p SET id = id + 1000 ORDER BY id DESC;
BEGIN;
UPDATE p SET id = id + 1 WHERE id IN (7, 30) ORDER BY id;
UPDATE p SET id = id + 1000 ORDER BY id DESC;
UPDATE p SET id = 70 WHERE id = 8;
ROLLBACK;
BEGIN;
UPDATE p SET id = id + 1 WHERE id IN (7, 31) ORDER BY id DESC;
COMMIT;
SELECT id, v FROM p ORDER BY id;
SELECT id, pid FROM c ORDER BY id;
SELECT id, pid FROM cc ORDER BY id;
SELECT id, pid FROM cn ORDER BY id;
SELECT id FROM x ORDER BY id;
