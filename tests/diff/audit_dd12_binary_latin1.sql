-- known-diff-file: types: binary and latin1 bytes are refused (bound as UTF-8 TEXT)
CREATE TABLE t (id INT PRIMARY KEY, b VARBINARY(10), c VARCHAR(10) CHARACTER SET latin1) ENGINE={E};
INSERT INTO t VALUES (1, 0xFF00FE, 'x');
INSERT INTO t VALUES (2, 'y', _latin1 X'E9');
SELECT id, HEX(b), HEX(c) FROM t ORDER BY id;
