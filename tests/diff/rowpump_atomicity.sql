-- Autocommit row-pump DML is one statement transaction: a failing row
-- leaves nothing behind.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, u INT NOT NULL,
                UNIQUE KEY uu (u)) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 3), (3, 6);
UPDATE t SET u = IF(id = 3, 11, u + 10) ORDER BY id LIMIT 3;
SELECT id, u FROM t ORDER BY id;
