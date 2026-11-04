-- Derived tables, CTEs and views over one table are written as that
-- table (MariaDB would merge them): their WHERE goes to the ON or the
-- outer WHERE, their columns become expressions. Ones MariaDB would not
-- merge (window, LIMIT, DISTINCT, GROUP BY) stay subqueries.
CREATE TABLE u (id INT NOT NULL PRIMARY KEY, age INT, bal DOUBLE,
                name VARCHAR(10) COLLATE utf8mb4_bin) ENGINE={E};
CREATE TABLE o (id INT NOT NULL PRIMARY KEY, uid INT, amt INT) ENGINE={E};
INSERT INTO u VALUES (1, 20, 100, 'a'), (2, 35, 900, 'b'), (3, 25, 800, 'c'),
                     (4, 50, NULL, 'd'), (5, NULL, 50, 'e'), (6, 28, 950, 'f');
INSERT INTO o VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30), (4, 3, 40), (5, 9, 50),
                     (6, 6, NULL);
CREATE VIEW v_young AS SELECT id, name, age * 2 AS dbl FROM u WHERE age < 30;

WITH young AS (SELECT * FROM u WHERE age < 30), rich AS (SELECT * FROM u WHERE bal > 700)
SELECT y.name, r.name FROM young y JOIN rich r ON y.id = r.id ORDER BY y.id;
WITH young AS (SELECT * FROM u WHERE age < 30), rich AS (SELECT * FROM u WHERE bal > 700)
SELECT y.name, r.name FROM young y JOIN rich r ON y.id = r.id ORDER BY y.id LIMIT 1;
-- Right side of a LEFT JOIN: its WHERE belongs in the ON.
SELECT u.id, d.amt FROM u LEFT JOIN (SELECT uid, amt FROM o WHERE amt > 15) d ON d.uid = u.id ORDER BY u.id, d.amt;
-- Left side of a LEFT JOIN: its WHERE stays a WHERE.
SELECT d.id, o.amt FROM (SELECT id FROM u WHERE age > 24) d LEFT JOIN o ON o.uid = d.id ORDER BY d.id, o.amt;
-- Expression columns, used in WHERE, GROUP BY and ORDER BY outside.
SELECT d.g, COUNT(*) FROM (SELECT CASE WHEN age < 30 THEN 'y' ELSE 'o' END AS g FROM u) d GROUP BY d.g ORDER BY d.g;
SELECT d.id, d.b2 FROM (SELECT id, bal * 2 AS b2 FROM u WHERE bal IS NOT NULL) d WHERE d.b2 > 1000 ORDER BY d.b2 DESC;
-- Nested derived tables and a comma join.
SELECT x.id FROM (SELECT * FROM (SELECT id, age FROM u WHERE age > 20) i WHERE i.age < 40) x ORDER BY x.id;
SELECT a.id, b.id FROM (SELECT id FROM u WHERE id < 3) a, (SELECT id FROM u WHERE id > 4) b ORDER BY a.id, b.id;
-- A CTE used twice.
WITH c AS (SELECT id, age FROM u WHERE age IS NOT NULL) SELECT a.id, b.id FROM c a JOIN c b ON b.age = a.age + 15 ORDER BY a.id;
-- Views.
SELECT id, dbl FROM v_young ORDER BY id;
SELECT v.name, o.amt FROM v_young v JOIN o ON o.uid = v.id ORDER BY o.id;
-- Not merged: window, LIMIT, DISTINCT, GROUP BY in the derived table.
SELECT d.id, d.rn FROM (SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM u) d WHERE d.id > 3 ORDER BY d.id;
SELECT d.id FROM (SELECT id FROM u ORDER BY id DESC LIMIT 2) d ORDER BY d.id;
SELECT d.age FROM (SELECT DISTINCT age FROM u) d WHERE d.age > 20 ORDER BY d.age;
SELECT d.uid, d.s FROM (SELECT uid, SUM(amt) AS s FROM o GROUP BY uid) d WHERE d.s > 10 ORDER BY d.uid;
-- A derived table in a subquery.
SELECT id FROM u WHERE id IN (SELECT d.uid FROM (SELECT uid FROM o WHERE amt >= 20) d) ORDER BY id;
SELECT id, (SELECT MAX(d.amt) FROM (SELECT uid, amt FROM o WHERE amt < 45) d WHERE d.uid = u.id) FROM u ORDER BY id;
-- The inner side of a LEFT JOIN reads NULL in every column of a
-- missing row, constants and expressions included.
SELECT u.id, d.one, d.amt0 FROM u LEFT JOIN (SELECT uid, 1 AS one, COALESCE(amt, 0) AS amt0 FROM o WHERE amt > 15) d ON d.uid = u.id ORDER BY u.id, d.amt0;
SELECT u.id, d.uid FROM u LEFT JOIN (SELECT uid FROM o WHERE amt IS NULL) d ON d.uid = u.id ORDER BY u.id;
