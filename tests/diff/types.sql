-- The type layer (src/stoolap_types.h): dates and times as packed
-- INTEGERs, TIMESTAMP as UTC, ENUM / SET as index / mask, binary strings
-- as hex, other character sets as utf8mb4. Values, order, index lookups
-- and ranges, pushed comparisons, and DML that must not compare them as
-- written.
SET time_zone = '+00:00';
CREATE TABLE tt (id INT NOT NULL PRIMARY KEY, d DATE, dt DATETIME(6),
                 tm TIME(3), ts TIMESTAMP(6) NULL, d2 DATE,
                 KEY kd (d), KEY kdt (dt), KEY ktm (tm), KEY kts (ts)) ENGINE={E};
INSERT INTO tt VALUES
  (1, '2024-02-29', '2024-02-29 23:59:59.999999', '838:59:59.999', '2024-02-29 12:00:00.5', '2024-02-29'),
  (2, '1000-01-01', '1000-01-01 00:00:00', '-838:59:59', '1970-01-01 00:00:01', '1000-01-02'),
  (3, '9999-12-31', '9999-12-31 23:59:59.999999', '00:00:00', '2038-01-19 03:14:07.999999', '9999-12-31'),
  (4, NULL, NULL, NULL, NULL, NULL),
  (5, '2024-03-01', '2024-03-01 00:00:00', '-00:00:01', '2024-03-01 00:00:00', '2024-02-29');
SELECT id, d, dt, tm, ts FROM tt ORDER BY id;
SELECT id FROM tt ORDER BY d, id;
SELECT id FROM tt ORDER BY dt DESC, id;
SELECT id FROM tt ORDER BY tm, id;
SELECT id FROM tt ORDER BY ts, id;
-- Pushed: same-kind comparisons, DATE against DATETIME, MIN / MAX, GROUP BY.
SELECT id FROM tt WHERE d = d2 ORDER BY id;
SELECT id FROM tt WHERE dt >= d2 ORDER BY id;
SELECT id FROM tt WHERE d < d2 ORDER BY id;
SELECT MIN(d), MAX(d), MIN(dt), MAX(dt), MIN(tm), MAX(tm), MIN(ts), MAX(ts) FROM tt;
SELECT d2, COUNT(*) FROM tt GROUP BY d2 ORDER BY d2;
SELECT COUNT(DISTINCT d2) FROM tt;
-- String constants compare by MariaDB's rules: a DATE against a time of
-- day compares as DATETIME. Pushed when they parse cleanly.
SELECT id FROM tt WHERE d >= '2024-01-01' ORDER BY id;
SELECT id FROM tt WHERE d = '2024-2-29' ORDER BY id;
SELECT id FROM tt WHERE d = '20240229' ORDER BY id;
SELECT id FROM tt WHERE d = '2024-02-29 10:00:00' ORDER BY id;
SELECT id FROM tt WHERE d < '2024-02-29 10:00:00' ORDER BY id;
SELECT id FROM tt WHERE '2024-03-01' > dt ORDER BY id;
SELECT id FROM tt WHERE dt = '2024-02-29 23:59:59.999999' ORDER BY id;
SELECT id FROM tt WHERE dt > '2024-02-29 23:59:59.9999995' ORDER BY id;
SELECT id FROM tt WHERE dt <=> '1000-01-01' ORDER BY id;
SELECT id FROM tt WHERE tm < '00:00:00' ORDER BY id;
SELECT id FROM tt WHERE tm > '1 10:00:00' ORDER BY id;
SELECT id FROM tt WHERE tm = '-838:59:59' ORDER BY id;
SELECT id FROM tt WHERE tm < '2024-01-01 10:00:00' ORDER BY id;
-- Left to MariaDB: BETWEEN, TIMESTAMP, warnings, zero dates, numbers.
SELECT id FROM tt WHERE dt BETWEEN '2024-02-29' AND '2024-03-01' ORDER BY id;
SELECT id FROM tt WHERE ts > '2000-01-01' ORDER BY id;
SELECT id FROM tt WHERE d = 'garbage' ORDER BY id;
SELECT id FROM tt WHERE d > '0000-00-00' ORDER BY id;
SELECT id FROM tt WHERE d = 20240229 ORDER BY id;
-- Index lookups and ranges through the handler (an InnoDB driver).
CREATE TABLE drv (k INT NOT NULL PRIMARY KEY, d DATE, tm TIME(3)) ENGINE=InnoDB;
INSERT INTO drv VALUES (1, '2024-02-29', '-838:59:59'), (2, '9999-12-31', '838:59:59.999'), (3, NULL, NULL);
SELECT drv.k, tt.id FROM drv STRAIGHT_JOIN tt FORCE INDEX (kd) ON tt.d = drv.d ORDER BY drv.k, tt.id;
SELECT drv.k, tt.id FROM drv STRAIGHT_JOIN tt FORCE INDEX (ktm) ON tt.tm = drv.tm ORDER BY drv.k, tt.id;
SELECT tt.id FROM tt FORCE INDEX (kdt) STRAIGHT_JOIN drv WHERE tt.dt > '2024-01-01' AND drv.k = 1 ORDER BY tt.id;
SELECT tt.id FROM tt FORCE INDEX (kts) STRAIGHT_JOIN drv WHERE tt.ts < '2024-03-01' AND drv.k = 1 ORDER BY tt.id;
-- TIMESTAMP is UTC: another session zone reads the same instant.
SET time_zone = '+03:00';
SELECT id, ts, UNIX_TIMESTAMP(ts) FROM tt ORDER BY id;
SET time_zone = '+00:00';
-- Zero and partial dates.
SET sql_mode = '';
INSERT INTO tt VALUES (6, '0000-00-00', '0000-00-00 00:00:00', '00:00:00', '0000-00-00 00:00:00', '2020-00-00');
SELECT id, d, dt, ts, d2 FROM tt WHERE id = 6;
SET sql_mode = DEFAULT;
-- DML on encoded columns goes through the row pump.
UPDATE tt SET d = '2025-01-01' WHERE dt < '1500-01-01';
UPDATE tt SET tm = '-01:00:00' WHERE tm > '800:00:00';
DELETE FROM tt WHERE ts = '2024-03-01 00:00:00';
SELECT id, d, tm FROM tt ORDER BY id;

