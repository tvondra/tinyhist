/*
 * tinyhist - implementation of small histogram data type for PostgreSQL
 *
 * Copyright (C) Tomas Vondra, 2025
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <float.h>

#include "postgres.h"
#include "access/htup_details.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "funcapi.h"

PG_MODULE_MAGIC;

#define HISTOGRAM_BUCKETS	16

static int bucket_bits[]   = {8, 9, 10, 11, 12, 13, 14, 15, 16,  17,  18,  19,  20,  21,  22,  23};
static int bucket_offset[] = {0, 8, 17, 27, 38, 50, 63, 77, 92, 108, 125, 143, 162, 182, 203, 225};

/* 32B */
typedef struct tinyhist_t {
	uint8		sample:4;		/* sampling rate for buckets (2^sample) */
	uint8		unit:4;			/* size of the smallest large (2^unit) */
	uint8		data[31];		/* buffer storing the buckets */
} tinyhist_t;

/* prototypes */
PG_FUNCTION_INFO_V1(tinyhist_accum);
PG_FUNCTION_INFO_V1(tinyhist_add);
PG_FUNCTION_INFO_V1(tinyhist_add_array);
PG_FUNCTION_INFO_V1(tinyhist_buckets);
PG_FUNCTION_INFO_V1(tinyhist_info);

PG_FUNCTION_INFO_V1(tinyhist_in);
PG_FUNCTION_INFO_V1(tinyhist_out);
PG_FUNCTION_INFO_V1(tinyhist_send);
PG_FUNCTION_INFO_V1(tinyhist_recv);
PG_FUNCTION_INFO_V1(tinyhist_combine);

Datum tinyhist_accum(PG_FUNCTION_ARGS);
Datum tinyhist_add(PG_FUNCTION_ARGS);
Datum tinyhist_add_array(PG_FUNCTION_ARGS);
Datum tinyhist_buckets(PG_FUNCTION_ARGS);
Datum tinyhist_info(PG_FUNCTION_ARGS);

Datum tinyhist_in(PG_FUNCTION_ARGS);
Datum tinyhist_out(PG_FUNCTION_ARGS);
Datum tinyhist_send(PG_FUNCTION_ARGS);
Datum tinyhist_recv(PG_FUNCTION_ARGS);
Datum tinyhist_combine(PG_FUNCTION_ARGS);

/*
 * histogram_bucket_get
 *		returns the count for a specified histogram bucket
 *
 * XXX Copy the appropriate bits from the bitmap. This implementation is rather
 * naive, working bit-by-bit. There probably is some smart way to do this by
 * manipulating larger bitstrings. Left as a future optimization.
 */
static int32
bucket_get(tinyhist_t *hist, int bucket)
{
	int		nbits = bucket_bits[bucket];
	int		offset = bucket_offset[bucket];
	int		value = 0;

	Assert((bucket >= 0) && (bucket < HISTOGRAM_BUCKETS));

	for (int i = 0; i < nbits; i++)
	{
		int	byte = (offset + i) / 8;
		int	bit = (offset + i) % 8;

		if (hist->data[byte] & (0x1 << bit))
			value |= (0x1 << i);
	}

	return value;
}

/*
 * histogram_bucket_set
 *		stores the count into a given histogram bucket
 *
 * XXX Copy the appropriate bits to the bitmap. This implementation is rather
 * naive, working bit-by-bit. There probably is some smart way to do this by
 * manipulating larger bitstrings. Left as a future optimization.
 */
static void
bucket_set(tinyhist_t *hist, int bucket, int count)
{
	int		nbits = bucket_bits[bucket];
	int		offset = bucket_offset[bucket];

	Assert((bucket >= 0) && (bucket < HISTOGRAM_BUCKETS));
	Assert(count < (0x1 << nbits));

	for (int i = 0; i < nbits; i++)
	{
		int	byte = (offset + i) / 8;
		int	bit = (offset + i) % 8;

		/* set or reset the bit (to overwrite the current value) */
		if (count & (0x1 << i))
			hist->data[byte] |= (0x1 << bit);
		else
			hist->data[byte] &= ~(0x1 << bit);
	}
}

static int32
bucket_maxcount(int bucket)
{
	return (1L << bucket_bits[bucket]) - 1;
}

