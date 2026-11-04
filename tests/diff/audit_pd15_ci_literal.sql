-- known-diff-file: pushdown: literal and expression collations are not guarded (ci compares bytewise)
CREATE TABLE t (id INT PRIMARY KEY, n INT) ENGINE={E};
INSERT INTO t VALUES (1,1);
SELECT id, 'abc' = 'ABC' FROM t ORDER BY id;
SELECT id FROM t WHERE CONCAT('X', n) = 'x1' ORDER BY id;