-- ENUM and SET sort by index and mask; UNIQUE holds.
CREATE TABLE te (id INT NOT NULL PRIMARY KEY, e ENUM('zeta', 'alpha', 'Mid'),
                 s SET('x', 'y', 'z'), UNIQUE KEY ue (e), KEY ks (s)) ENGINE={E};
INSERT INTO te VALUES (1, 'alpha', 'z'), (2, 'zeta', 'x,y'), (3, 'mid', ''), (4, NULL, 'y,z');
INSERT INTO te VALUES (5, 'Alpha', 'x');
SELECT id, e, s FROM te ORDER BY e, id;
SELECT id, e, s FROM te ORDER BY s, id;
SELECT id FROM te WHERE e = 'ALPHA';
SELECT id FROM te WHERE FIND_IN_SET('y', s) ORDER BY id;
SELECT e FROM te WHERE id = 3;
UPDATE te SET s = 'x,z' WHERE e = 'zeta';
SELECT id, s FROM te ORDER BY id;

-- Binary strings are bytes; other character sets keep their text.
CREATE TABLE tb (id INT NOT NULL PRIMARY KEY, b VARBINARY(8), f BINARY(4),
                 bl BLOB, l VARCHAR(10) CHARACTER SET latin1 COLLATE latin1_bin,
                 KEY kb (b), KEY kl (l)) ENGINE={E};
INSERT INTO tb VALUES (1, 0x00FF7F, 0x01, 0xDEADBEEF00, _latin1 X'E9', 'x'),
                      (2, 'abc', 'ab', '', 'Ã©', NULL),
                      (3, 0x00, NULL, NULL, 'é', 'e'),
                      (4, 0xFF, 0x00000000, 0x00, NULL, NULL);
SELECT id, HEX(b), HEX(f), HEX(bl), HEX(l) FROM tb ORDER BY id;
SELECT id FROM tb ORDER BY b, id;
SELECT id FROM tb WHERE b = 0x00FF7F;
SELECT id FROM tb WHERE b > 0x00 ORDER BY id;
SELECT id FROM tb WHERE l = 'é' ORDER BY id;
SELECT id, LENGTH(b), LENGTH(f) FROM tb ORDER BY id;
UPDATE tb SET b = 0x0102 WHERE f = 0x01;
SELECT id, HEX(b) FROM tb ORDER BY id;
