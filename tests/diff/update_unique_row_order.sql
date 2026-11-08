-- An UPDATE of a PRIMARY KEY or UNIQUE column is checked row by row, as
-- MariaDB writes it: u = u + 10 fails where the next row holds the new
-- value, and inside a transaction the duplicate fails the statement,
-- not the COMMIT.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, u INT, UNIQUE KEY tu (u)) ENGINE={E};
INSERT INTO t VALUES (1, 10), (2, 20), (3, 30);
UPDATE t SET u = u + 10 WHERE id <= 2;
SELECT id, u FROM t ORDER BY id;
UPDATE t SET u = u + 10 WHERE id >= 2;
SELECT id, u FROM t ORDER BY id;
UPDATE t SET u = 20 WHERE id <= 2;
UPDATE t SET id = id + 1;
SELECT id, u FROM t ORDER BY id;
BEGIN;
UPDATE t SET u = 20 WHERE id <= 2;
SELECT id, u FROM t ORDER BY id;
COMMIT;
SELECT id, u FROM t ORDER BY id;
