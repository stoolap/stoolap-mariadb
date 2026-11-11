-- known-diff-file: deleting a row whose FOREIGN KEY names that row itself, or moving its key with the value naming it: InnoDB refuses it (1451), Stoolap does it, as it leaves no row naming a missing one
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES t (id)) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 1), (3, 3);
DELETE FROM t WHERE id = 2;
DELETE FROM t WHERE id = 1;
CREATE TABLE r (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES r (id) ON UPDATE RESTRICT) ENGINE={E};
INSERT INTO r VALUES (1, 1);
UPDATE r SET id = 2, pid = 2 WHERE id = 1;
SELECT id, pid FROM t ORDER BY id;
SELECT id, pid FROM r ORDER BY id;
