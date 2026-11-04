-- UNION / INTERSECT / EXCEPT and derived tables through the SQL
-- generator: whole statements (PUSHED UNION), set operations inside
-- subqueries and derived tables, and derived tables joined to InnoDB
-- (PUSHED DERIVED).
CREATE TABLE a (id INT NOT NULL PRIMARY KEY, x INT, f DOUBLE,
                s VARCHAR(10) COLLATE utf8mb4_bin,
                c VARCHAR(10) COLLATE utf8mb4_general_ci) ENGINE={E};
CREATE TABLE b (id INT NOT NULL PRIMARY KEY, x INT, f DOUBLE,
                s VARCHAR(10) COLLATE utf8mb4_bin,
                c VARCHAR(10) COLLATE utf8mb4_general_ci) ENGINE={E};
INSERT INTO a VALUES (1, 1, 1.5, 'p', 'Q'), (2, 1, 2.5, 'q', 'q'),
                     (3, 2, NULL, NULL, 'r'), (4, NULL, 3.0, 'r', NULL);
INSERT INTO b VALUES (1, 1, 1.0, 'p', 'q'), (2, 3, 2.5, 'z', 'R'),
                     (3, NULL, NULL, NULL, 'x'), (4, 2, 7.0, 'q', NULL);
CREATE TABLE k (id INT NOT NULL PRIMARY KEY, label VARCHAR(10)) ENGINE=InnoDB;
INSERT INTO k VALUES (1, 'one'), (2, 'two'), (3, 'three');

-- Set operations, with the result's ORDER BY / LIMIT (NULLs first ASC).
SELECT x FROM a UNION SELECT x FROM b ORDER BY x;
SELECT x FROM a UNION ALL SELECT x FROM b ORDER BY x DESC LIMIT 4 OFFSET 1;
SELECT x, s FROM a UNION SELECT x, s FROM b ORDER BY x, s;
SELECT x FROM a INTERSECT SELECT x FROM b ORDER BY x;
SELECT x FROM a EXCEPT SELECT x FROM b ORDER BY x;
SELECT x FROM a INTERSECT ALL SELECT x FROM b ORDER BY x;
SELECT x FROM a EXCEPT ALL SELECT x FROM b ORDER BY x;
SELECT x FROM a UNION ALL SELECT x FROM b UNION SELECT 9 FROM a ORDER BY x;
SELECT x FROM a UNION SELECT x FROM b UNION ALL SELECT x FROM a ORDER BY x;
SELECT x FROM a UNION SELECT x FROM b INTERSECT SELECT 3 FROM a ORDER BY x;
SELECT x FROM a EXCEPT SELECT x FROM b UNION SELECT 7 FROM b ORDER BY x;
-- Result types: an integer leg widens to DOUBLE; a NULL leg fits any.
SELECT x FROM a UNION SELECT f FROM b ORDER BY 1;
SELECT x / 4e0 FROM a UNION ALL SELECT f FROM b ORDER BY 1;
SELECT s FROM a UNION ALL SELECT NULL FROM b ORDER BY 1;
-- Left to MariaDB: ci dedup, mixed families, DECIMAL, a leg's own LIMIT.
-- ('Q' and 'q' are one value under the ci collation: which spelling a
-- dedup keeps, or how ORDER BY places them, depends on the read order,
-- so these compare counts or unordered rows.)
SELECT COUNT(*) FROM (SELECT c FROM a UNION SELECT c FROM b) d;
SELECT c FROM a UNION ALL SELECT c FROM b;
SELECT x FROM a UNION SELECT s FROM b ORDER BY 1;
SELECT x FROM a UNION SELECT 0.5 FROM b ORDER BY 1;
(SELECT x FROM a ORDER BY x DESC LIMIT 1) UNION (SELECT x FROM b ORDER BY x LIMIT 1) ORDER BY 1;
SELECT x FROM a UNION SELECT x FROM b ORDER BY x LIMIT 2 OFFSET 1;
-- Set operations inside a pushed SELECT.
SELECT id FROM a WHERE x IN (SELECT x FROM b UNION SELECT 2 FROM b) ORDER BY id;
SELECT id FROM a WHERE x NOT IN (SELECT x FROM b WHERE x IS NOT NULL UNION ALL SELECT 3 FROM b) ORDER BY id;
SELECT id FROM a WHERE EXISTS (SELECT x FROM b WHERE b.x = a.x INTERSECT SELECT 1 FROM b) ORDER BY id;
SELECT id, (SELECT MAX(x) FROM b WHERE b.id <= a.id) FROM a ORDER BY id;
SELECT d.x, COUNT(*) FROM (SELECT x FROM a UNION ALL SELECT x FROM b) d GROUP BY d.x ORDER BY d.x;
WITH u AS (SELECT x, s FROM a UNION SELECT x, s FROM b) SELECT x, s FROM u WHERE x > 1 ORDER BY x, s;
-- Derived tables joined to InnoDB.
SELECT k.label, d.n FROM k JOIN (SELECT x, COUNT(*) AS n FROM a GROUP BY x) d ON d.x = k.id ORDER BY k.id;
SELECT k.label, d.x FROM k JOIN (SELECT x FROM a UNION SELECT x FROM b) d ON d.x = k.id ORDER BY k.id;
SELECT k.label, d.x FROM k JOIN (SELECT x FROM a INTERSECT SELECT x FROM b) d ON d.x = k.id ORDER BY k.id;
SELECT k.label, d.x FROM k JOIN (SELECT x FROM a GROUP BY x LIMIT 2) d ON d.x = k.id ORDER BY k.id;
SELECT k.label, d.x FROM k JOIN (SELECT x FROM a UNION ALL SELECT x FROM b ORDER BY x DESC LIMIT 3) d ON d.x = k.id ORDER BY k.id, d.x;
SELECT k.label, COUNT(*) FROM k JOIN (SELECT id, c FROM a UNION SELECT id, c FROM b) d ON d.id = k.id GROUP BY k.label;
SELECT k.label, d.v FROM k JOIN (SELECT id, CONCAT(s, '!') AS v FROM a) d ON d.id = k.id ORDER BY k.id;
SELECT k.id FROM k WHERE k.id IN (SELECT x FROM a UNION SELECT x FROM b) ORDER BY k.id;
-- A DISTINCT leg dedups its own values before the result's DOUBLE type
-- converts them: 2^53 and 2^53 + 1 stay two rows.
CREATE TABLE big (id INT NOT NULL PRIMARY KEY, v BIGINT) ENGINE={E};
INSERT INTO big VALUES (1, 9007199254740992), (2, 9007199254740993), (3, 9007199254740993);
SELECT COUNT(*) FROM (SELECT DISTINCT v FROM big UNION ALL SELECT f FROM b WHERE id = 1) d;
SELECT DISTINCT v FROM big UNION ALL SELECT f FROM b WHERE id = 1 ORDER BY 1;
SELECT v FROM big GROUP BY v UNION ALL SELECT f FROM b WHERE id = 1 ORDER BY 1;
SELECT COUNT(*) FROM (SELECT v FROM big GROUP BY v UNION ALL SELECT f FROM b WHERE id = 1) d;
