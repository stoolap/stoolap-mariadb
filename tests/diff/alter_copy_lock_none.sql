-- known-diff-file: a copying ALTER TABLE does not run online: LOCK=NONE is refused (1846), and without it writes wait for the ALTER, as MariaDB replays an online ALTER's concurrent writes from row events a direct UPDATE / DELETE does not write
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, v INT) ENGINE={E};
INSERT INTO t VALUES (1, 1);
ALTER TABLE t FORCE, ALGORITHM=COPY, LOCK=NONE;
SELECT id, v FROM t;
