\set ECHO none

-- disable the notices for the create script (shell types etc.)
SET client_min_messages = 'WARNING';
\i sql/tinyhist--1.0.0.sql
SET client_min_messages = 'NOTICE';

\set ECHO all

/* simple case */
SELECT tinyhist_agg(i) FROM generate_series(1,10000) s(i);

SELECT * FROM tinyhist_info((SELECT tinyhist_agg(i) FROM generate_series(1,10000) s(i)));

SELECT * FROM tinyhist_buckets((SELECT tinyhist_agg(i) FROM generate_series(1,10000) s(i)));

SELECT (SELECT tinyhist_agg(i) FROM generate_series(1,5000) s(i)) + (SELECT tinyhist_agg(i) FROM generate_series(5001,10000) s(i));

/* wider range */
SELECT tinyhist_agg(i*10) FROM generate_series(1,10000) s(i);

SELECT * FROM tinyhist_info((SELECT tinyhist_agg(i*10) FROM generate_series(1,10000) s(i)));

SELECT * FROM tinyhist_buckets((SELECT tinyhist_agg(i*10) FROM generate_series(1,10000) s(i)));

SELECT (SELECT tinyhist_agg(i * 10) FROM generate_series(1,5000) s(i)) + (SELECT tinyhist_agg(i * 10) FROM generate_series(5001,10000) s(i));

/* aggregate pre-aggregated histograms */
CREATE TABLE tinyhist_test (h tinyhist);

INSERT INTO tinyhist_test SELECT tinyhist_agg(i) FROM generate_series(1,10000) s(i) GROUP BY (random() * 10)::int;
SELECT tinyhist_agg(h) FROM tinyhist_test;

TRUNCATE TABLE tinyhist_test;

INSERT INTO tinyhist_test SELECT tinyhist_agg(i * 10) FROM generate_series(1,10000) s(i) GROUP BY (random() * 10)::int;
SELECT tinyhist_agg(h) FROM tinyhist_test;

DROP TABLE tinyhist_test;
