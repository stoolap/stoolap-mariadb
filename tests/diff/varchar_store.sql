-- A utf8mb4 VARCHAR value is read back byte for byte, at its length
-- limit, with multibyte and 4-byte characters, through a one-byte and a
-- two-byte length prefix, by the handler and by pushdown.
CREATE TABLE v (id INT NOT NULL PRIMARY KEY, s VARCHAR(4), t VARCHAR(300),
                KEY ks (s)) ENGINE={E} DEFAULT CHARSET=utf8mb4;
INSERT INTO v VALUES (1, 'abcd', REPEAT('x', 300)), (2, 'çğüş', REPEAT('é', 300)),
  (3, '😀😀😀😀', CONCAT(REPEAT('a', 296), '😀')), (4, '', ''), (5, 'a ', 'b  ');
SELECT id, s, HEX(s), LENGTH(t), CHAR_LENGTH(t), RIGHT(t, 2) FROM v ORDER BY id;
SELECT id, s, LENGTH(t) FROM v FORCE INDEX (ks) WHERE s >= '' ORDER BY s, id;
SELECT CONCAT(s, '|', LENGTH(s)) FROM v ORDER BY id;
CREATE TABLE j (id INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO j VALUES (1), (2), (3), (4), (5);
SELECT STRAIGHT_JOIN j.id, v.s, HEX(v.s), LENGTH(v.t) FROM j JOIN v ON v.id = j.id ORDER BY j.id;
