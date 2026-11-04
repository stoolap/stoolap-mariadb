-- known-diff-file: tx: LOCK TABLES with autocommit=0 runs statements after COMMIT in autocommit
CREATE TABLE t (id INT NOT NULL PRIMARY KEY) ENGINE={E};
SET autocommit = 0;
LOCK TABLES t WRITE;
INSERT INTO t VALUES (1);
COMMIT;
INSERT INTO t VALUES (2);
ROLLBACK;
UNLOCK TABLES;
SET autocommit = 1;
SELECT id FROM t ORDER BY id;
