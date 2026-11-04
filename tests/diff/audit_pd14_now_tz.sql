-- known-diff-file: pushdown: stoolap NOW() is UTC, MariaDB uses the session time_zone
SET time_zone = '+14:00';
CREATE TABLE t (id INT PRIMARY KEY, ts DATETIME) ENGINE={E};
INSERT INTO t VALUES (1, NOW() - INTERVAL 1 HOUR);
SELECT id FROM t WHERE ts < NOW() ORDER BY id;
