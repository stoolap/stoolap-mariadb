-- A row the handler reads keeps its BLOB / TEXT bytes in its own buffer,
-- not the field's, which MariaDB's own store into the field overwrites:
-- UPDATE's old and new rows, and the duplicate REPLACE and ON DUPLICATE
-- KEY UPDATE read next to the row they insert, each keep their values.
CREATE TABLE b (id INT NOT NULL PRIMARY KEY, t TEXT, bl BLOB, l TEXT CHARACTER SET latin1,
                c MEDIUMTEXT, n INT) ENGINE={E} DEFAULT CHARSET=utf8mb4;
INSERT INTO b VALUES (1, 'abc', X'00FF01', 'café', 'zip', 0),
  (2, 'old-long-text', X'0102030405060708', 'longer latin', 'compressed old', 0),
  (3, 'q', NULL, NULL, NULL, 0), (4, NULL, X'', '', '', 0);
UPDATE b SET t = 'xyz', bl = X'AABBCC', l = 'cafe', c = 'pzi' WHERE id = 1 ORDER BY id LIMIT 1;
UPDATE b SET n = n + 1, t = CONCAT(t, '+') ORDER BY id LIMIT 4;
SELECT id, t, HEX(bl), l, c, n FROM b ORDER BY id;
REPLACE INTO b VALUES (2, 'new', X'09', 'x', 'c', 7), (3, 'much longer than before', X'0A0B0C0D0E0F', 'y', 'cc', 8);
SELECT id, t, HEX(bl), l, c, n FROM b ORDER BY id;
INSERT INTO b VALUES (3, 'zz', X'11', 'w', 'v', 0)
  ON DUPLICATE KEY UPDATE t = CONCAT(VALUES(t), '!', t), bl = VALUES(bl), l = CONCAT(l, VALUES(l)), c = VALUES(c);
INSERT INTO b VALUES (4, 'n', X'22', 'm', 'k', 0), (1, 'p', X'33', 'o', 'j', 0)
  ON DUPLICATE KEY UPDATE t = CONCAT(IFNULL(t, '-'), VALUES(t)), n = n + 10;
SELECT id, t, HEX(bl), l, c, n FROM b ORDER BY id;
