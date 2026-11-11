-- known-diff-file: deleting a row whose FOREIGN KEY names that row itself: InnoDB refuses it (1451), Stoolap deletes it, as it leaves no row naming a missing one
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES t (id)) ENGINE={E};
INSERT INTO t VALUES (1, 1), (2, 1);
DELETE FROM t WHERE id = 2;
DELETE FROM t WHERE id = 1;
SELECT id, pid FROM t ORDER BY id;
