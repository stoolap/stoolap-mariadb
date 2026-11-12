-- known-diff-file: a FOREIGN KEY on a case-insensitive UNIQUE column with ON UPDATE CASCADE is refused (1296): stoolap's actions would write the column without its hidden collation key
CREATE TABLE p (id INT NOT NULL PRIMARY KEY, code VARCHAR(10) COLLATE utf8mb4_bin UNIQUE) ENGINE={E};
INSERT INTO p VALUES (1, 'a');
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, pc VARCHAR(10) UNIQUE,
                FOREIGN KEY (pc) REFERENCES p (code) ON UPDATE CASCADE) ENGINE={E};
INSERT INTO c VALUES (1, 'a');
UPDATE p SET code = 'b' WHERE id = 1;
SELECT id, pc FROM c ORDER BY id;
