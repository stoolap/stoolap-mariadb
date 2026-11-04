-- known-diff-file: DDL: identifiers needing filename encoding (p-1) break direct DML and FKs
CREATE TABLE `p-1` (id INT PRIMARY KEY, a INT) ENGINE={E};
INSERT INTO `p-1` VALUES (1, 10);
SELECT id, a FROM `p-1` ORDER BY id;
UPDATE `p-1` SET a = 11 WHERE id = 1;
CREATE TABLE c1 (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES `p-1`(id)) ENGINE={E};
SELECT id, a FROM `p-1` ORDER BY id;