/*
 * tinyhist_maxvalue
 *		maximum value the current histogram can accept
 * 
 * Upper boundary of the last histogram bucket.
 */
static int64
hist_maxvalue(tinyhist_t *hist)
{
	return (1L << hist->unit) * (0x1 << (HISTOGRAM_BUCKETS - 1));
}

/*
 * tinyhist_adjust_sample
 *		reduce the sampling frequency (to 1/2 of the current value)
 *
 * XXX If the value is odd, we make a systemic error due to rouding. We
 * should sometime add 1, to be 0.5 on average.
 */
static void
hist_adjust_sample(tinyhist_t *hist)
{
	/* cut all buckets in half */
	for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
	{
		int32	cnt = bucket_get(hist, i);

		bucket_set(hist, i, cnt/2);
	}

	/* divide the sample rate by 2 */
	hist->sample++;
}

static void
hist_adjust_unit(tinyhist_t *hist)
{
	/*
	 * We're ready to adjust the range - merge first buckets and then
	 * shift the rest. And reset the last bucket.
	 */
	bucket_set(hist, 0, bucket_get(hist, 0) + bucket_get(hist, 1));

	for (int i = 1; i < (HISTOGRAM_BUCKETS - 1); i++)
	{
		bucket_set(hist, i, bucket_get(hist, i + 1));
	}

	bucket_set(hist, (HISTOGRAM_BUCKETS - 1), 0);

	hist->unit++;
}

/*
 * tinyhist_adjust_range
 *		adjust the histogram range to accept value
 *
 * We double he size of the "unit" (smallest range) until the range gets
 * wide enough for the provided value. This requires merging the first two
 * buckets, and shifting the buckets, possibly multiple times. This means
 * some of the buckets may get full / would not fit anymore, and we solve
 * that by reducing the sample size (which divides bucket counts by two).
 */
static void
hist_adjust_range(tinyhist_t *hist, double value)
{
	/* repeat until the histogram can accept the value */
	while (hist_maxvalue(hist) < value)
	{
		/*
		 * We'll merge the first two buckets, and shift the other buckets
		 * to the left. Check we can do that, or whether we need to adjust
		 * the sample rate.
		 *
		 * XXX Do we need to do this multiple times? I don't think so, after
		 * reducing the sample rate once it should be done.
		 */
		while (true)
		{
			bool adjust_sample = false;

			/* Can we merge the first two buckets? */
			if (bucket_get(hist, 0) + bucket_get(hist, 1) >= bucket_maxcount(0))
				adjust_sample = true;

			/* Can we shift the remaining buckets? */
			for (int i = 1; i < (HISTOGRAM_BUCKETS - 1); i++)
			{
				if (bucket_get(hist, i+1) >= bucket_maxcount(i))
					adjust_sample = true;
			}

			/* don't need to adjust the sampling rate, we're done */
			if (!adjust_sample)
				break;

			hist_adjust_sample(hist);
		}

		hist_adjust_unit(hist);
	}
}

/*
 * bucket_index
 *		calculate bucket index for the value
 *
 * XXX Should only be called after ensuring the range is wide enough.
 */
static int
bucket_index(tinyhist_t *hist, double value)
{
	int			idx;
	int64		unit = (1L << hist->unit);

	Assert(hist_maxvalue(hist) >= value);

	idx = 0;
	while ((1 << idx) * unit < value)
		idx++;

	Assert(idx < HISTOGRAM_BUCKETS);

	return idx;
}

/*
 * hist_sample
 *		Determine if the next value should be added to the histgram.
 *
 * Randomly sample with 1/pow(2,sample) rate.
 */
static bool
hist_sample(tinyhist_t *hist)
{
	int64	s = ((1L << hist->sample) - 1);
	int64	r = random();

	/* sample if the lowest hist->sample bits are 0 */
	return ((r & s) == 0);
}

/*
 * Add a value to the histogram (create one if needed). Transition function
 * for tinyhist aggregate.
 */
