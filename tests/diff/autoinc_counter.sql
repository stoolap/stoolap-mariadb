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
-- known-diff: lowering copies the table (stoolap's counter only rises): the copied rows count as affected
ALTER TABLE t AUTO_INCREMENT = 10;
INSERT INTO t (a) VALUES (8);
-- Lowered below the largest: the largest plus one
-- known-diff: lowering copies the table (stoolap's counter only rises): the copied rows count as affected
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
