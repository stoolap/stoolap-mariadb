-- Writes to tables a foreign key names go through SQL, from the values
-- Rust reads in the record: single and multi-row INSERT (a multi-row one
-- lands whole or not at all), an INSERT ... SELECT past the 50000-row
-- flush, whose chunks share one transaction, and the row pump's UPDATE
-- and DELETE.
SET SESSION max_recursive_iterations = 100000;
CREATE TABLE fp (id INT NOT NULL PRIMARY KEY, n VARCHAR(10)) ENGINE={E};
CREATE TABLE fc (id INT NOT NULL PRIMARY KEY, pid INT NOT NULL, v VARCHAR(10),
                 d DATE, dc DECIMAL(6,2),
                 FOREIGN KEY (pid) REFERENCES fp (id)) ENGINE={E};
INSERT INTO fp VALUES (1, 'a'), (2, 'b'), (3, 'c');
INSERT INTO fc VALUES (10, 1, 'x', '2024-01-02', 1.25);
INSERT INTO fc VALUES (11, 2, 'y', NULL, NULL), (12, 99, 'z', NULL, NULL);
SELECT COUNT(*) FROM fc;
INSERT INTO fc VALUES (11, 2, 'y', '2000-01-01', -3.5), (12, 3, 'z😀', NULL, 0);
INSERT INTO fc (id, pid, v)
  WITH RECURSIVE s (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 60001)
  SELECT 1000 + n, IF(n = 60001, 99, 1 + n % 3), 'r' FROM s;
SELECT COUNT(*) FROM fc;
INSERT INTO fc (id, pid, v)
  WITH RECURSIVE s (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 60000)
  SELECT 1000 + n, 1 + n % 3, 'r' FROM s;
SELECT COUNT(*), SUM(pid) FROM fc;
UPDATE fc SET v = CONCAT(v, '!'), dc = dc + 1 WHERE id < 100 ORDER BY id LIMIT 3;
UPDATE fc SET pid = 99 WHERE id = 10 ORDER BY id LIMIT 1;
UPDATE fc SET pid = 3 WHERE id = 10 ORDER BY id LIMIT 1;
UPDATE fp SET n = 'bb' WHERE id = 2 ORDER BY id LIMIT 1;
DELETE FROM fp WHERE id = 1 ORDER BY id LIMIT 1;
DELETE FROM fc WHERE id >= 1000 ORDER BY id LIMIT 5;
DELETE FROM fc WHERE id = 11 ORDER BY id LIMIT 1;
SELECT id, pid, v, d, dc FROM fc WHERE id < 1010 ORDER BY id;
SELECT COUNT(*) FROM fc;
SELECT id, n FROM fp ORDER BY id;
