-- known-diff-file: a copying ALTER of a table another table's FOREIGN KEY names is refused (1296): MariaDB swaps the copy in by renames, and a rename takes the keys to the old table (stoolap#987)
CREATE TABLE p (id INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
INSERT INTO p VALUES (1);
INSERT INTO c VALUES (1, 1);
ALTER TABLE p ADD COLUMN w INT, ALGORITHM=COPY;
INSERT INTO p (id) VALUES (2);
INSERT INTO c VALUES (2, 2);
SELECT * FROM p ORDER BY id;
