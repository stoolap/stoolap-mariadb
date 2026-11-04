-- known-diff-file: read path: HA_READ_PREFIX_LAST(_OR_PREV) unsupported (MAX with a prefix bound)
CREATE TABLE t (id INT PRIMARY KEY, a INT NOT NULL, b INT NOT NULL, KEY kab (a,b)) ENGINE={E};
INSERT INTO t VALUES (1,1,10),(2,1,20),(3,2,30);
CREATE TABLE i (k INT PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO i VALUES (1);
SELECT MAX(t.b) FROM t, i WHERE t.a = 1;
