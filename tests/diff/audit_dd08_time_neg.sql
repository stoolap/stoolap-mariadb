-- known-diff-file: types: negative and >99h TIME values order wrongly as TEXT
CREATE TABLE t (id INT PRIMARY KEY, tm TIME, KEY (tm)) ENGINE={E};
INSERT INTO t VALUES (1,'-02:00:00'),(2,'-10:00:00'),(3,'09:00:00'),(4,'100:00:00');
SELECT id FROM t WHERE tm > '20:00:00' ORDER BY id;
SELECT id FROM t WHERE tm < '-05:00:00' ORDER BY id;
SELECT id FROM t ORDER BY tm, id;
