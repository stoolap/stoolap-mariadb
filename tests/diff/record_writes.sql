-- Direct writes read the record straight: integers of every width and
-- sign at their limits, FLOAT / DOUBLE, utf8mb4 VARCHAR, NULLs, and the
-- kinds the encode callback gives (DECIMAL, DATE, ENUM, TEXT), by single
-- and multi-row INSERT, INSERT ... SELECT and the row pump's UPDATE.
-- (BIGINT UNSIGNED stops at INT64_MAX: the plugin refuses larger values.)
CREATE TABLE w (id INT NOT NULL PRIMARY KEY,
  t TINYINT, tu TINYINT UNSIGNED, s SMALLINT, su SMALLINT UNSIGNED,
  m MEDIUMINT, mu MEDIUMINT UNSIGNED, i INT, iu INT UNSIGNED,
  b BIGINT, bu BIGINT UNSIGNED, f FLOAT, d DOUBLE, v VARCHAR(5),
  dc DECIMAL(10,3), dt DATE, e ENUM('x','y','z'), tx TEXT,
  KEY kv (v)) ENGINE={E} DEFAULT CHARSET=utf8mb4;
INSERT INTO w VALUES (1, -128, 255, -32768, 65535, -8388608, 16777215,
  -2147483648, 4294967295, -9223372036854775808, 9223372036854775807,
  -1.5, -2.25e300, 'çğü😀', -1234567.891, '2024-02-29', 'z', 'long text');
INSERT INTO w VALUES
  (2, 127, 0, 32767, 0, 8388607, 0, 2147483647, 0, 9223372036854775807, 0,
   3.4e38, 1.7976931348623157e308, '', 0.001, '1000-01-01', 'x', ''),
  (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
   NULL, NULL, NULL, NULL, NULL, NULL, NULL),
  (4, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0.1, 0.1, 'abcde', 1.5, '2000-01-01', 'y', 'b');
INSERT INTO w (id, v) SELECT id + 10, CONCAT(LEFT(v, 2), '!') FROM w WHERE id <= 4;
SELECT * FROM w ORDER BY id;
UPDATE w SET t = t - 1, iu = iu - 1, d = -d, v = 'ab😀', dc = dc * 2,
  dt = '2001-01-01', e = 'y', tx = CONCAT(tx, '+') WHERE id IN (1, 4) ORDER BY id LIMIT 2;
UPDATE w SET v = NULL, f = NULL WHERE id = 2 ORDER BY id LIMIT 1;
UPDATE w SET i = i WHERE id = 4 ORDER BY id LIMIT 1;
UPDATE w SET id = id + 100 WHERE id = 14 ORDER BY id LIMIT 1;
SELECT * FROM w ORDER BY id;
SELECT id, v FROM w FORCE INDEX (kv) WHERE v >= '' ORDER BY v, id;
