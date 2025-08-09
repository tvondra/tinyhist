# tinyhist extension

[![make installcheck](https://github.com/tvondra/tinyhist/actions/workflows/ci.yml/badge.svg)](https://github.com/tvondra/tinyhist/actions/workflows/ci.yml)

> :warning: **Warning**: This extension is still an early WIP version, not
> suitable for production use. The on-disk format, function signatures etc.
> may change and so on.

This PostgreSQL extension implements `tinyhist`, a data structure for
on-line accumulation of approximate histograms. The algorithm is very
friendly to parallel programs, fully mergeable, etc.

Each histogram is only 32B, and has 16 buckets. The buckets are not of
the same size. Each bucket covers twice the range of the preceding one,
so the accuracy is better for lower values (the buckets are smaller).
The smaller buckets also use less space, making the histogram compact.

The bucket sizes look like this:

```
0 =>	8 bits
1 =>	9 bits
2 =>	10 bits
3 =>	11 bits
...
15 =>	23 bits
```

Assuming uniformly distributed value, doubling a bucket range means it
will get twice the number of values. Which aligns with the extra bit
for each bucket, and all buckets getting "full" at about the same time.
Of course, the data distribution may not be compact, in which case the
buckets get full at different times.


histogram range
---------------

A histogram with a fixed number of buckets can represent only a limited
range of values. The 16th bucket is as wide as the 1st after 15 rounds
of doubling, so if the first bucket is `[0,1]`, then the last bucket is
`(16384,32768]`.

```
0 =>	[0,1]
1 =>	(1,2]
2 =>	(2,4]
3 =>	(4,8]
...
14 =>	(8192,16384]
15 =>	(16384,32768]
```

***Note**: This shows the buckets don't quite double, because the first
two buckets cover the same (exclusive) range. This may result in some
inefficiency, but has no other effects on how the histogram works.*

This determines the "dynamic range" of the histogram. It'll always be
the case that the last histogram is 32768x wider than the first one,
but it's to us to decide how the buckets map to real values.

Picking the range represented by the first bucket (called the "unit"
bucket) fully determines the mapping. For example, if we chose the unit
bucket to represents values `[0, 100]`, that determines ranges for all
other buckets, and the histogram as a whole. The last bucket will cover
range range `[1638400, 3276800]` and the histogram `[0, 3276800]`.

An empty histogram is created with the "unit" range set to "1", and is
adjusted depending on what values are added to the histogram.

If a histogram needs to absorb values outside the current range, the
histogram adjusts the "unit" range. The first two buckets are combined,
and a new bucket is added at the end. This means the unit range doubles.
This is repeated until the histogram can absorb the new value.

The histogram stores the number of doublings in 4 bits, which means the
maximum number of doublings is 15. The largest "unit" range is `2^15`,
i.e. 32768. Combined with the 16 buckets, this means the maximum total
range is `1073741824`.

***Note**: It'd be possible to allow wider histograms, but the primary
use case for this is tracking e.g. query timings in `ms`, and 1B ms is
roughly 12 days. You should not have very many queries taking that long.*


histogram sampling
------------------

The buckets are very small, with only a couple bits per bucket, and can
get full easily. The first bucket is only 8 bits, so the maximum value
for the counter is `255`. The last bucket is 23 bits, with a maximum
value if 8388608, but it's also much wider (by factor `2^15`).

The histogram needs to handle buckets getting full, which is done by
sampling. Initially, all data is processed, as if with sample rage 100%.
When any bucket gets full, the sample rate is cut in half, and the
bucket counters are adjusted to match that (divided by 2).

After the sampling rate gets reduced (from 100%), the histogram only
approximates the data distribution.

Similarly to the unit range, the sample rate is encoded in 4 bits. That
means the lowest sample rate is 1/32768.

***Note**: This may need some more thought. It means the smallest bucket
can accept only ~8355840 values. For short queries this may not be
enough. So maybe we should have one less bucket, and use the 8 bits for
sample rate, or something like that? Different tradeoffs. Or maybe it
should be customizable, similar to how we allow setting precision/scale
for numeric.*


## Usage

The extension provides two functions, which you can use to add values into
a histogram:

* `tinyhist_add(tinyhist hist, value double precision)`

* `tinyhist_add(tinyhist hist, values double precision[])`

That is, you can run something like this:

```
CREATE TABLE t (h tinyhist);
INSERT INTO t VALUES (NULL);
UPDATE t SET h = tinyhist_add(h, 123);
UPDATE t SET h = tinyhist_add(h, ARRAY[456, 789]);
```

The extension also adds `+` operators backed by these functions, so you
may do this instead:

```
UPDATE t SET h = h + 123;
UPDATE t SET h = h + ARRAY[456, 789];
```

There's also an aggregate function `tinyhist_agg`, which aggregates
values into a histogram:

```
SELECT tinyhist_agg(random() * 10000) FROM generate_series(1,10000);
```

*Note*: At the moment there are no functions/operators to combine
histograms, but adding those would be fairly straightforward.

The default output format of a histogram is not very readable (it's
just the raw counters etc.), so there's a set-returning function to
show a more readable version:

```
WITH hist AS (SELECT tinyhist_agg(random() * 10000) AS h FROM generate_series(1,10000))
SELECT * FROM tinyhist_buckets((SELECT h FROM hist));

 bucket_index | bucket_lower | bucket_upper | bucket_range | bucket_count | bucket_frac |  bucket_density   
--------------+--------------+--------------+--------------+--------------+-------------+-------------------
            0 |            0 |            1 |            1 |           14 |       0.014 |             0.014
            1 |            1 |            2 |            1 |            4 |       0.004 |             0.004
            2 |            2 |            4 |            2 |            4 |       0.004 |             0.002
            3 |            4 |            8 |            4 |            8 |       0.008 |             0.002
            4 |            8 |           16 |            8 |            7 |       0.007 |          0.000875
            5 |           16 |           32 |           16 |           15 |       0.015 |         0.0009375
            6 |           32 |           64 |           32 |           20 |        0.02 |          0.000625
            7 |           64 |          128 |           64 |           25 |       0.025 |       0.000390625
            8 |          128 |          256 |          128 |           50 |        0.05 |       0.000390625
            9 |          256 |          512 |          256 |           53 |       0.053 |     0.00020703125
           10 |          512 |         1024 |          512 |          106 |       0.106 |     0.00020703125
           11 |         1024 |         2048 |         1024 |          155 |       0.155 |   0.0001513671875
           12 |         2048 |         4096 |         2048 |          195 |       0.195 |   9.521484375e-05
           13 |         4096 |         8192 |         4096 |          247 |       0.247 |  6.0302734375e-05
           14 |         8192 |        16384 |         8192 |           97 |       0.097 | 1.18408203125e-05
           15 |        16384 |        32768 |        16384 |            0 |           0 |                 0
```


Notes
-----

At the moment, the extension only supports `double precision` values, but
it should not be very difficult to extend it to other numeric types (e.g.
integer and/or floating point). The ranges etc. should however remain int,
for efficiency reasons, so it's probably simpler to just cast values when
building the hisgogram.


License
-------
This software is distributed under the terms of PostgreSQL license.
See LICENSE or http://www.opensource.org/licenses/bsd-license.php for
more details.
