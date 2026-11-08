-- A failed statement inside a transaction is undone as a whole, and the
-- transaction goes on: its earlier statements commit, a user's savepoints
-- still work. Each write path: direct inserts, direct UPDATE / DELETE,
-- the row pump (ORDER BY ... LIMIT), and SQL writes for FOREIGN KEY tables.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, u INT, n TINYINT, UNIQUE KEY tu (u)) ENGINE={E};
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id)) ENGINE={E};
INSERT INTO t VALUES (1, 10, 1), (2, 20, 2), (3, 30, 120);
INSERT INTO p VALUES (1), (2);
INSERT INTO c VALUES (1, 1);
BEGIN;
INSERT INTO t VALUES (4, 40, 4);
INSERT INTO t VALUES (5, 50, 5), (6, 10, 6);
UPDATE t SET u = u + 10 WHERE id <= 2;
UPDATE t SET n = n + 10;
UPDATE t SET u = 99 ORDER BY id DESC LIMIT 2;
UPDATE t SET u = 30 ORDER BY id LIMIT 2;
INSERT INTO c VALUES (2, 2), (3, 7);
DELETE FROM p;
SAVEPOINT a;
INSERT INTO t VALUES (7, 70, 7);
INSERT INTO t VALUES (8, 80, 8), (9, 70, 9);
ROLLBACK TO SAVEPOINT a;
INSERT INTO t VALUES (10, 100, 10);
RELEASE SAVEPOINT a;
COMMIT;
SELECT id, u, n FROM t ORDER BY id;
SELECT id, pid FROM c ORDER BY id;
SELECT id FROM p ORDER BY id;
