-- A FOREIGN KEY naming its own table: a tree of rows, each naming its
-- parent row. Stoolap creates and enforces it (#973); the plugin also
-- keeps it through a copying ALTER, whose copy it then names.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, pid INT, v INT,
                FOREIGN KEY (pid) REFERENCES t (id)) ENGINE={E};
INSERT INTO t VALUES (1, NULL, 10), (2, 1, 20), (3, 1, 30), (4, 2, 40);
INSERT INTO t VALUES (5, 9, 50);
INSERT INTO t VALUES (6, 6, 60);
INSERT INTO t VALUES (7, 8, 70), (8, NULL, 80);
UPDATE t SET pid = 99 WHERE id = 3;
UPDATE t SET pid = 4 WHERE id = 3;
DELETE FROM t WHERE id = 2;
UPDATE t SET id = 12 WHERE id = 2;
SELECT id, pid, v FROM t ORDER BY id;
-- the actions, down a tree
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES c (id) ON DELETE CASCADE) ENGINE={E};
INSERT INTO c VALUES (1, NULL), (2, 1), (3, 2), (4, 3), (5, NULL), (6, 5);
DELETE FROM c WHERE id = 2;
SELECT id, pid FROM c ORDER BY id;
CREATE TABLE n (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES n (id) ON DELETE SET NULL) ENGINE={E};
INSERT INTO n VALUES (1, NULL), (2, 1), (3, 1), (4, 2);
DELETE FROM n WHERE id = 1;
SELECT id, pid FROM n ORDER BY id;
-- a copying ALTER keeps the key on the copy, whatever order its rows
-- are copied in (3 names 4)
ALTER TABLE t ADD COLUMN w INT, ALGORITHM=COPY;
INSERT INTO t VALUES (9, 77, 90, 0);
INSERT INTO t VALUES (9, 1, 90, 0);
DELETE FROM t WHERE id = 1;
DELETE FROM t WHERE id = 9;
SELECT id, pid, v, w FROM t ORDER BY id;
-- and by renamed columns (InnoDB renames a key's column only in place,
-- so the table is empty then)
CREATE TABLE r (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES r (id) ON DELETE CASCADE) ENGINE={E};
ALTER TABLE r CHANGE pid parent INT;
ALTER TABLE r RENAME COLUMN id TO rid;
INSERT INTO r VALUES (1, NULL), (2, 1), (3, 2);
INSERT INTO r VALUES (4, 9);
DELETE FROM r WHERE rid = 2;
SELECT rid, parent FROM r ORDER BY rid;
-- a copying ALTER of a table whose key column is also its own key's
-- parent: the check leaves the parent's unchanged values alone
CREATE TABLE kp (id INT NOT NULL PRIMARY KEY) ENGINE={E};
INSERT INTO kp VALUES (1), (2);
CREATE TABLE kc (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (id) REFERENCES kp (id),
                 FOREIGN KEY (pid) REFERENCES kc (id)) ENGINE={E};
INSERT INTO kc VALUES (1, NULL), (2, 1);
ALTER TABLE kc ADD COLUMN v INT, ALGORITHM=COPY;
INSERT INTO kc VALUES (3, 1, 0);
INSERT INTO kc VALUES (2, 3, 0);
SELECT id, pid, v FROM kc ORDER BY id;
-- a key an ALTER adds to its own table
CREATE TABLE a (id INT NOT NULL PRIMARY KEY, pid INT) ENGINE={E};
INSERT INTO a VALUES (1, NULL), (2, 1);
ALTER TABLE a ADD FOREIGN KEY (pid) REFERENCES a (id), ALGORITHM=COPY;
INSERT INTO a VALUES (3, 7);
INSERT INTO a VALUES (3, 2);
DELETE FROM a WHERE id = 1;
SELECT id, pid FROM a ORDER BY id;
-- the copy is checked once whole: a key added over rows that break it
CREATE TABLE b (id INT NOT NULL PRIMARY KEY, pid INT) ENGINE={E};
INSERT INTO b VALUES (1, NULL), (2, 7);
ALTER TABLE b ADD FOREIGN KEY (pid) REFERENCES b (id), ALGORITHM=COPY;
CREATE TABLE e (id INT NOT NULL PRIMARY KEY, bid INT) ENGINE={E};
INSERT INTO e VALUES (1, 1), (2, 3);
ALTER TABLE e ADD FOREIGN KEY (bid) REFERENCES b (id), ALGORITHM=COPY;
SET foreign_key_checks = 0;
ALTER TABLE e ADD FOREIGN KEY (bid) REFERENCES b (id), ALGORITHM=COPY;
SET foreign_key_checks = 1;
INSERT INTO e VALUES (3, 5);
SELECT id, bid FROM e ORDER BY id;
-- RENAME TABLE: the key follows
RENAME TABLE a TO a2;
INSERT INTO a2 VALUES (4, 8);
INSERT INTO a2 VALUES (4, 3);
DELETE FROM a2 WHERE id = 3;
SELECT id, pid FROM a2 ORDER BY id;
-- DROP TABLE of a table whose rows name each other
DROP TABLE n;
DROP TABLE c;
