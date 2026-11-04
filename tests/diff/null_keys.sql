CREATE TABLE t (id INT NOT NULL PRIMARY KEY, k INT, s VARCHAR(10) COLLATE utf8mb4_bin,
                KEY ik (k), KEY iks (k, s)) ENGINE={E};
INSERT INTO t VALUES (1, NULL, 'a'), (2, NULL, NULL), (3, 1, 'a'), (4, 1, NULL), (5, 2, 'b');
CREATE TABLE drv (k INT, s VARCHAR(10) COLLATE utf8mb4_bin) ENGINE=InnoDB;
INSERT INTO drv VALUES (NULL, 'a'), (1, NULL), (1, 'a'), (2, 'b'), (3, 'c');
SELECT d.k, d.s, t.id FROM drv d JOIN t ON t.k = d.k ORDER BY d.k, d.s, t.id;
SELECT d.k, d.s, t.id FROM drv d JOIN t ON t.k <=> d.k ORDER BY d.k, d.s, t.id;
SELECT d.k, d.s, t.id FROM drv d JOIN t ON t.k = d.k AND t.s = d.s ORDER BY d.k, d.s, t.id;
SELECT d.k, d.s, t.id FROM drv d JOIN t ON t.k <=> d.k AND t.s <=> d.s ORDER BY d.k, d.s, t.id;
SELECT id FROM t WHERE k IS NULL ORDER BY id;
SELECT id FROM t WHERE k > 0 ORDER BY id;
