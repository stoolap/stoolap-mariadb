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
-- IGNORE skips a row whose move fails, and leaves the row as it was
UPDATE IGNORE p SET id = 2, v = 9 WHERE id = 8;
-- the move fails down a cascade: nothing of the row stays either
CREATE TABLE q (id INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE qc (id INT NOT NULL PRIMARY KEY,
                 FOREIGN KEY (id) REFERENCES q (id) ON UPDATE CASCADE) ENGINE={E};
CREATE TABLE qg (id INT NOT NULL PRIMARY KEY, cq INT,
                 FOREIGN KEY (cq) REFERENCES qc (id)) ENGINE={E};
INSERT INTO q VALUES (1), (2);
INSERT INTO qc VALUES (1), (2);
INSERT INTO qg VALUES (100, 1);
UPDATE IGNORE q SET id = id + 10 ORDER BY id;
SELECT id, cq FROM qg ORDER BY id;
-- DROP DATABASE refuses a three-table chain with rows on STOOLAP
DELETE FROM qg;
-- a key that is a child's own primary key moves that row, and a child
-- naming the key twice takes both along
CREATE TABLE r (id INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE rx (id INT NOT NULL PRIMARY KEY,
                 FOREIGN KEY (id) REFERENCES r (id) ON UPDATE CASCADE) ENGINE={E};
CREATE TABLE r2 (id INT NOT NULL PRIMARY KEY, a INT, b INT,
                 FOREIGN KEY (a) REFERENCES r (id) ON UPDATE CASCADE,
                 FOREIGN KEY (b) REFERENCES r (id) ON UPDATE CASCADE) ENGINE={E};
INSERT INTO r VALUES (1), (2);
INSERT INTO rx VALUES (1);
INSERT INTO r2 VALUES (10, 2, 2), (11, 2, NULL);
UPDATE r SET id = 100 WHERE id = 1;
UPDATE r SET id = 200 WHERE id = 2;
-- a key column named in upper case beyond ASCII
CREATE TABLE u (`Ä` INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE uc (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (pid) REFERENCES u (`Ä`)) ENGINE={E};
INSERT INTO u VALUES (1), (2);
INSERT INTO uc VALUES (10, 1);
UPDATE u SET `Ä` = 9 WHERE `Ä` = 1;
UPDATE u SET `Ä` = 20 WHERE `Ä` = 2;
SELECT id, v FROM p ORDER BY id;
SELECT id FROM q ORDER BY id;
SELECT id FROM qc ORDER BY id;
SELECT id FROM r ORDER BY id;
SELECT id FROM rx ORDER BY id;
SELECT id, a, b FROM r2 ORDER BY id;
SELECT `Ä` FROM u ORDER BY 1;
SELECT id, pid FROM uc ORDER BY id;
SELECT id, pid FROM c ORDER BY id;
SELECT id, pid FROM cc ORDER BY id;
SELECT id, pid FROM cn ORDER BY id;
SELECT id FROM x ORDER BY id;