Datum
tinyhist_accum(PG_FUNCTION_ARGS)
{
	tinyhist_t *state;
	double		value;

	MemoryContext aggcontext;

	/* cannot be called directly because of internal-type argument */
	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "tinyhist_add called in non-aggregate context");

	/*
	 * We want to skip NULL values altogether - we return either the existing
	 * histogram (if it already exists) or NULL.
	 */
	if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();

		/* if there already is a state accumulated, don't forget it */
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	}

	/* if there's no histogram aggstate allocated, create it now */
	if (PG_ARGISNULL(0))
	{
		MemoryContext	oldcontext;

		oldcontext = MemoryContextSwitchTo(aggcontext);

		state = palloc0(sizeof(tinyhist_t));

		MemoryContextSwitchTo(oldcontext);
	}
	else
		state = (tinyhist_t *) PG_GETARG_POINTER(0);

	value = PG_GETARG_FLOAT8(1);

	/* sample this value? */
	if (hist_sample(state))
	{
		int	bucket;

		/* if needed, increase the range covered by the histogram */
		hist_adjust_range(state, value);

		/* after ensuring sufficient range */
		bucket = bucket_index(state, value);

		/* if the bucket is already full, reduce the sampling rate */
		if (bucket_get(state, bucket) == bucket_maxcount(bucket))
			hist_adjust_sample(state);

		/*
		 * increment the bucket
		 *
		 * XXX shouldn't we resample the value (if we adjusted the sample)?
		 */
		bucket_set(state, bucket, bucket_get(state, bucket) + 1);
	}

	PG_RETURN_POINTER(state);
}

/*
 * Add a value to the histogram (create one if needed). Transition function
 * for histogram aggregate.
 */
Datum
tinyhist_add(PG_FUNCTION_ARGS)
{
	tinyhist_t *state;
	double		value;

	/*
	 * We want to skip NULL values altogether - we return either the existing
	 * histogram (if it already exists) or NULL.
	 */
	if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();

		/* if there already is a state accumulated, don't forget it */
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	}

	state = palloc0(sizeof(tinyhist_t));

	if (!PG_ARGISNULL(0))
	{
		memcpy(state, (tinyhist_t *) PG_GETARG_POINTER(0), sizeof(tinyhist_t));
	}

	value = PG_GETARG_FLOAT8(1);

	/* */
	if (hist_sample(state))
	{
		int	bucket;

		/* if needed, increase the range covered by the histogram */
		hist_adjust_range(state, value);

		bucket = bucket_index(state, value);

		/* if the bucket is already full, reduce the sampling rate */
		if (bucket_get(state, bucket) == bucket_maxcount(bucket))
			hist_adjust_sample(state);

		/*
		 * increment the bucket
		 *
		 * XXX shouldn't we resample the value (if we adjusted the sample)?
		 */
		bucket_set(state, bucket, bucket_get(state, bucket) + 1);
	}

	PG_RETURN_POINTER(state);
}

/*
 * Add an array of values to the histogram (create one if needed). Transition function
 * for histogram aggregate.
 */
Datum
tinyhist_add_array(PG_FUNCTION_ARGS)
{
	tinyhist_t *state;
	ArrayType  *array;
	Datum	   *values;
	bool	   *nulls;
	int			nvalues;

	/*
	 * We want to skip NULL values altogether - we return either the existing
	 * histogram (if it already exists) or NULL.
	 */
	if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();

		/* if there already is a state accumulated, don't forget it */
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	}

	state = palloc0(sizeof(tinyhist_t));

	if (!PG_ARGISNULL(0))
	{
		memcpy(state, (tinyhist_t *) PG_GETARG_POINTER(0), sizeof(tinyhist_t));
	}

	array = PG_GETARG_ARRAYTYPE_P(1);

	deconstruct_array(array, FLOAT8OID,
	/* hard-wired info on type float8 */
					  sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE,
					  &values,
					  &nulls,
					  &nvalues);

	for (int i = 0; i < nvalues; i++)
	{
		double	value;

		/* ignore NULL values */
		if (nulls[i])
			continue;

		value = DatumGetFloat8(values[i]);

		if (hist_sample(state))
		{
			int bucket;

			/* if needed, increase the range covered by the histogram */
			hist_adjust_range(state, value);

			bucket = bucket_index(state, value);

			/* if the bucket is already full, reduce the sampling rate */
			if (bucket_get(state, bucket) == bucket_maxcount(bucket))
				hist_adjust_sample(state);

			/*
			 * increment the bucket
			 *
			 * XXX shouldn't we resample the value (if we adjusted the sample)?
			 */
			bucket_set(state, bucket, bucket_get(state, bucket) + 1);
		}
	}

	PG_RETURN_POINTER(state);
}

