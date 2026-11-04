-- Direct reads and writes name rows by stoolap's row id, and the
-- statement's own transaction holds its writes until it ends.
-- An ordered read keeps each row's id: the update hits the row it read,
-- not row 0.
CREATE TABLE t1 (id INT NOT NULL PRIMARY KEY, a INT, b INT, v INT,
                 KEY kab (a, b)) ENGINE={E};
INSERT INTO t1 VALUES (0, 9, 9, 0), (1, 1, 2, 10), (2, 1, 1, 20), (3, 2, 1, 30);
UPDATE t1 FORCE INDEX (kab) SET v = v + 1000 WHERE a = 1 ORDER BY b DESC LIMIT 1;
DELETE FROM t1 WHERE a = 1 ORDER BY b LIMIT 1;
SELECT id, a, b, v FROM t1 ORDER BY id;

-- A duplicate of a row the same statement wrote is the row's own error:
-- IGNORE skips it, ON DUPLICATE KEY UPDATE and REPLACE act on it.
CREATE TABLE t2 (id INT NOT NULL PRIMARY KEY, u INT, UNIQUE KEY uu (u)) ENGINE={E};
INSERT IGNORE INTO t2 VALUES (1, 7), (2, 7), (3, 8);
SELECT id, u FROM t2 ORDER BY id;
INSERT INTO t2 VALUES (4, 9), (5, 9) ON DUPLICATE KEY UPDATE id = id + 10;
SELECT id, u FROM t2 ORDER BY id;
REPLACE INTO t2 VALUES (6, 10), (7, 10);
SELECT id, u FROM t2 ORDER BY id;
INSERT INTO t2 VALUES (8, 11), (9, 11);

-- SQL reads in the statement see its direct writes.
CREATE TABLE t3 (id INT NOT NULL PRIMARY KEY) ENGINE={E};
INSERT INTO t3 VALUES (1);
CREATE TRIGGER t3ai AFTER INSERT ON t3 FOR EACH ROW
  SET @seen = (SELECT MAX(id) FROM t3);
INSERT INTO t3 VALUES (2);
SELECT @seen;
