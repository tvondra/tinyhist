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

CREATE OR REPLACE FUNCTION tinyhist_add(hist tinyhist, val double precision)
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_add'
    LANGUAGE C IMMUTABLE;

CREATE OPERATOR + (
    LEFTARG = tinyhist,
    RIGHTARG = double precision,
    FUNCTION = tinyhist_add
);

CREATE OR REPLACE FUNCTION tinyhist_add(hist tinyhist, val double precision[])
    RETURNS tinyhist
    AS 'tinyhist', 'tinyhist_add_array'
    LANGUAGE C IMMUTABLE;

CREATE OPERATOR + (
    LEFTARG = tinyhist,
    RIGHTARG = double precision[],
    FUNCTION = tinyhist_add
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

-- information about a histogram
CREATE OR REPLACE FUNCTION tinyhist_info(
  in  hist tinyhist,					-- input histogram
  out hist_unit int,					-- size of "unit" range
  out hist_sample_rate int,				-- sampled fraction of values
  out hist_count bigint,				-- number of values in buckets
  out hist_upper bigint					-- histogram upper boundary
)
    RETURNS record
    AS 'tinyhist', 'tinyhist_info'
    LANGUAGE C IMMUTABLE STRICT;


-- information about buckets of a histogram
CREATE OR REPLACE FUNCTION tinyhist_buckets(
  in  hist tinyhist,					-- input histogram
  out bucket_index int,					-- bucket index (0 .. 15)
  out bucket_lower double precision,	-- bucket lower boundary
  out bucket_upper double precision,	-- bucket upper boundary
  out bucket_range double precision,	-- range (upper - lower)
  out bucket_count double precision,	-- number of values in bucket
  out bucket_frac double precision,		-- fraction of the total
  out bucket_density double precision	-- density (fraction / range)
)
    RETURNS SETOF record
    AS 'tinyhist', 'tinyhist_buckets'
    LANGUAGE C IMMUTABLE STRICT;
