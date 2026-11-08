-- known-diff-file: a primary key move whose children follow it (ON UPDATE CASCADE / SET NULL) is refused until stoolap's UPDATE moves a key (stoolap#952)
CREATE TABLE p (id INT NOT NULL PRIMARY KEY) ENGINE={E};
CREATE TABLE cc (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (pid) REFERENCES p (id) ON UPDATE CASCADE) ENGINE={E};
CREATE TABLE cn (id INT NOT NULL PRIMARY KEY, pid INT,
                 FOREIGN KEY (pid) REFERENCES p (id) ON UPDATE SET NULL) ENGINE={E};
INSERT INTO p VALUES (1), (2), (3);
INSERT INTO cc VALUES (20, 1);
INSERT INTO cn VALUES (30, 2);
UPDATE p SET id = 10 WHERE id = 1;
UPDATE p SET id = 20 WHERE id = 2;
-- a child whose own key is the foreign key, and a child naming it twice
CREATE TABLE rx (id INT NOT NULL PRIMARY KEY,
                 FOREIGN KEY (id) REFERENCES p (id) ON UPDATE CASCADE) ENGINE={E};
CREATE TABLE r2 (id INT NOT NULL PRIMARY KEY, a INT, b INT,
                 FOREIGN KEY (a) REFERENCES p (id) ON UPDATE CASCADE,
                 FOREIGN KEY (b) REFERENCES p (id) ON UPDATE CASCADE) ENGINE={E};
INSERT INTO rx VALUES (3);
INSERT INTO r2 VALUES (10, 3, 3), (11, 3, NULL);
UPDATE p SET id = 300 WHERE id = 3;
SELECT id FROM p ORDER BY id;
SELECT id, pid FROM cc ORDER BY id;
SELECT id, pid FROM cn ORDER BY id;
SELECT id FROM rx ORDER BY id;
SELECT id, a, b FROM r2 ORDER BY id;