Datum
tinyhist_in(PG_FUNCTION_ARGS)
{
	int			r;
	char	   *str = PG_GETARG_CSTRING(0);
	tinyhist_t  *hist = palloc0(sizeof(tinyhist_t));
	int			buckets[HISTOGRAM_BUCKETS];
	uint8		sample;
	uint8		unit;

	r = sscanf(str, "{%hhu, %hhu, %d, %d, %d, %d, %d, %d, %d, %d, "
					 "%d, %d, %d, %d, %d, %d, %d, %d}",
			   &sample, &unit,
			   &buckets[0],  &buckets[1],  &buckets[2],  &buckets[3],  &buckets[4],
			   &buckets[5],  &buckets[6],  &buckets[7],  &buckets[8],  &buckets[9],
			   &buckets[10], &buckets[11], &buckets[12], &buckets[13], &buckets[14],
			   &buckets[15]);

	if (r != 32)
		elog(ERROR, "failed to parse tinyhist value");

	hist->sample = sample;
	hist->unit = unit;

	for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
	{
		bucket_set(hist, i, buckets[i]);
	}

	PG_RETURN_POINTER(hist);
}

Datum
tinyhist_out(PG_FUNCTION_ARGS)
{
	tinyhist_t  *hist = (tinyhist_t *) (PG_GETARG_POINTER(0));
	StringInfoData	str;
	int			buckets[HISTOGRAM_BUCKETS];

	for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
	{
		buckets[i] = bucket_get(hist, i);
	}

	initStringInfo(&str);

	appendStringInfo(&str, "{%hhu, %hhu, %d, %d, %d, %d, %d, %d, %d, %d, "
					 "%d, %d, %d, %d, %d, %d, %d, %d}",
					 hist->sample, hist->unit,
					 buckets[0],  buckets[1],  buckets[2],  buckets[3],  buckets[4],
					 buckets[5],  buckets[6],  buckets[7],  buckets[8],  buckets[9],
					 buckets[10], buckets[11], buckets[12], buckets[13], buckets[14],
					 buckets[15]);

	PG_RETURN_CSTRING(str.data);
}

