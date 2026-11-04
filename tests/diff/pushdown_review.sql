-- Review findings on the SELECT translator (PR #7), each a statement
-- that pushed with a different result.
CREATE TABLE t (id INT NOT NULL PRIMARY KEY, k INT, v INT,
                s VARCHAR(10) COLLATE utf8mb4_general_ci,
                tr VARCHAR(10) COLLATE utf8mb4_turkish_ci) ENGINE={E};
INSERT INTO t VALUES (1, 5, 2, NULL, 'i'), (2, 3, 1, '\tx\t', 'ı'),
                     (3, 9, 1, 'ab', 'i'), (4, 1, 3, 'cd', 'I'), (5, 3, 4, NULL, NULL);
-- IF / CASE / COALESCE branches mixing INT and DOUBLE take the DOUBLE type.
SELECT id, IF(id = 1, 3, 1e0) / 2, COALESCE(v, 0.5e0) / 2 FROM t ORDER BY id;
-- CONCAT is NULL when any argument is NULL.
SELECT id, CONCAT(s, 'x'), CONCAT('a', s, 'b') FROM t ORDER BY id;
-- One-argument TRIM strips spaces only.
SELECT id, TRIM(s), LTRIM(s), RTRIM(s) FROM t ORDER BY id;
-- Case mapping follows the collation (Turkish dotted / dotless i).
SELECT id, UPPER(tr), LOWER(tr) FROM t ORDER BY id;
-- WITH TIES keeps every row tied with the last one.
SELECT id FROM t ORDER BY v FETCH FIRST 1 ROWS WITH TIES;
-- GROUP BY sorts by its keys (and their direction) unless ORDER BY says otherwise.
SELECT k FROM t GROUP BY k LIMIT 1;
SELECT k FROM t GROUP BY k DESC LIMIT 1;
SELECT k, COUNT(*) FROM t GROUP BY k LIMIT 2 OFFSET 1;
-- SQL_CALC_FOUND_ROWS counts past the LIMIT.
SELECT SQL_CALC_FOUND_ROWS id FROM t ORDER BY id LIMIT 1;
SELECT FOUND_ROWS();
-- An exact ref on a filtered (PAD SPACE) key part keeps the next part's order.
CREATE TABLE kp (id INT NOT NULL PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_bin,
                 n INT, KEY ks (s, n)) ENGINE={E};
INSERT INTO kp VALUES (1, 'a', 10), (2, 'a ', 1), (3, 'b', 5);
SELECT n FROM kp FORCE INDEX (ks) WHERE s = 'a ' ORDER BY n LIMIT 1;
SELECT n FROM kp FORCE INDEX (ks) WHERE s = 'a' ORDER BY n DESC LIMIT 1;
-- More mixed-type results: CASE branches, IFNULL, and a DECIMAL branch.
SELECT id, CASE WHEN v > 2 THEN 7 ELSE 2.5e0 END / 2,
       CASE k WHEN 3 THEN 1 WHEN 5 THEN 1e0 ELSE 0 END / 4,
       IFNULL(s, 1) FROM t ORDER BY id;
SELECT id, IF(id = 1, 3, 0.5) FROM t ORDER BY id;
-- A grouped derived table under a LIMIT, and GROUP BY with HAVING.
SELECT * FROM (SELECT k, COUNT(*) AS c FROM t GROUP BY k LIMIT 2) d ORDER BY k;
SELECT k, SUM(v) FROM t GROUP BY k HAVING SUM(v) > 1;
-- EXISTS ignores its select list, but an aggregate one always has a row.
SELECT id FROM t WHERE EXISTS (SELECT LOWER(tr) FROM t t2 WHERE t2.k = t.v) ORDER BY id;
SELECT COUNT(*) FROM t WHERE EXISTS (SELECT COUNT(*) FROM t t2 WHERE t2.id > 100);
SELECT COUNT(*) FROM t WHERE EXISTS (SELECT k FROM t t2 WHERE t2.id > 100 GROUP BY k);
-- sql_mode=ORACLE variants keep the plain names: CONCAT skips NULLs,
-- DECODE matches NULL to NULL.
SET sql_mode = 'ORACLE';
SELECT id, CONCAT(s, 'x'), DECODE(s, NULL, 'null', 'other') FROM t ORDER BY id;
SET sql_mode = DEFAULT;
