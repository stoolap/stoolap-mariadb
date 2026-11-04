-- known-diff-file: pushdown: stoolap LENGTH counts characters, MariaDB counts bytes
CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_bin) ENGINE={E};
INSERT INTO t VALUES (1,'héllo');
SELECT id, LENGTH(s), CHAR_LENGTH(s) FROM t ORDER BY id;
