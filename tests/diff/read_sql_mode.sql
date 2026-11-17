-- A value is read as stored, whatever the reading session's sql_mode
CREATE TABLE t (id INT PRIMARY KEY, d DATE, dt DATETIME, e ENUM('x','y'), n INT) ENGINE={E};
SET SESSION sql_mode = 'ALLOW_INVALID_DATES';
INSERT INTO t VALUES (1, '2024-02-31', '2024-02-31 10:00:00', 'x', 0);
SET SESSION sql_mode = '';
INSERT INTO t VALUES (2, '2024-00-15', '0000-00-00 00:00:00', 'bogus', 0);
SET SESSION sql_mode = DEFAULT;
SELECT id, CAST(d AS CHAR), CAST(dt AS CHAR), e FROM t ORDER BY id;
SELECT id, CAST(d AS CHAR), CAST(dt AS CHAR) FROM t WHERE n = 0 ORDER BY id;
-- A strict UPDATE through the row pump reads the rows it skips and changes
UPDATE t SET n = 1 WHERE e = '' LIMIT 1;
UPDATE t SET n = n + 1 ORDER BY id LIMIT 2;
SET SESSION sql_mode = 'TRADITIONAL';
SELECT id, CAST(d AS CHAR), CAST(dt AS CHAR), e, n FROM t ORDER BY id;
UPDATE t SET n = 5 WHERE id = 1 LIMIT 1;
SET SESSION sql_mode = DEFAULT;
