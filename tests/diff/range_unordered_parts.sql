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
