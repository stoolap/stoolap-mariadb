-- Autocommit row-pump DML commits per row (stoolap/stoolap#884).
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, u INT NOT NULL,
                UNIQUE KEY uu (u)) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 3), (3, 6);
UPDATE t SET u = IF(id = 3, 11, u + 10) ORDER BY id LIMIT 3;
-- known-diff: stoolap/stoolap#884, rows before the failing one stay changed
SELECT id, u FROM t ORDER BY id;
-- known-diff-final: stoolap/stoolap#884, the partial UPDATE stays committed
