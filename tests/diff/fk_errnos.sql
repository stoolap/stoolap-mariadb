CREATE TABLE p (id INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, pid INT NOT NULL,
                FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
INSERT INTO p VALUES (1), (2), (3);
INSERT INTO c VALUES (10, 2);
-- child side: referenced row missing
INSERT INTO c VALUES (11, 99);
UPDATE c SET pid = 99 WHERE id = 10;
-- parent side: row still referenced, direct and row-pump
DELETE FROM p WHERE id = 2;
DELETE FROM p WHERE id = 2 LIMIT 1;
-- known-diff: stoolap refuses UPDATE of a single-column PRIMARY KEY (see pk_update.sql)
UPDATE p SET id = 20 WHERE id = 2;
DELETE FROM p WHERE id = 3;
SELECT id FROM p ORDER BY id;
