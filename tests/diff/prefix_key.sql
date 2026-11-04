CREATE TABLE t (id INT NOT NULL PRIMARY KEY, a VARCHAR(20) COLLATE utf8mb4_bin,
                KEY k (a(3))) ENGINE={E};
INSERT INTO t VALUES (1, 'abc1'), (2, 'abc2'), (3, 'xyz');
CREATE TABLE d (k VARCHAR(20) COLLATE utf8mb4_bin NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO d VALUES ('abc1'), ('xyz');
-- known-diff: index_read_map binds a prefix key part as an exact match (follow-up to PR #4)
SELECT d.k, t.id FROM d STRAIGHT_JOIN t FORCE INDEX (k) ON t.a = d.k ORDER BY d.k;
SELECT id FROM t WHERE a = 'abc2';
