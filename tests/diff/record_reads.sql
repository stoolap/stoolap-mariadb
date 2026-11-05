-- Handler reads write each row straight into the record: integers of
-- every width and sign at their limits, FLOAT / DOUBLE plain and with
-- fixed decimals, NULL in each, read by a mixed-engine join (the handler
-- path), an index range and the row pump. (BIGINT UNSIGNED stops at
-- INT64_MAX: the plugin refuses larger values.)
CREATE TABLE r (id INT NOT NULL PRIMARY KEY,
  t TINYINT, tu TINYINT UNSIGNED, s SMALLINT, su SMALLINT UNSIGNED,
  m MEDIUMINT, mu MEDIUMINT UNSIGNED, i INT, iu INT UNSIGNED,
  b BIGINT, bu BIGINT UNSIGNED, bo BOOL,
  f FLOAT, d DOUBLE, fd FLOAT(7,2), dd DOUBLE(10,3),
  v VARCHAR(10), KEY ki (i)) ENGINE={E} DEFAULT CHARSET=utf8mb4;
INSERT INTO r VALUES
  (1, -128, 255, -32768, 65535, -8388608, 16777215, -2147483648, 4294967295,
   -9223372036854775808, 9223372036854775807, 1, -1.5, -2.25e300, 12345.67, -1234567.891, 'a'),
  (2, 127, 0, 32767, 0, 8388607, 0, 2147483647, 0,
   9223372036854775807, 0, 0, 3.4e38, 1.7976931348623157e308, -0.01, 0.001, ''),
  (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
  (4, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, NULL, 0.1, 0.1, 0.1, 0.1, 'zzzzzzzzzz');
CREATE TABLE k (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO k VALUES (1), (2), (3), (4);
SELECT STRAIGHT_JOIN r.* FROM k JOIN r ON r.id = k.id ORDER BY k.id;
SELECT id, t, iu, bu, f, d FROM r FORCE INDEX (ki) WHERE i >= -2147483648 ORDER BY i;
SELECT STRAIGHT_JOIN k.id, r.fd + 0, r.dd + 0, r.bo FROM k JOIN r ON r.id = k.id ORDER BY k.id;
UPDATE r SET v = CONCAT(v, '!') WHERE v IS NOT NULL ORDER BY id LIMIT 3;
UPDATE r SET bu = bu - 1, iu = iu + 0 WHERE id = 1 ORDER BY id LIMIT 1;
SELECT STRAIGHT_JOIN r.id, r.bu, r.iu, r.v FROM k JOIN r ON r.id = k.id ORDER BY k.id;

-- The same rows through pushdown: the whole SELECT, expressions whose
-- result fields MariaDB types itself, and a derived table joined to
-- InnoDB.
SELECT * FROM r ORDER BY id;
SELECT id, t + 1, iu * 2, b DIV 2, f * 2, d / 2, CONCAT(v, id), LENGTH(v), bo AND 1 FROM r ORDER BY id;
SELECT STRAIGHT_JOIN k.id, x.t, x.bu, x.d, x.v FROM k JOIN (SELECT id, t, bu, d, v FROM r WHERE id > 0) x ON x.id = k.id ORDER BY k.id;
