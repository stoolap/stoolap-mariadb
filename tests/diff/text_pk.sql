-- Stoolap accepts only an INTEGER single-column PRIMARY KEY ("Only
-- INTEGER PRIMARY KEY is supported"); a composite PRIMARY KEY may hold
-- any type.
-- known-diff-final: tables with a non-INTEGER single-column PRIMARY KEY are not created
-- known-diff: stoolap refuses a non-INTEGER single-column PRIMARY KEY
CREATE TABLE s (id VARCHAR(10) COLLATE utf8mb4_bin NOT NULL PRIMARY KEY, n INT) ENGINE={E};
-- known-diff: table s was not created
INSERT INTO s VALUES ('a', 1), ('b', 2);
-- known-diff: table s was not created
UPDATE s SET id = 'c' WHERE id = 'a';
-- known-diff: table s was not created
SELECT id, n FROM s ORDER BY id;
-- A DATE is stored as an INTEGER, so it can be the PRIMARY KEY.
CREATE TABLE dpk (d DATE NOT NULL PRIMARY KEY) ENGINE={E};
INSERT INTO dpk VALUES ('2024-02-29'), ('0000-00-00'), ('1999-12-31');
SELECT d FROM dpk ORDER BY d;
SELECT d FROM dpk WHERE d >= '1999-12-31' ORDER BY d DESC;
CREATE TABLE cpk (id VARCHAR(10) COLLATE utf8mb4_bin NOT NULL, x INT NOT NULL,
                  PRIMARY KEY (id, x)) ENGINE={E};
INSERT INTO cpk VALUES ('a', 1), ('a', 2);
SELECT id, x FROM cpk ORDER BY id, x;
