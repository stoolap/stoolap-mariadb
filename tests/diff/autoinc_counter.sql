-- AUTO_INCREMENT of a PRIMARY KEY by stoolap's id counter: kept across
-- DDL, raised and lowered by ALTER as InnoDB does (never below the
-- largest id), carried by a copying ALTER, reset by TRUNCATE.
CREATE TABLE t (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, a INT, KEY ka (a)) ENGINE={E};
INSERT INTO t (a) VALUES (1), (2), (3), (4);
SELECT LAST_INSERT_ID();
DELETE FROM t WHERE id >= 3;
SELECT AUTO_INCREMENT FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 't';
-- In place: an index, a column
ALTER TABLE t ADD KEY ka2 (a, id);
ALTER TABLE t ADD COLUMN b INT;
INSERT INTO t (a) VALUES (5);
-- Copying: a column's type
ALTER TABLE t MODIFY a BIGINT;
INSERT INTO t (a) VALUES (6);
SELECT id, a FROM t ORDER BY id;
-- Raised past the largest id
ALTER TABLE t AUTO_INCREMENT = 50;
INSERT INTO t (a) VALUES (7);
-- Lowered: below the deleted ids, above the largest
DELETE FROM t WHERE id = 50;
ALTER TABLE t AUTO_INCREMENT = 10;
INSERT INTO t (a) VALUES (8);
-- Lowered below the largest: the largest plus one
ALTER TABLE t AUTO_INCREMENT = 2;
INSERT INTO t (a) VALUES (9);
SELECT id, a FROM t ORDER BY id;
SELECT AUTO_INCREMENT FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 't';
-- An explicit id past the counter moves it; a rolled-back insert's id stays taken
INSERT INTO t (id, a) VALUES (30, 10);
INSERT INTO t (a) VALUES (11);
BEGIN;
INSERT INTO t (a) VALUES (12);
ROLLBACK;
INSERT INTO t (a) VALUES (13);
SELECT id, a FROM t ORDER BY id;
-- Kept by RENAME
RENAME TABLE t TO t2;
INSERT INTO t2 (a) VALUES (14);
SELECT MAX(id) FROM t2;
-- Reset by TRUNCATE
TRUNCATE TABLE t2;
INSERT INTO t2 (a) VALUES (15);
SELECT id, a FROM t2 ORDER BY id;
-- CREATE ... AUTO_INCREMENT = n, a multi-row INSERT, INSERT ... SELECT
CREATE TABLE c (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, a INT) ENGINE={E} AUTO_INCREMENT = 1000;
INSERT INTO c (a) VALUES (1), (2), (3);
INSERT INTO c (a) SELECT a FROM c;
SELECT id, a FROM c ORDER BY id;
-- auto_increment_increment and _offset
SET SESSION auto_increment_increment = 5, auto_increment_offset = 3;
INSERT INTO c (a) VALUES (4), (5);
SET SESSION auto_increment_increment = 1, auto_increment_offset = 1;
INSERT INTO c (a) VALUES (6);
SELECT id, a FROM c ORDER BY id;
-- An explicit id earlier in a multi-row INSERT: the rows after it go past it
CREATE TABLE m (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, a INT) ENGINE={E};
INSERT INTO m (id) VALUES (1), (NULL);
INSERT INTO m (id) VALUES (100), (NULL), (50), (NULL);
SELECT id FROM m ORDER BY id;
-- AUTO_INCREMENT = 0: the largest id plus one
DELETE FROM m WHERE id > 50;
ALTER TABLE m AUTO_INCREMENT = 0;
INSERT INTO m (a) VALUES (1);
SELECT id FROM m ORDER BY id;
-- A composite PRIMARY KEY's AUTO_INCREMENT part
CREATE TABLE k (id INT NOT NULL AUTO_INCREMENT, g INT NOT NULL, PRIMARY KEY (id, g)) ENGINE={E};
INSERT INTO k VALUES (100, 1);
INSERT INTO k (g) VALUES (2), (3);
SELECT id, g FROM k ORDER BY id;
-- A row refused as a duplicate raises the counter to its id, as InnoDB's
CREATE TABLE d (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, u INT, UNIQUE KEY (u)) ENGINE={E};
INSERT INTO d (u) VALUES (1);
INSERT IGNORE INTO d (id, u) SELECT 100, 1;
INSERT INTO d (u) VALUES (2);
SELECT id, u FROM d ORDER BY id;
-- The last id: MariaDB estimates two rows, one takes it
CREATE TABLE x (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY) ENGINE={E} AUTO_INCREMENT = 9223372036854775807;
INSERT INTO x VALUES (NULL), (-1);
SELECT id FROM x ORDER BY id;
-- REPLACE and ON DUPLICATE KEY UPDATE of an explicit id past it
INSERT INTO d (id, u) VALUES (200, 1) ON DUPLICATE KEY UPDATE u = 10;
INSERT INTO d (u) VALUES (3);
REPLACE INTO d (id, u) VALUES (300, 3);
INSERT INTO d (u) VALUES (4);
SELECT id, u FROM d ORDER BY id;
