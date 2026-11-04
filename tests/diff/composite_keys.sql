CREATE TABLE ck (a INT NOT NULL, b INT NOT NULL, c INT,
                 d VARCHAR(10) COLLATE utf8mb4_bin, n INT NOT NULL DEFAULT 0,
                 PRIMARY KEY (a, b), UNIQUE KEY uq_cd (c, d),
                 KEY k_bn (b, n)) ENGINE={E};
INSERT INTO ck VALUES (1, 1, 10, 'x', 0), (1, 2, 10, 'y', 0),
                      (2, 1, NULL, 'x', 0), (3, 1, NULL, 'x', 0);
INSERT INTO ck VALUES (1, 1, 99, 'z', 0);
INSERT INTO ck VALUES (5, 5, 10, 'x', 0);
INSERT INTO ck VALUES (1, 2, 77, 'q', 0) ON DUPLICATE KEY UPDATE n = n + 1;
REPLACE INTO ck VALUES (1, 1, 11, 'r', 7);
UPDATE ck SET b = 5 WHERE a = 3 ORDER BY a LIMIT 1;
UPDATE ck SET a = 2, b = 1 WHERE a = 1 AND b = 2;
SELECT a, b, n FROM ck WHERE b = 1 ORDER BY a;
SELECT COUNT(*) FROM ck WHERE b = 1 AND n = 7;
CREATE TABLE drv (a INT NOT NULL, b INT NOT NULL, PRIMARY KEY (a, b)) ENGINE=InnoDB;
INSERT INTO drv VALUES (1, 1), (1, 2), (2, 1), (3, 5), (9, 9);
SELECT d.a, d.b, ck.n FROM drv d JOIN ck ON ck.a = d.a AND ck.b = d.b ORDER BY d.a, d.b;
SELECT d.a, COUNT(ck.b) FROM drv d LEFT JOIN ck ON ck.a = d.a GROUP BY d.a ORDER BY d.a;
