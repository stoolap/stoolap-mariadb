-- A duplicate in a multi-row INSERT reports 1062 (the bulk flush sets
-- my_errno, which mysql_insert reports).
CREATE TABLE t (id INT NOT NULL PRIMARY KEY) ENGINE={E};
INSERT INTO t VALUES (1);
INSERT INTO t VALUES (2), (1);
SELECT id FROM t ORDER BY id;
