-- known-diff-file: a FOREIGN KEY to a case-insensitive UNIQUE column is refused (1296): stoolap keeps that key on the hidden collation key column, so it finds no UNIQUE index on the column the key names
CREATE TABLE p (id INT NOT NULL PRIMARY KEY, code VARCHAR(10) UNIQUE) ENGINE={E};
INSERT INTO p VALUES (1, 'abc');
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, pc VARCHAR(10),
                FOREIGN KEY (pc) REFERENCES p (code)) ENGINE={E};
INSERT INTO c VALUES (1, 'abc');
SELECT id, pc FROM c ORDER BY id;
