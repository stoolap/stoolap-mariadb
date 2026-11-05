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

-- A row moved to a new PRIMARY KEY keeps UNIQUE per row: IGNORE skips
-- the second row's duplicate, its own unchanged key is no conflict, and
-- a taken id is a duplicate of the key.
CREATE TABLE t4 (id INT NOT NULL PRIMARY KEY, u INT, UNIQUE KEY uu (u)) ENGINE={E};
INSERT INTO t4 VALUES (1, 1), (2, 2);
UPDATE IGNORE t4 SET id = id + 10, u = 7 ORDER BY id LIMIT 2;
SELECT id, u FROM t4 ORDER BY id;
UPDATE t4 SET id = id + 100 ORDER BY id LIMIT 1;
SELECT id, u FROM t4 ORDER BY id;
UPDATE t4 SET id = 11 ORDER BY id DESC LIMIT 1;
SELECT id, u FROM t4 ORDER BY id;

-- A read in index order goes on after the rows it gave when the
-- statement's own writes stop the index from answering: no row skipped,
-- none given twice.
CREATE TABLE t5 (id INT NOT NULL PRIMARY KEY, k INT, v INT, KEY kk (k)) ENGINE={E};
INSERT INTO t5 WITH RECURSIVE s (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 40)
  SELECT n, 7, 0 FROM s;
DELETE FROM t5 ORDER BY id LIMIT 20;
SELECT MIN(id), MAX(id), COUNT(*) FROM t5;
UPDATE t5 FORCE INDEX (kk) SET v = v + 1 ORDER BY k LIMIT 18;
SELECT COUNT(*), SUM(v), MAX(v) FROM t5;
DELETE FROM t5 ORDER BY id DESC LIMIT 17;
SELECT MIN(id), MAX(id), COUNT(*) FROM t5;

-- A row moved to a new PRIMARY KEY is not read again: the secondary index
-- MariaDB scans does not hold the key, so it does not buffer the rows.
CREATE TABLE t6 (id BIGINT NOT NULL PRIMARY KEY, a BIGINT UNSIGNED NOT NULL,
                 KEY ka (a)) ENGINE={E};
INSERT INTO t6 WITH RECURSIVE s (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 200)
  SELECT n, 1 FROM s;
UPDATE t6 FORCE INDEX (ka) SET id = id + 100000 WHERE a < 9223372036854775808 LIMIT 1000;
SELECT MIN(id), MAX(id), COUNT(*) FROM t6;
UPDATE t6 FORCE INDEX (ka) SET id = id + 100000 WHERE a = 1 LIMIT 1000;
SELECT MIN(id), MAX(id), COUNT(*) FROM t6;
UPDATE t6 SET id = id + 100000 ORDER BY id DESC LIMIT 150;
SELECT MIN(id), MAX(id), COUNT(*) FROM t6;
