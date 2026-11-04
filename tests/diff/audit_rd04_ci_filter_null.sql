-- known-diff-file: read path: the ci ref filter ignores NULL on both sides
CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_general_ci NULL, KEY ks (s)) ENGINE={E};
INSERT INTO t VALUES (1,'abc'),(2,NULL),(3,'');
CREATE TABLE i (k INT PRIMARY KEY, s VARCHAR(10) COLLATE utf8mb4_general_ci NULL) ENGINE=InnoDB;
INSERT INTO i VALUES (1,'ABC'),(2,NULL);
SELECT i.k, t.id FROM i STRAIGHT_JOIN t FORCE INDEX(ks) ON t.s <=> i.s ORDER BY i.k, t.id;
