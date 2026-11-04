-- known-diff-file: pushdown: stoolap drops MariaDB executable comments /*! ... */
CREATE TABLE t (id INT PRIMARY KEY, n INT) ENGINE={E};
INSERT INTO t VALUES (1,1),(2,2);
SELECT id FROM t WHERE 1=1 /*!40000 AND n > 1 */ ORDER BY id;
