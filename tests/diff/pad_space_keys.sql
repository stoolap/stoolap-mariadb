-- A key lookup on a PAD SPACE binary collation finds every value equal
-- but for trailing spaces, through a secondary and a UNIQUE key, a join,
-- a composite key, a NULL, multibyte values, and a column too long for
-- the lookups (a range there).
CREATE TABLE p (id INT NOT NULL PRIMARY KEY, s VARCHAR(8) COLLATE utf8mb4_bin,
                u VARCHAR(10) COLLATE utf8mb4_bin, n INT NOT NULL,
                KEY ks (s), UNIQUE KEY ku (u), KEY ksn (s, n)) ENGINE={E};
INSERT INTO p VALUES (1, 'a', 'x1', 1), (2, 'a ', 'x2', 2), (3, 'a  ', 'x3', 3),
  (4, 'a\t', 'x4', 4), (5, 'ab', 'x5', 5), (6, NULL, 'x6', 6), (7, '', 'x7', 7),
  (8, ' ', 'x8', 8), (9, 'é', 'é ', 9), (10, 'é  ', 'y', 10), (11, '😀 ', 'z', 11),
  (12, 'a       ', 'x12', 12);
SELECT id, HEX(s) FROM p FORCE INDEX (ks) WHERE s = 'a' ORDER BY id;
SELECT id, HEX(s) FROM p FORCE INDEX (ks) WHERE s = 'a   ' ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s = '' ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s = 'é' ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s = '😀' ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s IS NULL ORDER BY id;
SELECT id, n FROM p FORCE INDEX (ksn) WHERE s = 'a ' AND n = 2;
SELECT id FROM p FORCE INDEX (ku) WHERE u = 'é' ORDER BY id;
SELECT id FROM p FORCE INDEX (ku) WHERE u = 'x1   ' ORDER BY id;
CREATE TABLE q (k INT NOT NULL PRIMARY KEY, s VARCHAR(8) COLLATE utf8mb4_bin) ENGINE=InnoDB;
INSERT INTO q VALUES (1, 'a'), (2, 'a '), (3, 'é '), (4, ''), (5, 'nope'), (6, NULL);
SELECT q.k, p.id FROM q STRAIGHT_JOIN p FORCE INDEX (ks) ON p.s = q.s ORDER BY q.k, p.id;
SELECT q.k, p.id FROM q STRAIGHT_JOIN p FORCE INDEX (ku) ON p.u = q.s ORDER BY q.k, p.id;
INSERT INTO p VALUES (20, 'b', 'x1', 0) ON DUPLICATE KEY UPDATE n = n + 100;
SELECT id, n FROM p WHERE id IN (1, 20) ORDER BY id;

-- Past the lookups' cap: a byte range bounds the candidates.
CREATE TABLE l (id INT NOT NULL PRIMARY KEY, s VARCHAR(300) COLLATE utf8mb4_bin,
                KEY ks (s)) ENGINE={E};
INSERT INTO l VALUES (1, 'a'), (2, 'a '), (3, CONCAT('a', REPEAT(' ', 299))),
  (4, 'a\t'), (5, 'a !'), (6, 'b');
SELECT id FROM l FORCE INDEX (ks) WHERE s = 'a  ' ORDER BY id;
SELECT q.k, l.id FROM q STRAIGHT_JOIN l FORCE INDEX (ks) ON l.s = q.s ORDER BY q.k, l.id;

-- A range on such a part, or on a prefix part, is MariaDB's comparison:
-- 'a' and 'a ' are one value, and a prefix bound covers longer values.
SELECT id FROM p FORCE INDEX (ks) WHERE s BETWEEN 'a' AND 'a' ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s IN ('a', 'é') ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s < 'a ' ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s > 'a' AND s < 'b' ORDER BY id;
SELECT id, n FROM p FORCE INDEX (ksn) WHERE s = 'a' AND n BETWEEN 2 AND 12 ORDER BY id;
SELECT id FROM p FORCE INDEX (ks) WHERE s >= 'a' ORDER BY s, id;
CREATE TABLE x (id INT NOT NULL PRIMARY KEY, b VARBINARY(10), KEY kb (b(2))) ENGINE={E};
INSERT INTO x VALUES (1, 'aa'), (2, 'aab'), (3, 'ab'), (4, 'b'), (5, 'a');
SELECT id FROM x FORCE INDEX (kb) WHERE b <= 'aa' ORDER BY id;
SELECT id FROM x FORCE INDEX (kb) WHERE b BETWEEN 'aa' AND 'ab' ORDER BY id;
SELECT id FROM x FORCE INDEX (kb) WHERE b = 'aab';
