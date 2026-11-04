-- Constructs the SELECT translator writes for stoolap (pushed when every
-- part translates); each must match InnoDB.
CREATE TABLE u (id INT NOT NULL PRIMARY KEY, name VARCHAR(20) COLLATE utf8mb4_nopad_bin,
                age INT, bal DOUBLE) ENGINE={E};
CREATE TABLE o (id INT NOT NULL PRIMARY KEY, uid INT, amt INT,
                st VARCHAR(10) COLLATE utf8mb4_nopad_bin) ENGINE={E};
INSERT INTO u VALUES (1,'ann',30,100.5),(2,'bob',NULL,20.25),(3,'cid',45,NULL),
                     (4,'dee',30,75.0),(5,NULL,60,5.5);
INSERT INTO o VALUES (1,1,10,'done'),(2,1,20,'open'),(3,2,30,'done'),
                     (4,NULL,40,'done'),(5,4,NULL,'void');
SELECT id, CASE WHEN age < 35 THEN 'young' WHEN age < 50 THEN 'mid' ELSE 'old' END FROM u ORDER BY id;
SELECT id, CASE WHEN age < 35 THEN 'young' END FROM u ORDER BY id;
SELECT id, CASE age WHEN 30 THEN 'x' WHEN 45 THEN 'y' ELSE 'z' END FROM u ORDER BY id;
SELECT id, IF(bal > 50, 'rich', 'poor'), COALESCE(age, -1), IFNULL(bal, 0), NULLIF(age, 30) FROM u ORDER BY id;
SELECT id FROM u WHERE id IN (SELECT uid FROM o WHERE st = 'done') ORDER BY id;
SELECT id FROM u WHERE id NOT IN (SELECT uid FROM o WHERE st = 'done') ORDER BY id;
SELECT id FROM u WHERE id NOT IN (SELECT uid FROM o WHERE uid IS NOT NULL) ORDER BY id;
SELECT id FROM u WHERE EXISTS (SELECT 1 FROM o WHERE o.uid = u.id AND o.amt > 15) ORDER BY id;
SELECT id FROM u WHERE NOT EXISTS (SELECT 1 FROM o WHERE o.uid = u.id) ORDER BY id;
SELECT id, (SELECT COUNT(*) FROM o WHERE o.uid = u.id) FROM u ORDER BY id;
SELECT id FROM u WHERE bal > (SELECT AVG(bal) FROM u) ORDER BY id;
SELECT g, COUNT(*) FROM (SELECT CASE WHEN age < 40 THEN 'a' ELSE 'b' END g FROM u) t GROUP BY g ORDER BY g;
WITH big AS (SELECT uid, SUM(amt) s FROM o GROUP BY uid HAVING SUM(amt) > 15) SELECT u.id, big.s FROM u JOIN big ON big.uid = u.id ORDER BY u.id;
SELECT id, ROW_NUMBER() OVER (ORDER BY age DESC, id), RANK() OVER (ORDER BY age), LAG(bal, 1) OVER (ORDER BY id), LEAD(age, 2) OVER (ORDER BY id), DENSE_RANK() OVER (PARTITION BY age ORDER BY id) FROM u ORDER BY id;
SELECT id, SUM(amt) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) FROM o ORDER BY id;
SELECT id, age FROM u ORDER BY age, id LIMIT 3 OFFSET 1;
SELECT id, age FROM u ORDER BY age DESC, id LIMIT 3;
SELECT u.id, o.id FROM u LEFT JOIN o ON o.uid = u.id AND o.amt > 15 ORDER BY u.id, o.id;
SELECT uid, COUNT(*), SUM(amt), MIN(amt), MAX(amt), COUNT(DISTINCT st) FROM o GROUP BY uid HAVING COUNT(*) >= 1 ORDER BY uid;
SELECT DISTINCT age FROM u ORDER BY age;
SELECT id FROM u WHERE name LIKE 'b%' OR name NOT LIKE '%e%' ORDER BY id;
SELECT id FROM u WHERE age <=> NULL OR age <=> 45 ORDER BY id;
SELECT id FROM u WHERE age BETWEEN 30 AND 45 AND id NOT BETWEEN 2 AND 3 ORDER BY id;
SELECT id, -age, ABS(bal - 50), bal * 2 + 1, bal / 4 FROM u ORDER BY id;
SELECT u.id, UPPER(name), LOWER(name), CHAR_LENGTH(name), CONCAT(name, '-', st) FROM u JOIN o ON o.uid = u.id ORDER BY u.id, o.id;
