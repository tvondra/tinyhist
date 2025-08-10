\set ECHO all

/* aggregate pre-aggregated histograms */
CREATE TABLE tinyhist_test (h tinyhist);

INSERT INTO tinyhist_test SELECT tinyhist_agg(i) FROM generate_series(1,10000) s(i) GROUP BY (random() * 10)::int;
SELECT tinyhist_agg(h) FROM tinyhist_test;

TRUNCATE TABLE tinyhist_test;

INSERT INTO tinyhist_test SELECT tinyhist_agg(i * 10) FROM generate_series(1,10000) s(i) GROUP BY (random() * 10)::int;
SELECT tinyhist_agg(h) FROM tinyhist_test;

DROP TABLE tinyhist_test;