Datum
tinyhist_send(PG_FUNCTION_ARGS)
{
	tinyhist_t  *hist = (tinyhist_t *) PG_GETARG_POINTER(0);
	StringInfoData buf;
	int			i;

	pq_begintypsend(&buf);

	pq_sendbyte(&buf, hist->sample);

	for (i = 0; i < HISTOGRAM_BUCKETS; i++)
	{
		pq_sendbyte(&buf, bucket_get(hist, i));
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
tinyhist_recv(PG_FUNCTION_ARGS)
{
	int i;
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	tinyhist_t  *hist = palloc0(sizeof(tinyhist_t));

	hist->sample = pq_getmsgbyte(buf);

	for (i = 0; i < HISTOGRAM_BUCKETS; i++)
	{
		bucket_set(hist, i, pq_getmsgbyte(buf));
	}

	PG_RETURN_POINTER(hist);
}

Datum
tinyhist_combine(PG_FUNCTION_ARGS)
{
	tinyhist_t	   *src;
	tinyhist_t	   *dst;

	int				unit;
	int				sample;

	MemoryContext aggcontext;
	MemoryContext oldcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "tinyhist_combine called in non-aggregate context");

	/* the second parameter must not be NULL */
	Assert(!PG_ARGISNULL(1));

	/* so just grab it */
	src = (tinyhist_t *) PG_GETARG_POINTER(1);

	/* when NULL in the first parameter, just return a copy of the second one */
	if (PG_ARGISNULL(0))
	{
		tinyhist_t *tmp;

		/* copy the histogram into the right long-lived memory context */
		oldcontext = MemoryContextSwitchTo(aggcontext);
		tmp = palloc0(sizeof(tinyhist_t));
		memcpy(tmp, src, sizeof(tinyhist_t));
		MemoryContextSwitchTo(oldcontext);

		PG_RETURN_POINTER(tmp);
	}

	dst = (tinyhist_t *) PG_GETARG_POINTER(0);

	/*
	 * Equalize the two histograms, i.e. make sure they have the same
	 * unit and sample rate.
	 *
	 * XXX Should we do this in a particular order? E.g. unit first and
	 * then sample rate, or the other way around? Or it doesn't matter?
	 */
	sample = Max(src->sample, dst->sample);

	while (src->sample < sample)
		hist_adjust_sample(src);

	while (dst->sample < sample)
		hist_adjust_sample(dst);

	unit = Max(src->unit, dst->unit);

	while (src->unit < unit)
		hist_adjust_unit(src);

	while (dst->unit < unit)
		hist_adjust_unit(dst);

	Assert(src->sample == dst->sample);
	Assert(src->unit == dst->unit);

	/*
	 * Now check we can merge the histograms, with all counts fitting into
	 * the buckets. If not, adjust the sample once more.
	 */
	{
		bool	adjust_sample = false;

		for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
		{
			int	cnt = bucket_get(src,i) + bucket_get(dst,i);

			if (cnt > bucket_maxcount(i))
			{
				adjust_sample = true;
				break;
			}
		}

		if (adjust_sample)
		{
			hist_adjust_sample(src);
			hist_adjust_sample(dst);
		}
	}

	/* OK, time to do the merge */
	for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
	{
		int	cnt = bucket_get(src,i) + bucket_get(dst,i);

		bucket_set(dst, i, cnt);
	}

	PG_RETURN_POINTER(dst);
}

static TupleDesc
tinyhist_info_tupledesc(void)
{
	TupleDesc	tupdesc;
	AttrNumber	a = 0;

	tupdesc = CreateTemplateTupleDesc(4);

	TupleDescInitEntry(tupdesc, ++a, "hist_unit", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "hist_sample", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "hist_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, ++a, "hist_upper", INT8OID, -1, 0);

	return BlessTupleDesc(tupdesc);
}


/*
 * tinyhist_info
 *		information about a single histogram
 */
Datum
tinyhist_info(PG_FUNCTION_ARGS)
{
	tinyhist_t *hist = (tinyhist_t *) PG_GETARG_POINTER(0);
	Datum		values[4];
	bool		nulls[4] = {0};
	int64		count = 0;
	TupleDesc	tupdesc = tinyhist_info_tupledesc();

	for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
		count += bucket_get(hist, i);

	values[0] = Int32GetDatum(1 << hist->unit);
	values[1] = Int32GetDatum(1 << hist->sample);
	values[2] = Int64GetDatum(count);
	values[3] = Int64GetDatum((1 << 15) * (1 << hist->unit));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}


/*
 * tinyhist_buckets
 *		information about buckets of a histogram
 */
Datum
tinyhist_buckets(PG_FUNCTION_ARGS)
{
	tinyhist_t *hist = (tinyhist_t *) PG_GETARG_POINTER(0);
	FuncCallContext *fctx;
	TupleDesc		tupdesc;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext mctx;

		fctx = SRF_FIRSTCALL_INIT();

		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		fctx->user_fctx = tupdesc;
		fctx->max_calls = HISTOGRAM_BUCKETS;

		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();

	if (fctx->call_cntr < fctx->max_calls)
	{
		HeapTuple	resultTuple;
		Datum		result;
		Datum		values[7];
		bool		nulls[7];
		double		lower,
					upper,
					range;
		double		density = pow(2.0, hist->sample);
		double		total = 0;
		double		unit = pow(2.0, hist->unit);
		int			cnt;

		tupdesc = fctx->user_fctx;

		memset(nulls, 0, sizeof(nulls));

		lower = 0;
		if (fctx->call_cntr > 0)
			lower = 1L << (hist->unit + (fctx->call_cntr - 1));

		upper = 1L << (hist->unit + fctx->call_cntr);

		range = (upper - lower);

		total = 0;
		for (int i = 0; i < HISTOGRAM_BUCKETS; i++)
			total += bucket_get(hist, i);

		cnt = bucket_get(hist, fctx->call_cntr);

		/* Extract information from the line pointer */
		values[0] = Int32GetDatum(fctx->call_cntr);
		values[1] = Float8GetDatum(lower);
		values[2] = Float8GetDatum(upper);
		values[3] = Float8GetDatum(range);
		values[4] = Float8GetDatum(cnt * density);
		values[5] = Float8GetDatum(cnt / total);
		values[6] = Float8GetDatum(cnt / (total * range / unit));

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(tupdesc, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		SRF_RETURN_NEXT(fctx, result);
	}
	else
		SRF_RETURN_DONE(fctx);
}
