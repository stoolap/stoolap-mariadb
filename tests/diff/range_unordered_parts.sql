-- Composite ranges over a part stoolap cannot order (multi-byte hex, ci).
CREATE TABLE cu (id INT NOT NULL PRIMARY KEY, a INT NOT NULL,
                 c VARCHAR(2) CHARACTER SET utf16le COLLATE utf16le_bin,
                 KEY kac (a, c)) ENGINE={E};
INSERT INTO cu VALUES (1, 1, _utf16le X'4100'), (2, 2, _utf16le X'4100'), (3, 2, _utf16le X'0001'), (4, 3, _utf16le X'4100');
SELECT id FROM cu FORCE INDEX (kac) WHERE a BETWEEN 1 AND 2 AND c = _utf16le X'4100' ORDER BY id;
SELECT id FROM cu FORCE INDEX (kac) WHERE a >= 2 AND c = _utf16le X'4100' ORDER BY id;
SELECT id FROM cu FORCE INDEX (kac) WHERE a = 2 AND c > _utf16le X'4100' ORDER BY id;
CREATE TABLE ci2 (id INT NOT NULL PRIMARY KEY, a INT NOT NULL,
                  c VARCHAR(5) COLLATE utf8mb4_general_ci, KEY kac (a, c)) ENGINE={E};
INSERT INTO ci2 VALUES (1, 1, 'b'), (2, 2, 'B'), (3, 2, 'a'), (4, 3, 'b'), (5, 2, 'c');
SELECT id FROM ci2 FORCE INDEX (kac) WHERE a BETWEEN 1 AND 2 AND c = 'b' ORDER BY id;
SELECT id FROM ci2 FORCE INDEX (kac) WHERE a = 2 AND c > 'A' ORDER BY id;
SELECT id FROM ci2 FORCE INDEX (kac) WHERE a = 2 AND c BETWEEN 'A' AND 'B' ORDER BY id;
-- A NULL first part sorts first: an upper bound keeps it.
CREATE TABLE cn (id INT NOT NULL PRIMARY KEY, a INT NULL,
                 c VARCHAR(2) CHARACTER SET utf16le COLLATE utf16le_bin,
                 KEY kac (a, c)) ENGINE={E};
INSERT INTO cn VALUES (1, NULL, _utf16le X'4100'), (2, 2, _utf16le X'4100'), (3, 3, _utf16le X'4100'), (4, NULL, _utf16le X'0001');
SELECT id FROM cn FORCE INDEX (kac) WHERE (a IS NULL OR a <= 2) AND c = _utf16le X'4100' ORDER BY id;
-- A PAD SPACE first part: 'a ' equals 'a'.
CREATE TABLE cp (id INT NOT NULL PRIMARY KEY, a VARCHAR(3) COLLATE utf8mb4_bin,
                 c VARCHAR(2) CHARACTER SET utf16le COLLATE utf16le_bin,
                 KEY kac (a, c)) ENGINE={E};
INSERT INTO cp VALUES (1, 'a', _utf16le X'4100'), (2, 'a ', _utf16le X'4100'), (3, 'b', _utf16le X'4100'), (4, 'c', _utf16le X'4100');
SELECT id FROM cp FORCE INDEX (kac) WHERE a IN ('a', 'b') AND c = _utf16le X'4100' ORDER BY id;
-- A constant unordered middle part keeps the order of the parts after it.
CREATE TABLE co (id INT NOT NULL PRIMARY KEY, a INT NOT NULL,
                 c VARCHAR(2) CHARACTER SET utf16le COLLATE utf16le_bin, b INT NOT NULL,
                 KEY kacb (a, c, b)) ENGINE={E};
INSERT INTO co VALUES (1, 1, _utf16le X'4100', 3), (2, 1, _utf16le X'4100', 1), (3, 2, _utf16le X'4100', 2),
                      (4, 2, _utf16le X'4100', 0), (5, 1, _utf16le X'0001', 0), (6, 1, _utf16le X'4100', 2);
SELECT id, a, b FROM co FORCE INDEX (kacb) WHERE a IN (1, 2) AND c = _utf16le X'4100' ORDER BY a, b;
SELECT id FROM co FORCE INDEX (kacb) WHERE a IN (1, 2) AND c = _utf16le X'4100' ORDER BY a, b LIMIT 2;
