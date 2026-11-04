-- known-diff-file: types: DATE/TIME stored as TEXT compare lexicographically
CREATE TABLE t (id INT PRIMARY KEY, d DATE, tm TIME) ENGINE={E};
INSERT INTO t VALUES (1,'2024-01-10','10:00:00'),(2,'2024-01-03','100:00:00'),(3,'2024-01-20','20:00:00');
SELECT id FROM t WHERE d > '2024-1-5' ORDER BY id;
SELECT id FROM t ORDER BY tm;
