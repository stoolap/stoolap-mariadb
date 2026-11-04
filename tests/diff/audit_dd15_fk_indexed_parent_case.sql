-- known-diff-file: DDL: FK to an indexed non-unique parent column refused
CREATE TABLE p (id INT PRIMARY KEY, code INT, KEY (code)) ENGINE={E};
CREATE TABLE c (id INT PRIMARY KEY, pcode INT, FOREIGN KEY (pcode) REFERENCES p(code)) ENGINE={E};
CREATE TABLE T1 (id INT PRIMARY KEY) ENGINE={E};
CREATE TABLE t1 (id INT PRIMARY KEY) ENGINE={E};
