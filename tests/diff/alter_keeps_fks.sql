-- A copying ALTER of a child table keeps its FOREIGN KEYs: through ADD
-- COLUMN, CREATE INDEX, an ALTER that adds another key, and CHANGE and
-- RENAME COLUMN of the key's column, with its ON DELETE action.
-- ALGORITHM=COPY has InnoDB copy the rows too, as the plugin does; it
-- renames a key's column only in place, so that child is empty then. The
-- children sort before the parent: DROP DATABASE drops in name order, and
-- stoolap keeps a parent rows reference.
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
INSERT INTO p VALUES (1), (2), (3);
CREATE TABLE c (id INT PRIMARY KEY, pid INT, qid INT,
  FOREIGN KEY (pid) REFERENCES p (id) ON DELETE CASCADE) ENGINE={E};
INSERT INTO c VALUES (1, 1, NULL), (2, 2, NULL);
ALTER TABLE c ADD COLUMN z INT, ALGORITHM=COPY;
INSERT INTO c VALUES (3, 99, NULL, NULL);
CREATE INDEX kq ON c (qid) ALGORITHM=COPY;
INSERT INTO c VALUES (4, 99, NULL, NULL);
ALTER TABLE c ADD FOREIGN KEY (qid) REFERENCES p (id), ALGORITHM=COPY;
INSERT INTO c VALUES (5, 1, 99, NULL);
INSERT INTO c VALUES (6, 99, 1, NULL);
INSERT INTO c VALUES (7, 3, 3, NULL);
CREATE TABLE b (id INT PRIMARY KEY, pid INT,
  FOREIGN KEY (pid) REFERENCES p (id) ON DELETE CASCADE) ENGINE={E};
ALTER TABLE b CHANGE pid pid2 INT;
INSERT INTO b VALUES (1, 99);
ALTER TABLE b RENAME COLUMN pid2 TO pid3;
INSERT INTO b VALUES (2, 99);
INSERT INTO b VALUES (3, 1), (4, 2);
DELETE FROM p WHERE id = 1;
SELECT id, pid, qid FROM c ORDER BY id;
SELECT id, pid3 FROM b ORDER BY id;
