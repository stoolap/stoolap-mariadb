-- Key reads build their conditions in Rust from the restored key: NULL
-- and BIGINT UNSIGNED bounds past INT64_MAX as MariaDB's index order puts
-- them, composite ranges in lexicographic order, backward reads, lookups
-- of a mixed-engine join, and a ci range the handler filters.
CREATE TABLE kb (id INT NOT NULL PRIMARY KEY, a INT, b INT, u BIGINT UNSIGNED,
                 s VARCHAR(10), d DECIMAL(6,2),
                 KEY ka (a), KEY kab (a, b), KEY ku (u), KEY ks (s), KEY kd (d))
  ENGINE={E} DEFAULT CHARSET=utf8mb4;
INSERT INTO kb VALUES (1, NULL, 1, 0, 'abc', 1.50), (2, 1, NULL, 5, 'ABC', -2.00),
  (3, 1, 2, 9223372036854775807, 'abd', NULL), (4, 2, 1, NULL, 'Abc', 3.25),
  (5, 2, 3, 100, NULL, 0.00), (6, 3, 3, 7, 'b', 1.50);
-- Each read joins a one-row InnoDB table first, so the handler reads kb
-- by its key rather than pushdown taking the whole statement.
CREATE TABLE o (x INT) ENGINE=InnoDB;
INSERT INTO o VALUES (1);
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ka) WHERE kb.a <=> NULL ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ka) WHERE kb.a IS NULL OR kb.a < 2 ORDER BY kb.id;
-- the NULLs a range from NULL keeps, under every upper bound
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ka) WHERE kb.a IS NULL OR kb.a <= 2 ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ka) WHERE kb.a IS NULL OR kb.a < 2 ORDER BY kb.a DESC, kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a IS NULL OR kb.a < 2 ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a = 1 AND (kb.b IS NULL OR kb.b < 3) ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a = 1 AND (kb.b IS NULL OR kb.b <= 2) ORDER BY kb.b DESC, kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a = 2 AND (kb.b IS NULL OR kb.b < 3) ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a BETWEEN 0 AND 1 AND (kb.b IS NULL OR kb.b < 3) ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a BETWEEN 1 AND 2 AND (kb.b IS NULL OR kb.b <= 1) ORDER BY kb.a DESC, kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ka) WHERE kb.a < 2 ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ka) WHERE kb.a > 1 ORDER BY kb.a DESC, kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ku) WHERE kb.u = 18446744073709551615;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ku) WHERE kb.u > 18446744073709551000 ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ku) WHERE kb.u < 18446744073709551000 ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ku) WHERE kb.u >= 9223372036854775808 ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab)
  WHERE (kb.a = 1 AND kb.b >= 2) OR (kb.a = 2 AND kb.b < 3) OR kb.a > 2 ORDER BY kb.a, kb.b;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a = 2 AND kb.b > 1 ORDER BY kb.a, kb.b;
SELECT STRAIGHT_JOIN MAX(kb.b) FROM o JOIN kb FORCE INDEX (kab) WHERE kb.a = 2 AND kb.b < 3;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ks) WHERE kb.s = 'abc' ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (ks) WHERE kb.s >= 'abc' AND kb.s < 'abe' ORDER BY kb.id;
SELECT STRAIGHT_JOIN kb.id FROM o JOIN kb FORCE INDEX (kd) WHERE kb.d >= -2 AND kb.d < 1.6 ORDER BY kb.id;
CREATE TABLE kj (a INT, u BIGINT UNSIGNED, s VARCHAR(10)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
INSERT INTO kj VALUES (1, 5, 'ABC'), (2, 18446744073709551615, 'abd'), (NULL, NULL, NULL), (3, 7, 'B');
SELECT STRAIGHT_JOIN kj.a, kb.id FROM kj JOIN kb FORCE INDEX (ka) ON kb.a = kj.a ORDER BY kj.a, kb.id;
SELECT STRAIGHT_JOIN kj.u, kb.id FROM kj JOIN kb FORCE INDEX (ku) ON kb.u = kj.u ORDER BY kj.u, kb.id;
SELECT STRAIGHT_JOIN kj.s, kb.id FROM kj JOIN kb FORCE INDEX (ks) ON kb.s = kj.s ORDER BY kj.s, kb.id;
UPDATE kb FORCE INDEX (kab) SET b = b + 10 WHERE a = 1 AND b >= 1 ORDER BY a, b LIMIT 1;
SELECT id, a, b FROM kb ORDER BY id;
