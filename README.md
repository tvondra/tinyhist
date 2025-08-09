# tinyhist extension

[![make installcheck](https://github.com/tvondra/tinyhist/actions/workflows/ci.yml/badge.svg)](https://github.com/tvondra/tinyhist/actions/workflows/ci.yml)

> :warning: **Warning**: This extension is still an early WIP version, not
> suitable for production use. The on-disk format, function signatures etc.
> may change and so on.

This PostgreSQL extension implements `tinyhist`, a data structure for
on-line accumulation of approximate histograms. The algorithm is very
friendly to parallel programs, fully mergeable, etc.

Each histogram is only 32B, and has 16 buckets, with each bucket being
twice as wide (range of values it covers) as the preceding bucket. This
means the accuracy is better for lower values, and the smaller buckets
may also use less space.

Each bucket gets 1 more bit, compared to the preceding bucket, so the
maximum value of the bucket counter doubles (which aligns with the range
doubling).

The bucket ranges and sizes look like this:

```
0 =>	[0,1]			8 bits
1 =>	(1,2]			9 bits
2 =>	(2,4]			10 bits
3 =>	(4,8]			11 bits
3 =>	(4,8]			11 bits
...
15 =>	(16384,32768]	23 bits
```

histogram range
---------------

If the histogram needs to store values outside this "initial" range, it
merges buckets at the beginning, until the last bucket gets wide enough
for the value. Each step effectively doubles the range covered by the
histogram, but it also incrases the size of the initial bucket.

It's possible to do up to 15 such "doublings", in which case the first
bucket is `[0,32768]`, and the total range is `1073741824`.

*Note*: It might be possible to allow wider histograms, but the primary
use case for this is tracking e.g. query timings in `ms`, and 1B ms is
roughly 12 days. You should not have very many queries taking that long.


histogram sampling
------------------

The buckets are very small, and can get full easily. The first bucket
is only 8 bits, which means the counter range is `[0,255]`. The largest
bucket is 23 bits, so the maximum value is 8388608, but it's also much
wider (by the same factor - buckets double and get 1 more bit).

If any bucket gets full, the histogram starts sampling data (randomly).
Initially all data is added (100% sample rate), and every time a bucket
gets full the sample rate is cut in half.

This can repeat 15x, so we sample 100%, 50%, 25%, 12.5%, .... 1/32768.

*Note*: This may need some more thought, because it means the smallest
bucket can accept only ~8355840 values. For short queries this may not
be enough. So maybe we should one less bucket, and use the 8 bits for
sample rate, or something like that? Different tradeoff.


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
multiple histograms, but adding those would be fairly straightforward.

The default output format of a histogram is not very readable (it's
just the raw counters etc.), so there's a set-returning function to
show a more readable version:

```
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
integer and/or floating point). The ranges etc. will however remain int,
for efficiency reasons.


License
-------
This software is distributed under the terms of PostgreSQL license.
See LICENSE or http://www.opensource.org/licenses/bsd-license.php for
more details.
