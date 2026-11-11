-- known-diff-file: an ON UPDATE CASCADE / SET NULL that comes back into its own table: InnoDB refuses it (1451), Stoolap runs it, as PostgreSQL does
CREATE TABLE uc (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (pid) REFERENCES uc (id) ON UPDATE CASCADE) ENGINE={E};
CREATE TABLE un (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (pid) REFERENCES un (id) ON UPDATE SET NULL) ENGINE={E};
INSERT INTO uc VALUES (1, NULL), (2, 1);
INSERT INTO un VALUES (1, NULL), (2, 1);
UPDATE uc SET id = 10 WHERE id = 1;
UPDATE un SET id = 10 WHERE id = 1;
SELECT id, pid FROM uc ORDER BY id;
SELECT id, pid FROM un ORDER BY id;
