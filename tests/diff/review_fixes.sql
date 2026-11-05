-- Fixes from a review of the whole plugin.
-- UPDATE / DELETE ... ORDER BY read an empty table through index_first,
-- whose end is not an error.
CREATE TABLE e (id INT NOT NULL PRIMARY KEY, n INT, KEY kn (n)) ENGINE={E};
DELETE FROM e ORDER BY id LIMIT 2;
UPDATE e SET n = n + 1 ORDER BY id LIMIT 3;
DELETE FROM e ORDER BY id DESC LIMIT 1;
UPDATE e FORCE INDEX (kn) SET n = 1 ORDER BY n LIMIT 1;
SELECT MIN(id), MAX(n) FROM e;
-- A decimal literal past a double's precision is not pushed: stoolap
-- reads it as a double. Within it, it pushes and comes back exact.
INSERT INTO e VALUES (1, 2);
SELECT 12345678901234567890.5, 123456789012345.5, 0.000000000000000123456789, n + 0.25 FROM e;
