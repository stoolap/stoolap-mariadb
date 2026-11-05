-- A multi-row INSERT into a table with a UNIQUE key checks its rows at
-- once: a key twice in the statement, a key the table holds, a key an
-- earlier statement of the transaction wrote, NULLs (which never
-- collide), a composite key, and a second UNIQUE key failing alone.
CREATE TABLE u (id INT NOT NULL PRIMARY KEY, e VARCHAR(20) COLLATE utf8mb4_bin,
                f INT, a INT, b INT, UNIQUE KEY ue (e), UNIQUE KEY uf (f),
                UNIQUE KEY uab (a, b)) ENGINE={E};
INSERT INTO u VALUES (1, 'x1', 1, 1, 1), (2, 'x2', 2, 1, 2), (3, NULL, NULL, NULL, 1),
  (4, NULL, NULL, NULL, 1), (5, 'x5', 5, 2, NULL), (6, 'x6', 6, 2, NULL);
INSERT INTO u VALUES (10, 'y10', 10, 10, 10), (11, 'y10', 11, 11, 11);
INSERT INTO u VALUES (12, 'y12', 12, 12, 12), (13, 'x1', 13, 13, 13);
INSERT INTO u VALUES (14, 'y14', 14, 14, 14), (15, 'y15', 2, 15, 15);
INSERT INTO u VALUES (16, 'y16', 16, 1, 1), (17, 'y17', 17, 17, 17);
INSERT INTO u VALUES (18, 'y18', 18, 18, 18), (19, 'y19', 19, 18, 18);
SELECT id, e, f, a, b FROM u ORDER BY id;
BEGIN;
INSERT INTO u VALUES (20, 'z20', 20, 20, 20), (21, 'z21', 21, 21, 21);
INSERT INTO u VALUES (22, 'z22', 22, 22, 22), (23, 'z20', 23, 23, 23);
INSERT INTO u VALUES (24, 'z24', 24, 24, 24), (25, 'z25', 21, 25, 25);
INSERT INTO u VALUES (26, 'z26', 26, 26, 26), (27, 'z27', 27, 26, 26);
INSERT INTO u VALUES (28, 'z28', 28, 28, 28), (29, 'z29', 29, 29, 29);
COMMIT;
SELECT id, e, f, a, b FROM u ORDER BY id;
INSERT INTO u SELECT id + 100, CONCAT('s', id), id + 100, id + 100, 0 FROM u WHERE id < 30;
INSERT INTO u SELECT id + 200, e, id + 200, id + 200, 0 FROM u WHERE id = 1 OR id = 2;
SELECT COUNT(*), MIN(id), MAX(id) FROM u;
