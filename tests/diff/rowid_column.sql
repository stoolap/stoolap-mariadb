-- Stoolap keeps `_rowid_` for a row's identity and refuses it as a column
-- or an alias; MariaDB takes it as any other name, in any case. The plugin
-- gives such a column another stoolap name, so it is created, read,
-- written, keyed and altered as any other column.
CREATE TABLE t (`_rowid_` INT, v INT) ENGINE={E};
INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);
UPDATE t SET `_rowid_` = `_rowid_` + 100 WHERE v = 20;
DELETE FROM t WHERE `_rowid_` = 3;
SELECT `_rowid_`, v FROM t ORDER BY `_rowid_`;
SELECT v AS `_rowid_` FROM t ORDER BY 1;
SELECT d.`_rowid_` FROM (SELECT v AS `_rowid_` FROM t) d ORDER BY 1;
SELECT `_ROWID_` % 2, COUNT(*) FROM t GROUP BY 1 ORDER BY 1;
-- a primary key, a UNIQUE key and an index on the name
CREATE TABLE k (`_ROWID_` INT NOT NULL PRIMARY KEY AUTO_INCREMENT,
                `_rowid_x` INT, u INT, UNIQUE KEY (u)) ENGINE={E};
INSERT INTO k (`_rowid_x`, u) VALUES (1, 1), (2, 2);
INSERT INTO k VALUES (10, 3, 3);
INSERT INTO k (`_rowid_x`, u) VALUES (4, 4);
INSERT INTO k VALUES (30, 5, 1);
UPDATE k SET `_rowid_` = 20 WHERE `_rowid_` = 10;
UPDATE k SET u = 9 WHERE `_rowid_` = 1;
UPDATE k SET u = u + 1 WHERE `_rowid_` > 1 ORDER BY `_rowid_` DESC LIMIT 2;
SELECT `_rowid_`, `_rowid_x`, u FROM k WHERE `_rowid_` BETWEEN 2 AND 30 ORDER BY 1;
CREATE TABLE i (id INT NOT NULL PRIMARY KEY, `_rowid_` INT, KEY (`_rowid_`)) ENGINE={E};
INSERT INTO i VALUES (1, 5), (2, 6), (3, 5);
SELECT id FROM i WHERE `_rowid_` = 5 ORDER BY id;
-- ALTER: add it, rename to and from it, drop it, CHANGE to it
ALTER TABLE i ADD COLUMN `_Rowid_` INT, ALGORITHM=COPY;
CREATE TABLE a (id INT NOT NULL PRIMARY KEY, n INT) ENGINE={E};
INSERT INTO a VALUES (1, 7);
ALTER TABLE a ADD COLUMN `_rowid_` INT DEFAULT 3, ALGORITHM=COPY;
UPDATE a SET `_rowid_` = n + `_rowid_`;
ALTER TABLE a RENAME COLUMN `_rowid_` TO m, ALGORITHM=COPY;
ALTER TABLE a RENAME COLUMN n TO `_rowid_`, ALGORITHM=COPY;
SELECT id, `_rowid_`, m FROM a;
ALTER TABLE a DROP COLUMN `_rowid_`, ALGORITHM=COPY;
SELECT * FROM a;
ALTER TABLE a CHANGE m `_ROWID_` BIGINT, ALGORITHM=COPY;
UPDATE a SET `_rowid_` = `_rowid_` * 2;
SELECT * FROM a;
-- a FOREIGN KEY from and to it
CREATE TABLE p (`_rowid_` INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, `_rowid_` INT,
                FOREIGN KEY (`_rowid_`) REFERENCES p (`_rowid_`)
                ON UPDATE CASCADE ON DELETE SET NULL) ENGINE={E};
INSERT INTO p VALUES (1), (2);
INSERT INTO c VALUES (10, 1), (11, 2);
INSERT INTO c VALUES (12, 3);
UPDATE p SET `_rowid_` = 5 WHERE `_rowid_` = 1;
DELETE FROM p WHERE `_rowid_` = 2;
SELECT id, `_rowid_` FROM c ORDER BY id;
ALTER TABLE c ADD COLUMN w INT, ALGORITHM=COPY;
INSERT INTO c VALUES (13, 4, 0);
SELECT id, `_rowid_`, w FROM c ORDER BY id;
