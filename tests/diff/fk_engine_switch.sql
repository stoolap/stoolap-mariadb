-- A table a FOREIGN KEY names keeps its engine, as a parent and as a
-- child (1217, as InnoDB refuses it): the other engine's copy would leave
-- the keys with the old table. A table no key names moves.
CREATE TABLE p (id INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT NOT NULL PRIMARY KEY, pid INT,
                FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
CREATE TABLE f (id INT NOT NULL PRIMARY KEY) ENGINE={E};
INSERT INTO p VALUES (1);
INSERT INTO c VALUES (1, 1);
INSERT INTO f VALUES (1);
ALTER TABLE p ENGINE=MyISAM;
ALTER TABLE c ENGINE=MyISAM;
INSERT INTO c VALUES (2, 7);
INSERT INTO p VALUES (2);
INSERT INTO c VALUES (3, 2);
DELETE FROM p WHERE id = 1;
ALTER TABLE f ENGINE=MyISAM;
SELECT id, pid FROM c ORDER BY id;
SELECT id FROM f;
