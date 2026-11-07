-- A parent whose name has upper-case letters (P under
-- lower_case_table_names = 0, as on Linux) is still a parent: its writes
-- go through the FOREIGN KEY checks.
CREATE TABLE P (id INT PRIMARY KEY, u INT, UNIQUE KEY pu (u)) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pu INT, FOREIGN KEY (pu) REFERENCES P(u)) ENGINE={E};
INSERT INTO P VALUES (1, 10), (2, 20);
INSERT INTO c VALUES (1, 10);
UPDATE P SET u = 11 WHERE id = 1;
DELETE FROM P WHERE id = 1;
UPDATE P SET u = 21 ORDER BY id DESC LIMIT 1;
SELECT id, u FROM P ORDER BY id;
SELECT id, pu FROM c ORDER BY id;
