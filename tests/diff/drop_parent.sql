-- A parent other tables' FOREIGN KEYs name is not dropped, rows or not,
-- alone or listed before its child; after its child it is, and under
-- foreign_key_checks = 0. DROP DATABASE drops its own children with it,
-- whatever their order, and refuses a parent another database's table
-- names.
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
DROP TABLE p;
INSERT INTO p VALUES (1);
INSERT INTO c VALUES (1, 1);
DROP TABLE p;
DROP TABLE p, c;
INSERT INTO c VALUES (2, 77);
DROP TABLE c, p;
CREATE TABLE p (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p (id)) ENGINE={E};
SET foreign_key_checks = 0;
DROP TABLE p;
SET foreign_key_checks = 1;
DROP TABLE c;
CREATE DATABASE dd_{E};
CREATE TABLE dd_{E}.a (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE dd_{E}.b (id INT PRIMARY KEY, aid INT, FOREIGN KEY (aid) REFERENCES a (id)) ENGINE={E};
INSERT INTO dd_{E}.a VALUES (1);
INSERT INTO dd_{E}.b VALUES (1, 1);
DROP DATABASE dd_{E};
CREATE DATABASE dd_{E};
CREATE TABLE dd_{E}.a (id INT PRIMARY KEY) ENGINE={E};
INSERT INTO dd_{E}.a VALUES (1);
CREATE TABLE x (id INT PRIMARY KEY, aid INT, FOREIGN KEY (aid) REFERENCES dd_{E}.a (id)) ENGINE={E};
INSERT INTO x VALUES (1, 1);
DROP DATABASE dd_{E};
INSERT INTO x VALUES (2, 9);
DROP TABLE x;
DROP DATABASE dd_{E};
SHOW DATABASES LIKE 'dd\_{E}';
