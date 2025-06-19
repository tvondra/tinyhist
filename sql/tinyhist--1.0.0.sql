/* tinyhist for the double precision */
CREATE TYPE tinyhist;

CREATE OR REPLACE FUNCTION tinyhist_in(cstring)
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_in'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION tinyhist_out(tinyhist)
    RETURNS cstring
    AS 'tinyhist', 'tinyhist_out'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION tinyhist_send(tinyhist)
    RETURNS bytea
    AS 'tinyhist', 'tinyhist_send'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION tinyhist_recv(internal)
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_recv'
    LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE tinyhist (
    INPUT = tinyhist_in,
    OUTPUT = tinyhist_out,
    RECEIVE = tinyhist_recv,
    SEND = tinyhist_send,
    INTERNALLENGTH = 32
);

CREATE OR REPLACE FUNCTION tinyhist_append(hist tinyhist, val double precision)
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_append'
    LANGUAGE C IMMUTABLE;

CREATE OPERATOR + (
    LEFTARG = tinyhist,
    RIGHTARG = double precision,
    FUNCTION = tinyhist_append
);

CREATE OR REPLACE FUNCTION tinyhist_append(hist tinyhist, val double precision[])
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_append_array'
    LANGUAGE C IMMUTABLE;

CREATE OPERATOR + (
    LEFTARG = tinyhist,
    RIGHTARG = double precision[],
    FUNCTION = tinyhist_append
);

CREATE OR REPLACE FUNCTION tinyhist_accum(hist tinyhist, val double precision)
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_accum'
    LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION tinyhist_combine(hist_a tinyhist, hist_b tinyhist)
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_combine'
    LANGUAGE C IMMUTABLE;

CREATE AGGREGATE tinyhist_agg(double precision) (
    SFUNC = tinyhist_accum,
    STYPE = tinyhist,
    COMBINEFUNC = tinyhist_combine,
    PARALLEL = SAFE
);

CREATE OR REPLACE FUNCTION tinyhist_buckets(hist tinyhist, out bucket_index int, out bucket_lower double precision, out bucket_upper double precision, out bucket_range double precision, out bucket_count double precision, out bucket_frac double precision, out bucket_density double precision)
    RETURNS SETOF record
    AS 'tinyhist', 'tinyhist_buckets'
    LANGUAGE C IMMUTABLE STRICT;
