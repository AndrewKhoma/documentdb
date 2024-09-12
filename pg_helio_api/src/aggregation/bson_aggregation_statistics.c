/*-------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation.  All rights reserved.
 *
 * src/aggregation/bson_aggregation_statistics.c
 *
 * Implementation of the aggregate functions typically used in
 * statistical analysis (such as covariance, standard deviation, etc.)
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>
#include <fmgr.h>
#include <windowapi.h>
#include <executor/nodeWindowAgg.h>
#include <executor/executor.h>
#include <catalog/pg_type.h>
#include <math.h>
#include <nodes/pg_list.h>

#include "utils/mongo_errors.h"
#include "types/decimal128.h"


/* --------------------------------------------------------- */
/* Data-types */
/* --------------------------------------------------------- */

/* state used for pop and sample variance/covariance both */
/* for variance calculation x = y */
typedef struct BsonCovarianceAndVarianceAggState
{
	bson_value_t sx;
	bson_value_t sy;
	bson_value_t sxy;
	bson_value_t count; /* TODO: change to int64 */

	/* number of decimal values in current window, used to determine if we need to return decimal128 value */
	int decimalCount;
} BsonCovarianceAndVarianceAggState;

typedef struct BsonExpMovingAvg
{
	bool init;
	bool isAlpha;
	bson_value_t weight;
	bson_value_t preValue;
} BsonExpMovingAvg;

enum InputValidFlags
{
	InputValidFlags_Unknown = 0,
	InputValidFlags_N = 1,
	InputValidFlags_Alpha = 2,
	InputValidFlags_Input = 4
};

typedef struct BsonIntegralAndDerivativeAggState
{
	/* The result value of current window */
	bson_value_t result;

	/* anchorX/anchorY is an anchor point for calculating integral or derivative.
	 * For $integral, anchorX is updated to the previous document of the window,
	 * For $derivative, anchorX is always updated to the first document of the window.
	 */
	bson_value_t anchorX;
	bson_value_t anchorY;
} BsonIntegralAndDerivativeAggState;

/* --------------------------------------------------------- */
/* Forward declaration */
/* --------------------------------------------------------- */

#define GENERATE_ERROR_MSG(ErrorMessage, Element1, Element2) \
	ErrorMessage \
	# Element1 " = %s, " # Element2 " = %s", \
	BsonValueToJsonForLogging(&(Element1)), BsonValueToJsonForLogging(&(Element2))

#define HANDLE_DECIMAL_OP_ERROR(OperatorFuncName, Element1, Element2, Result, \
								ErrorMessage) \
	do { \
		Decimal128Result decimalOpResult = (OperatorFuncName) (&(Element1), &(Element2), \
															   &(Result)); \
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult != \
			Decimal128Result_Inexact) { \
			ereport(ERROR, (errcode(MongoInternalError)), \
					errmsg(GENERATE_ERROR_MSG(ErrorMessage, Element1, Element2))); \
		} \
	} while (0)

bool ParseInputWeightForExpMovingAvg(const bson_value_t *opValue,
									 bson_value_t *inputExpression,
									 bson_value_t *weightExpression,
									 bson_value_t *decimalWeightValue);

static bytea * AllocateBsonCovarianceOrVarianceAggState(void);
static void CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr(const
																  BsonCovarianceAndVarianceAggState
																  *leftState, const
																  BsonCovarianceAndVarianceAggState
																  *rightState,
																  BsonCovarianceAndVarianceAggState
																  *currentState);
static void CalculateInvFuncForCovarianceOrVarianceWithYCAlgr(const
															  bson_value_t *newXValue,
															  const bson_value_t *
															  newYValue,
															  BsonCovarianceAndVarianceAggState
															  *currentState);
static void CalculateSFuncForCovarianceOrVarianceWithYCAlgr(const bson_value_t *newXValue,
															const bson_value_t *newYValue,
															BsonCovarianceAndVarianceAggState
															*currentState);
static bool CalculateExpMovingAvg(bson_value_t *currentValue, bson_value_t *perValue,
								  bson_value_t *weightValue, bool isAlpha,
								  bson_value_t *resultValue);
static bytea * AllocateBsonIntegralAndDerivativeAggState(void);

static void HandleIntegralDerivative(bson_value_t *xBsonValue, bson_value_t *yBsonValue,
									 long timeUnitInMs,
									 BsonIntegralAndDerivativeAggState *currentState,
									 const bool isIntegralOperator);
static void RunTimeCheckForIntegralAndDerivative(bson_value_t *xBsonValue,
												 bson_value_t *yBsonValue,
												 long timeUnitInMs,
												 const bool isIntegralOperator);
static bool IntegralOfTwoPointsByTrapezoidalRule(bson_value_t *xValue,
												 bson_value_t *yValue,
												 BsonIntegralAndDerivativeAggState *
												 currentState,
												 bson_value_t *timeUnitInMs);
static bool DerivativeOfTwoPoints(bson_value_t *xValue, bson_value_t *yValue,
								  BsonIntegralAndDerivativeAggState *currentState,
								  bson_value_t *timeUnitInMs);
static void CalculateSqrtForStdDev(const bson_value_t *inputResult,
								   bson_value_t *outputResult);

/* --------------------------------------------------------- */
/* Top level exports */
/* --------------------------------------------------------- */

PG_FUNCTION_INFO_V1(bson_covariance_pop_samp_transition);
PG_FUNCTION_INFO_V1(bson_covariance_pop_samp_combine);
PG_FUNCTION_INFO_V1(bson_covariance_pop_samp_invtransition);
PG_FUNCTION_INFO_V1(bson_covariance_pop_final);
PG_FUNCTION_INFO_V1(bson_covariance_samp_final);
PG_FUNCTION_INFO_V1(bson_std_dev_pop_samp_transition);
PG_FUNCTION_INFO_V1(bson_std_dev_pop_samp_combine);
PG_FUNCTION_INFO_V1(bson_std_dev_pop_final);
PG_FUNCTION_INFO_V1(bson_std_dev_samp_final);
PG_FUNCTION_INFO_V1(bson_exp_moving_avg);
PG_FUNCTION_INFO_V1(bson_derivative_transition);
PG_FUNCTION_INFO_V1(bson_integral_transition);
PG_FUNCTION_INFO_V1(bson_integral_derivative_final);
PG_FUNCTION_INFO_V1(bson_std_dev_pop_samp_winfunc_invtransition);
PG_FUNCTION_INFO_V1(bson_std_dev_pop_winfunc_final);
PG_FUNCTION_INFO_V1(bson_std_dev_samp_winfunc_final);

/*
 * Transition function for the BSONCOVARIANCEPOP and BSONCOVARIANCESAMP aggregate.
 * Use the Youngs-Cramer algorithm to incorporate the new value into the
 * transition values.
 * If calculating variance, we use X and Y as the same value.
 */
Datum
bson_covariance_pop_samp_transition(PG_FUNCTION_ARGS)
{
	bytea *bytes;
	BsonCovarianceAndVarianceAggState *currentState;

	/* If the intermediate state has never been initialized, create it */
	if (PG_ARGISNULL(0))
	{
		MemoryContext aggregateContext;
		if (AggCheckCallContext(fcinfo, &aggregateContext) != AGG_CONTEXT_WINDOW)
		{
			ereport(ERROR, errmsg(
						"window aggregate function called in non-window-aggregate context"));
		}

		/* Create the aggregate state in the aggregate context. */
		MemoryContext oldContext = MemoryContextSwitchTo(aggregateContext);

		bytes = AllocateBsonCovarianceOrVarianceAggState();

		currentState = (BsonCovarianceAndVarianceAggState *) VARDATA(bytes);
		currentState->sx.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->sx);
		currentState->sy.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->sy);
		currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->sxy);
		currentState->count.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->count);
		currentState->decimalCount = 0;

		MemoryContextSwitchTo(oldContext);
	}
	else
	{
		bytes = PG_GETARG_BYTEA_P(0);
		currentState = (BsonCovarianceAndVarianceAggState *) VARDATA_ANY(bytes);
	}

	pgbson *currentXValue = PG_GETARG_MAYBE_NULL_PGBSON(1);
	pgbson *currentYValue = PG_GETARG_MAYBE_NULL_PGBSON(2);

	if (currentXValue == NULL || IsPgbsonEmptyDocument(currentXValue) || currentYValue ==
		NULL || IsPgbsonEmptyDocument(currentYValue))
	{
		PG_RETURN_POINTER(bytes);
	}

	pgbsonelement currentXValueElement;
	PgbsonToSinglePgbsonElement(currentXValue, &currentXValueElement);
	pgbsonelement currentYValueElement;
	PgbsonToSinglePgbsonElement(currentYValue, &currentYValueElement);

	/* we should ignore numeric values */
	if (BsonValueIsNumber(&currentXValueElement.bsonValue) && BsonValueIsNumber(
			&currentYValueElement.bsonValue))
	{
		CalculateSFuncForCovarianceOrVarianceWithYCAlgr(&currentXValueElement.bsonValue,
														&currentYValueElement.bsonValue,
														currentState);
	}

	PG_RETURN_POINTER(bytes);
}


/*
 * Applies the "combine function" (COMBINEFUNC) for BSONCOVARIANCEPOP and BSONCOVARIANCESAMP.
 * takes two of the aggregate state structures (BsonCovarianceAndVarianceAggState)
 * and combines them to form a new BsonCovarianceAndVarianceAggState that has the combined
 * sum and count.
 */
Datum
bson_covariance_pop_samp_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggregateContext;
	if (AggCheckCallContext(fcinfo, &aggregateContext) != AGG_CONTEXT_WINDOW)
	{
		ereport(ERROR, errmsg(
					"window aggregate function called in non-window-aggregate context"));
	}

	/* Create the aggregate state in the aggregate context. */
	MemoryContext oldContext = MemoryContextSwitchTo(aggregateContext);

	bytea *combinedStateBytes = AllocateBsonCovarianceOrVarianceAggState();
	BsonCovarianceAndVarianceAggState *currentState =
		(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(combinedStateBytes);

	MemoryContextSwitchTo(oldContext);

	/* Handle either left or right being null. A new state needs to be allocated regardless */
	currentState->sx.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->sx);
	currentState->sy.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->sy);
	currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->sxy);
	currentState->count.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->count);
	currentState->decimalCount = 0;

	/* handle left or right being null */
	/* It's worth handling the special cases N1 = 0 and N2 = 0 separately */
	/* since those cases are trivial, and we then don't need to worry about */
	/* division-by-zero errors in the general case. */
	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
		{
			PG_RETURN_NULL();
		}
		memcpy(VARDATA(combinedStateBytes), VARDATA_ANY(PG_GETARG_BYTEA_P(1)),
			   sizeof(BsonCovarianceAndVarianceAggState));
	}
	else if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
		{
			PG_RETURN_NULL();
		}
		memcpy(VARDATA(combinedStateBytes), VARDATA_ANY(PG_GETARG_BYTEA_P(0)),
			   sizeof(BsonCovarianceAndVarianceAggState));
	}
	else
	{
		BsonCovarianceAndVarianceAggState *leftState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(PG_GETARG_BYTEA_P(0));
		BsonCovarianceAndVarianceAggState *rightState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(PG_GETARG_BYTEA_P(1));

		CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr(leftState, rightState,
															  currentState);
	}

	PG_RETURN_POINTER(combinedStateBytes);
}


/*
 * Applies the "inverse transition function" (MINVFUNC) for BSONCOVARIANCEPOP and BSONCOVARIANCESAMP.
 * takes one aggregate state structures (BsonCovarianceAndVarianceAggState)
 * and single data point. Remove the single data from BsonCovarianceAndVarianceAggState
 */
Datum
bson_covariance_pop_samp_invtransition(PG_FUNCTION_ARGS)
{
	MemoryContext aggregateContext;
	if (AggCheckCallContext(fcinfo, &aggregateContext) != AGG_CONTEXT_WINDOW)
	{
		ereport(ERROR, errmsg(
					"window aggregate function called in non-window-aggregate context"));
	}

	bytea *bytes;
	BsonCovarianceAndVarianceAggState *currentState;

	if (PG_ARGISNULL(0))
	{
		PG_RETURN_NULL();
	}
	else
	{
		bytes = PG_GETARG_BYTEA_P(0);
		currentState = (BsonCovarianceAndVarianceAggState *) VARDATA_ANY(bytes);
	}

	pgbson *currentXValue = PG_GETARG_MAYBE_NULL_PGBSON(1);
	pgbson *currentYValue = PG_GETARG_MAYBE_NULL_PGBSON(2);

	if (currentXValue == NULL || IsPgbsonEmptyDocument(currentXValue) || currentYValue ==
		NULL || IsPgbsonEmptyDocument(currentYValue))
	{
		PG_RETURN_POINTER(bytes);
	}

	pgbsonelement currentXValueElement;
	PgbsonToSinglePgbsonElement(currentXValue, &currentXValueElement);
	pgbsonelement currentYValueElement;
	PgbsonToSinglePgbsonElement(currentYValue, &currentYValueElement);

	if (!BsonTypeIsNumber(currentXValueElement.bsonValue.value_type) ||
		!BsonTypeIsNumber(currentYValueElement.bsonValue.value_type))
	{
		PG_RETURN_POINTER(bytes);
	}

	/* restart aggregate if NaN or Infinity in current state or current values */
	/* or count is 0 */
	if (IsBsonValueNaN(&currentState->sxy) || IsBsonValueInfinity(&currentState->sxy) ||
		IsBsonValueNaN(&currentXValueElement.bsonValue) || IsBsonValueInfinity(
			&currentXValueElement.bsonValue) ||
		IsBsonValueNaN(&currentYValueElement.bsonValue) || IsBsonValueInfinity(
			&currentYValueElement.bsonValue) ||
		BsonValueAsInt64(&currentState->count) == 0)
	{
		PG_RETURN_NULL();
	}

	CalculateInvFuncForCovarianceOrVarianceWithYCAlgr(&currentXValueElement.bsonValue,
													  &currentYValueElement.bsonValue,
													  currentState);

	PG_RETURN_POINTER(bytes);
}


/*
 * Applies the "final calculation" (FINALFUNC) for BSONCOVARIANCEPOP.
 * This takes the final value created and outputs a bson covariance pop
 * with the appropriate type.
 */
Datum
bson_covariance_pop_final(PG_FUNCTION_ARGS)
{
	bytea *covarianceIntermediateState = PG_ARGISNULL(0) ? NULL : PG_GETARG_BYTEA_P(0);

	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;
	if (covarianceIntermediateState != NULL)
	{
		bson_value_t decimalResult = { 0 };
		decimalResult.value_type = BSON_TYPE_DECIMAL128;
		BsonCovarianceAndVarianceAggState *covarianceState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(
				covarianceIntermediateState);

		if (IsBsonValueNaN(&covarianceState->sxy) || IsBsonValueInfinity(
				&covarianceState->sxy) != 0)
		{
			decimalResult.value.v_decimal128 = covarianceState->sxy.value.v_decimal128;
		}
		else if (BsonValueAsInt64(&covarianceState->count) == 0)
		{
			/* Mongo returns null for empty sets or wrong input field count */
			finalValue.bsonValue.value_type = BSON_TYPE_NULL;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else if (BsonValueAsInt64(&covarianceState->count) == 1)
		{
			/* Mongo returns 0 for single numeric value */
			/* return double even if the value is decimal128 */
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = 0;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else
		{
			HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, covarianceState->sxy,
									covarianceState->count, decimalResult,
									"Failed while calculating bson_covariance_pop_final decimalResult for these values: ");
		}

		/* if there is at least one decimal value in current window, we should return decimal128; otherwise, return double */
		if (covarianceState->decimalCount > 0)
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DECIMAL128;
			finalValue.bsonValue.value.v_decimal128 = decimalResult.value.v_decimal128;
		}
		else
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = BsonValueAsDouble(&decimalResult);
		}
	}
	else
	{
		/* Mongo returns null for empty sets */
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
	}

	PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
}


/*
 * Applies the "final calculation" (FINALFUNC) for BSONCOVARIANCESAMP.
 * This takes the final value created and outputs a bson covariance samp
 * with the appropriate type.
 */
Datum
bson_covariance_samp_final(PG_FUNCTION_ARGS)
{
	bytea *covarianceIntermediateState = PG_ARGISNULL(0) ? NULL : PG_GETARG_BYTEA_P(0);

	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;
	if (covarianceIntermediateState != NULL)
	{
		bson_value_t decimalResult = { 0 };
		decimalResult.value_type = BSON_TYPE_DECIMAL128;
		BsonCovarianceAndVarianceAggState *covarianceState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(
				covarianceIntermediateState);

		if (IsBsonValueNaN(&covarianceState->sxy) || IsBsonValueInfinity(
				&covarianceState->sxy))
		{
			decimalResult.value.v_decimal128 = covarianceState->sxy.value.v_decimal128;
		}
		else if (BsonValueAsInt64(&covarianceState->count) == 0 || BsonValueAsInt64(
					 &covarianceState->count) == 1)
		{
			/* Mongo returns null for empty sets, single numeric value or wrong input field count */
			finalValue.bsonValue.value_type = BSON_TYPE_NULL;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else
		{
			bson_value_t decimalOne;
			decimalOne.value_type = BSON_TYPE_DECIMAL128;
			decimalOne.value.v_decimal128 = GetDecimal128FromInt64(1);

			bson_value_t countMinus1;
			countMinus1.value_type = BSON_TYPE_DECIMAL128;
			HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, covarianceState->count,
									decimalOne, countMinus1,
									"Failed while calculating bson_covariance_samp_final countMinus1 for these values: ");

			HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, covarianceState->sxy,
									countMinus1, decimalResult,
									"Failed while calculating bson_covariance_samp_final decimalResult for these values: ");
		}

		/* if there is at least one decimal value in current window, we should return decimal128; otherwise, return double */
		if (covarianceState->decimalCount > 0)
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DECIMAL128;
			finalValue.bsonValue.value.v_decimal128 = decimalResult.value.v_decimal128;
		}
		else
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = BsonValueAsDouble(&decimalResult);
		}
	}
	else
	{
		/* Mongo returns null for empty sets */
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
	}

	PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
}


/*
 * Transition function for the BSON_STD_DEV_POP and BSON_STD_DEV_SAMP aggregate.
 * Implementation refer to https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/float.c#L2950
 */
Datum
bson_std_dev_pop_samp_transition(PG_FUNCTION_ARGS)
{
	bytea *bytes;
	BsonCovarianceAndVarianceAggState *currentState;

	/* If the intermediate state has never been initialized, create it */
	if (PG_ARGISNULL(0))
	{
		MemoryContext aggregateContext;
		if (!AggCheckCallContext(fcinfo, &aggregateContext))
		{
			ereport(ERROR, errmsg(
						"aggregate function std dev pop sample transition called in non-aggregate context"));
		}

		/* Create the aggregate state in the aggregate context. */
		MemoryContext oldContext = MemoryContextSwitchTo(aggregateContext);

		bytes = AllocateBsonCovarianceOrVarianceAggState();

		currentState = (BsonCovarianceAndVarianceAggState *) VARDATA(bytes);
		currentState->sx.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->sx);
		currentState->sy.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->sy);
		currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->sxy);
		currentState->count.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128Zero(&currentState->count);
		currentState->decimalCount = 0;

		MemoryContextSwitchTo(oldContext);
	}
	else
	{
		bytes = PG_GETARG_BYTEA_P(0);
		currentState = (BsonCovarianceAndVarianceAggState *) VARDATA_ANY(bytes);
	}
	pgbson *currentValue = PG_GETARG_MAYBE_NULL_PGBSON(1);

	if (currentValue == NULL || IsPgbsonEmptyDocument(currentValue))
	{
		PG_RETURN_POINTER(bytes);
	}

	pgbsonelement currentValueElement;
	PgbsonToSinglePgbsonElement(currentValue, &currentValueElement);

	/* mongo ignores non-numeric values */
	if (BsonValueIsNumber(&currentValueElement.bsonValue))
	{
		CalculateSFuncForCovarianceOrVarianceWithYCAlgr(&currentValueElement.bsonValue,
														&currentValueElement.bsonValue,
														currentState);
	}

	PG_RETURN_POINTER(bytes);
}


/*
 * Applies the "combine function" (COMBINEFUNC) for std_dev_pop and std_dev_samp.
 * takes two of the aggregate state structures (bson_std_dev_agg_state)
 * and combines them to form a new bson_std_dev_agg_state that has the combined
 * sum and count.
 */
Datum
bson_std_dev_pop_samp_combine(PG_FUNCTION_ARGS)
{
	MemoryContext aggregateContext;
	if (!AggCheckCallContext(fcinfo, &aggregateContext))
	{
		ereport(ERROR, errmsg("aggregate function called in non-aggregate context"));
	}

	/* Create the aggregate state in the aggregate context. */
	MemoryContext oldContext = MemoryContextSwitchTo(aggregateContext);

	bytea *combinedStateBytes = AllocateBsonCovarianceOrVarianceAggState();
	BsonCovarianceAndVarianceAggState *currentState =
		(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(
			combinedStateBytes);

	MemoryContextSwitchTo(oldContext);

	/* Handle either left or right being null. A new state needs to be allocated regardless */
	currentState->sx.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->sx);
	currentState->sy.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->sy);
	currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->sxy);
	currentState->count.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&currentState->count);
	currentState->decimalCount = 0;

	/*--------------------
	 * The transition values combine using a generalization of the
	 * Youngs-Cramer algorithm as follows:
	 *
	 *	N = N1 + N2
	 *	Sx = Sx1 + Sx2
	 *	Sxx = Sxx1 + Sxx2 + N1 * N2 * (Sx1/N1 - Sx2/N2)^2 / N;
	 *
	 * It's worth handling the special cases N1 = 0 and N2 = 0 separately
	 * since those cases are trivial, and we then don't need to worry about
	 * division-by-zero errors in the general case.
	 *--------------------
	 */

	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
		{
			PG_RETURN_NULL();
		}
		memcpy(VARDATA(combinedStateBytes), VARDATA_ANY(PG_GETARG_BYTEA_P(1)),
			   sizeof(BsonCovarianceAndVarianceAggState));
	}
	else if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
		{
			PG_RETURN_NULL();
		}
		memcpy(VARDATA(combinedStateBytes), VARDATA_ANY(PG_GETARG_BYTEA_P(0)),
			   sizeof(BsonCovarianceAndVarianceAggState));
	}
	else
	{
		BsonCovarianceAndVarianceAggState *leftState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(
				PG_GETARG_BYTEA_P(0));
		BsonCovarianceAndVarianceAggState *rightState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(
				PG_GETARG_BYTEA_P(1));

		CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr(leftState, rightState,
															  currentState);
	}

	PG_RETURN_POINTER(combinedStateBytes);
}


/*
 * Applies the "final calculation" (FINALFUNC) for std_dev_pop.
 * This takes the final value created and outputs a bson "std_dev_pop"
 * with the appropriate type.
 */
Datum
bson_std_dev_pop_final(PG_FUNCTION_ARGS)
{
	bytea *stdDevIntermediateState = PG_ARGISNULL(0) ? NULL : PG_GETARG_BYTEA_P(0);

	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;
	if (stdDevIntermediateState != NULL)
	{
		BsonCovarianceAndVarianceAggState *stdDevState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(
				stdDevIntermediateState);

		if (IsBsonValueNaN(&stdDevState->sxy) || IsBsonValueInfinity(&stdDevState->sxy))
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = NAN;
		}
		else if (BsonValueAsInt64(&stdDevState->count) == 0)
		{
			/* Mongo returns $null for empty sets */
			finalValue.bsonValue.value_type = BSON_TYPE_NULL;
		}
		else if (BsonValueAsInt64(&stdDevState->count) == 1)
		{
			/* Mongo returns 0 for single numeric value */
			finalValue.bsonValue.value_type = BSON_TYPE_INT32;
			finalValue.bsonValue.value.v_int32 = 0;
		}
		else
		{
			bson_value_t result;
			result.value_type = BSON_TYPE_DECIMAL128;
			HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, stdDevState->sxy,
									stdDevState->count, result,
									"Failed while calculating bson_std_dev_pop_final result for these values: ");

			/* The value type of finalValue.bsonValue is set to BSON_TYPE_DOUBLE inside the function*/
			CalculateSqrtForStdDev(&result, &finalValue.bsonValue);
		}
	}
	else
	{
		/* Mongo returns $null for empty sets */
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
	}

	PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
}


/*
 * Applies the "final calculation" (FINALFUNC) for std_dev_samp.
 * This takes the final value created and outputs a bson "std_dev_samp"
 * with the appropriate type.
 */
Datum
bson_std_dev_samp_final(PG_FUNCTION_ARGS)
{
	bytea *stdDevIntermediateState = PG_ARGISNULL(0) ? NULL : PG_GETARG_BYTEA_P(0);

	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;
	if (stdDevIntermediateState != NULL)
	{
		BsonCovarianceAndVarianceAggState *stdDevState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(
				stdDevIntermediateState);

		if (BsonValueAsInt64(&stdDevState->count) == 0 || BsonValueAsInt64(
				&stdDevState->count) == 1)
		{
			/* Mongo returns $null for empty sets or single numeric value */
			finalValue.bsonValue.value_type = BSON_TYPE_NULL;
		}
		else if (IsBsonValueInfinity(&stdDevState->sxy))
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = NAN;
		}
		else
		{
			bson_value_t decimalOne;
			decimalOne.value_type = BSON_TYPE_DECIMAL128;
			decimalOne.value.v_decimal128 = GetDecimal128FromInt64(1);

			bson_value_t countMinus1;
			countMinus1.value_type = BSON_TYPE_DECIMAL128;
			HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, stdDevState->count,
									decimalOne, countMinus1,
									"Failed while calculating bson_std_dev_samp_final countMinus1 for these values: ");

			bson_value_t result;
			result.value_type = BSON_TYPE_DECIMAL128;
			HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, stdDevState->sxy,
									countMinus1, result,
									"Failed while calculating bson_std_dev_samp_final result for these values: ");

			/* The value type of finalValue.bsonValue is set to BSON_TYPE_DOUBLE inside the function*/
			CalculateSqrtForStdDev(&result, &finalValue.bsonValue);
		}
	}
	else
	{
		/* Mongo returns $null for empty sets */
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
	}

	PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
}


/*
 * Function that calculate expMovingAvg value one by one.
 */
Datum
bson_exp_moving_avg(PG_FUNCTION_ARGS)
{
	WindowObject winobj = PG_WINDOW_OBJECT();
	BsonExpMovingAvg *stateData;

	stateData = (BsonExpMovingAvg *)
				WinGetPartitionLocalMemory(winobj, sizeof(BsonExpMovingAvg));

	bool isnull = false;

	pgbson *currentValue = DatumGetPgBson(WinGetFuncArgCurrent(winobj, 0, &isnull));

	pgbsonelement currentValueElement;
	PgbsonToSinglePgbsonElement(currentValue, &currentValueElement);
	bson_value_t bsonCurrentValue = currentValueElement.bsonValue;

	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;

	/* if currentValue is not a numeric type, return null. */
	if (BsonValueIsNumber(&bsonCurrentValue))
	{
		/* first call, init stateData. */
		if (!stateData->init)
		{
			/* get weight value, if isAlpha == true, the weightValue is Alpha, if isAlpha == false, the weightValue is N. */
			pgbson *weightValue = DatumGetPgBson(WinGetFuncArgCurrent(winobj, 1,
																	  &isnull));
			bool isAlpha = DatumGetBool(WinGetFuncArgCurrent(winobj, 2, &isnull));

			pgbsonelement weightValueElement;
			PgbsonToSinglePgbsonElement(weightValue, &weightValueElement);
			bson_value_t bsonWeightValue = weightValueElement.bsonValue;

			stateData->init = true;
			stateData->isAlpha = isAlpha;
			stateData->preValue = bsonCurrentValue;
			stateData->weight = bsonWeightValue;
			finalValue.bsonValue = bsonCurrentValue;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else
		{
			bson_value_t bsonPerValue = stateData->preValue;
			bson_value_t bsonResultValue;

			/*
			 * CalculateExpMovingAvg will compute the result of expMovingAvg.
			 *
			 * If the parameter is N,
			 * the calculation is: current result = current value * ( 2 / ( N + 1 ) ) + previous result * ( 1 - ( 2 / ( N + 1 ) ) )
			 * To improve calculation accuracy, we need to convert the calculation to: (currentValue * 2 + preValue * ( N - 1 ) ) / ( N + 1 )
			 *
			 * If the parameter is alpha,
			 * the calculation is: current result = current value * alpha + previous result * ( 1 - alpha )
			 */
			if (!CalculateExpMovingAvg(&bsonCurrentValue, &bsonPerValue,
									   &stateData->weight,
									   stateData->isAlpha, &bsonResultValue))
			{
				ereport(ERROR, (errcode(MongoInternalError)),
						errmsg(
							"CalculateStandardVariance: currentValue = %s, preValue = %s, weightValue = %s",
							BsonValueToJsonForLogging(
								&bsonCurrentValue),
							BsonValueToJsonForLogging(&bsonPerValue),
							BsonValueToJsonForLogging(&stateData->weight)),
						errhint(
							"CalculateStandardVariance: currentValue = %s, preValue = %s, weightValue = %s",
							BsonValueToJsonForLogging(
								&bsonCurrentValue),
							BsonValueToJsonForLogging(&bsonPerValue),
							BsonValueToJsonForLogging(&stateData->weight)));
			}

			/* If currentValue is of type decimal128, then expMovingResult will also be of type decimal128, */
			/* and all subsequent expMoving results will also be of type decimal128. */
			if (bsonCurrentValue.value_type == BSON_TYPE_DECIMAL128 ||
				stateData->preValue.value_type == BSON_TYPE_DECIMAL128)
			{
				stateData->preValue = bsonResultValue;
			}
			else
			{
				/* If result can be represented as an integer, we need to keep only the integer part. : 5.0 -> 5 */
				/* If result overflows int32, IsBsonValue32BitInteger will return false. */
				bool checkFixedInteger = true;
				if (IsBsonValue32BitInteger(&bsonResultValue, checkFixedInteger))
				{
					stateData->preValue.value_type = BSON_TYPE_INT32;
					stateData->preValue.value.v_int32 = BsonValueAsInt32(
						&bsonResultValue);
				}
				else
				{
					/* If result can be represented as an integer, we need to keep only the integer part. : 5.0 -> 5 */
					if (IsBsonValue64BitInteger(&bsonResultValue, checkFixedInteger))
					{
						stateData->preValue.value_type = BSON_TYPE_INT64;
						stateData->preValue.value.v_int64 = BsonValueAsInt64(
							&bsonResultValue);
					}
					else
					{
						stateData->preValue.value_type = BSON_TYPE_DOUBLE;
						stateData->preValue.value.v_double = BsonValueAsDouble(
							&bsonResultValue);
					}
				}
			}

			finalValue.bsonValue = stateData->preValue;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
	}
	else
	{
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
		PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
	}
}


/* transition function for the BSON_INTEGRAL aggregate
 * use the trapzoidal rule to calculate the integral
 */
Datum
bson_integral_transition(PG_FUNCTION_ARGS)
{
	bytea *bytes;
	bool isIntegral = true;
	BsonIntegralAndDerivativeAggState *currentState;

	pgbson *xValue = PG_GETARG_MAYBE_NULL_PGBSON(1);
	pgbson *yValue = PG_GETARG_MAYBE_NULL_PGBSON(2);
	long timeUnitInt64 = PG_GETARG_INT64(3);
	pgbsonelement xValueElement, yValueElement;
	PgbsonToSinglePgbsonElement(xValue, &xValueElement);
	PgbsonToSinglePgbsonElement(yValue, &yValueElement);
	if (IsPgbsonEmptyDocument(xValue) || IsPgbsonEmptyDocument(yValue))
	{
		PG_RETURN_NULL();
	}
	RunTimeCheckForIntegralAndDerivative(&xValueElement.bsonValue,
										 &yValueElement.bsonValue, timeUnitInt64,
										 isIntegral);
	if (PG_ARGISNULL(0))
	{
		MemoryContext aggregateContext;
		if (AggCheckCallContext(fcinfo, &aggregateContext) != AGG_CONTEXT_WINDOW)
		{
			ereport(ERROR, errmsg(
						"window aggregate function called in non-window-aggregate context"));
		}

		/* Create the aggregate state in the aggregate context. */
		MemoryContext oldContext = MemoryContextSwitchTo(aggregateContext);

		bytes = AllocateBsonIntegralAndDerivativeAggState();
		currentState = (BsonIntegralAndDerivativeAggState *) VARDATA(bytes);
		currentState->result.value_type = BSON_TYPE_DOUBLE;

		/* update the anchor point with current document in window */
		currentState->anchorX = xValueElement.bsonValue;
		currentState->anchorY = yValueElement.bsonValue;

		/* if xValue is a date, convert it to double */
		if (xValueElement.bsonValue.value_type == BSON_TYPE_DATE_TIME)
		{
			currentState->anchorX.value_type = BSON_TYPE_DOUBLE;
			currentState->anchorX.value.v_double = BsonValueAsDouble(
				&xValueElement.bsonValue);
		}
		MemoryContextSwitchTo(oldContext);
		PG_RETURN_POINTER(bytes);
	}
	else
	{
		bytes = PG_GETARG_BYTEA_P(0);
		currentState = (BsonIntegralAndDerivativeAggState *) VARDATA_ANY(bytes);
	}
	HandleIntegralDerivative(&xValueElement.bsonValue, &yValueElement.bsonValue,
							 timeUnitInt64,
							 currentState, isIntegral);

	/* update the anchor point with current document in window */
	currentState->anchorX = xValueElement.bsonValue;
	currentState->anchorY = yValueElement.bsonValue;

	/* if xValue is a date, convert it to double */
	if (xValueElement.bsonValue.value_type == BSON_TYPE_DATE_TIME)
	{
		currentState->anchorX.value_type = BSON_TYPE_DOUBLE;
		currentState->anchorX.value.v_double = BsonValueAsDouble(
			&xValueElement.bsonValue);
	}
	PG_RETURN_POINTER(bytes);
}


/* transition function for the BSON_DERIVATIVE aggregate
 * use dy/dx to calculate the derivative
 */
Datum
bson_derivative_transition(PG_FUNCTION_ARGS)
{
	pgbson *xValue = PG_GETARG_MAYBE_NULL_PGBSON(1);
	pgbson *yValue = PG_GETARG_MAYBE_NULL_PGBSON(2);
	long timeUnitInt64 = PG_GETARG_INT64(3);
	pgbsonelement xValueElement, yValueElement;
	PgbsonToSinglePgbsonElement(xValue, &xValueElement);
	PgbsonToSinglePgbsonElement(yValue, &yValueElement);

	bytea *bytes;
	bool isIntegral = false;
	BsonIntegralAndDerivativeAggState *currentState;
	if (IsPgbsonEmptyDocument(xValue) || IsPgbsonEmptyDocument(yValue))
	{
		PG_RETURN_NULL();
	}
	RunTimeCheckForIntegralAndDerivative(&xValueElement.bsonValue,
										 &yValueElement.bsonValue, timeUnitInt64,
										 isIntegral);
	if (PG_ARGISNULL(0))
	{
		MemoryContext aggregateContext;
		if (AggCheckCallContext(fcinfo, &aggregateContext) != AGG_CONTEXT_WINDOW)
		{
			ereport(ERROR, errmsg(
						"window aggregate function called in non-window-aggregate context"));
		}

		/* Create the aggregate state in the aggregate context. */
		MemoryContext oldContext = MemoryContextSwitchTo(aggregateContext);

		bytes = AllocateBsonIntegralAndDerivativeAggState();
		currentState = (BsonIntegralAndDerivativeAggState *) VARDATA(bytes);
		currentState->result.value_type = BSON_TYPE_NULL;

		/* anchor points are always the first document in the window for $derivative*/
		/* if xValue is a date, convert it to double */
		if (xValueElement.bsonValue.value_type == BSON_TYPE_DATE_TIME)
		{
			currentState->anchorX.value_type = BSON_TYPE_DOUBLE;
			currentState->anchorX.value.v_double = BsonValueAsDouble(
				&xValueElement.bsonValue);
		}
		else
		{
			currentState->anchorX = xValueElement.bsonValue;
		}
		currentState->anchorY = yValueElement.bsonValue;

		/* We have the first document in state and the second incoming document
		 * it's ready to calculate the derivative */
		MemoryContextSwitchTo(oldContext);
		PG_RETURN_POINTER(bytes);
	}
	else
	{
		bytes = PG_GETARG_BYTEA_P(0);
		currentState = (BsonIntegralAndDerivativeAggState *) VARDATA_ANY(bytes);
	}
	if (IsPgbsonEmptyDocument(xValue) || IsPgbsonEmptyDocument(yValue))
	{
		PG_RETURN_POINTER(bytes);
	}
	HandleIntegralDerivative(&xValueElement.bsonValue, &yValueElement.bsonValue,
							 timeUnitInt64,
							 currentState, isIntegral);
	PG_RETURN_POINTER(bytes);
}


/* final function for the BSON_INTEGRAL and BSON_DERIVATIVE aggregate
 * This takes the final value created and outputs a bson "integral" or "derivative"
 * with the appropriate type.
 */
Datum
bson_integral_derivative_final(PG_FUNCTION_ARGS)
{
	bytea *currentState = PG_ARGISNULL(0) ? NULL : PG_GETARG_BYTEA_P(0);
	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;

	if (currentState != NULL)
	{
		BsonIntegralAndDerivativeAggState *state =
			(BsonIntegralAndDerivativeAggState *) VARDATA_ANY(
				currentState);
		if (state->result.value_type != BSON_TYPE_NULL)
		{
			finalValue.bsonValue = state->result;
		}
		else if (state->result.value_type == BSON_TYPE_NULL)
		{
			finalValue.bsonValue.value_type = BSON_TYPE_NULL;
		}
	}
	else
	{
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
	}
	PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
}


/*
 * Applies the "final calculation" (FINALFUNC) for BSONSTDDEVPOP window aggregate operator.
 * This takes the final value created and outputs a bson stddev pop
 * with the appropriate type.
 */
Datum
bson_std_dev_pop_winfunc_final(PG_FUNCTION_ARGS)
{
	bytea *stdDevIntermediateState = PG_ARGISNULL(0) ? NULL : PG_GETARG_BYTEA_P(0);

	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;
	if (stdDevIntermediateState != NULL)
	{
		bson_value_t decimalResult = { 0 };
		decimalResult.value_type = BSON_TYPE_DECIMAL128;
		BsonCovarianceAndVarianceAggState *stdDevState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(stdDevIntermediateState);

		if (IsBsonValueNaN(&stdDevState->sxy) ||
			IsBsonValueInfinity(&stdDevState->sxy) != 0)
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = NAN;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else if (BsonValueAsInt64(&stdDevState->count) == 0)
		{
			/* we return null for empty sets */
			finalValue.bsonValue.value_type = BSON_TYPE_NULL;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else if (BsonValueAsInt64(&stdDevState->count) == 1)
		{
			/* we returns 0 for single numeric value */
			/* return double even if the value is decimal128 */
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = 0;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else
		{
			HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, stdDevState->sxy,
									stdDevState->count, decimalResult,
									"Failed while calculating bson_std_dev_pop_winfunc_final decimalResult for these values: ");
		}

		/* The value type of finalValue.bsonValue is set to BSON_TYPE_DOUBLE inside the function*/
		CalculateSqrtForStdDev(&decimalResult, &finalValue.bsonValue);
	}
	else
	{
		/* we return null for empty sets */
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
	}

	PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
}


/*
 * Applies the "final calculation" (FINALFUNC) for BSONSTDDEVSAMP window function.
 * This takes the final value created and outputs a bson stddev samp
 * with the appropriate type.
 */
Datum
bson_std_dev_samp_winfunc_final(PG_FUNCTION_ARGS)
{
	bytea *stdDevIntermediateState = PG_ARGISNULL(0) ? NULL : PG_GETARG_BYTEA_P(0);

	pgbsonelement finalValue;
	finalValue.path = "";
	finalValue.pathLength = 0;
	if (stdDevIntermediateState != NULL)
	{
		bson_value_t decimalResult = { 0 };
		decimalResult.value_type = BSON_TYPE_DECIMAL128;
		BsonCovarianceAndVarianceAggState *stdDevState =
			(BsonCovarianceAndVarianceAggState *) VARDATA_ANY(stdDevIntermediateState);

		if (IsBsonValueNaN(&stdDevState->sxy) ||
			IsBsonValueInfinity(&stdDevState->sxy))
		{
			finalValue.bsonValue.value_type = BSON_TYPE_DOUBLE;
			finalValue.bsonValue.value.v_double = NAN;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else if (BsonValueAsInt64(&stdDevState->count) == 0 ||
				 BsonValueAsInt64(&stdDevState->count) == 1)
		{
			/* we returns null for empty sets or single numeric value */
			finalValue.bsonValue.value_type = BSON_TYPE_NULL;
			PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
		}
		else
		{
			bson_value_t decimalOne;
			decimalOne.value_type = BSON_TYPE_DECIMAL128;
			decimalOne.value.v_decimal128 = GetDecimal128FromInt64(1);

			bson_value_t countMinus1;
			countMinus1.value_type = BSON_TYPE_DECIMAL128;
			HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, stdDevState->count,
									decimalOne, countMinus1,
									"Failed while calculating bson_std_dev_samp_winfunc_final countMinus1 for these values: ");

			HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, stdDevState->sxy,
									countMinus1, decimalResult,
									"Failed while calculating bson_std_dev_samp_winfunc_final decimalResult for these values: ");
		}

		/* The value type of finalValue.bsonValue is set to BSON_TYPE_DOUBLE inside the function*/
		CalculateSqrtForStdDev(&decimalResult, &finalValue.bsonValue);
	}
	else
	{
		/* we return null for empty sets */
		finalValue.bsonValue.value_type = BSON_TYPE_NULL;
	}

	PG_RETURN_POINTER(PgbsonElementToPgbson(&finalValue));
}


/*
 * Applies the "inverse transition function" (MINVFUNC) for BSONSTDDEVPOP and BSONSTDDEVSAMP.
 * takes one aggregate state structures (BsonCovarianceAndVarianceAggState)
 * and single data point. Remove the single data from BsonCovarianceAndVarianceAggState
 */
Datum
bson_std_dev_pop_samp_winfunc_invtransition(PG_FUNCTION_ARGS)
{
	MemoryContext aggregateContext;
	if (AggCheckCallContext(fcinfo, &aggregateContext) != AGG_CONTEXT_WINDOW)
	{
		ereport(ERROR, errmsg(
					"window aggregate function called in non-window-aggregate context"));
	}

	bytea *bytes;
	BsonCovarianceAndVarianceAggState *currentState;

	if (PG_ARGISNULL(0))
	{
		PG_RETURN_NULL();
	}
	else
	{
		bytes = PG_GETARG_BYTEA_P(0);
		currentState = (BsonCovarianceAndVarianceAggState *) VARDATA_ANY(bytes);
	}

	pgbson *currentValue = PG_GETARG_MAYBE_NULL_PGBSON(1);

	if (currentValue == NULL || IsPgbsonEmptyDocument(currentValue))
	{
		PG_RETURN_POINTER(bytes);
	}

	pgbsonelement currentValueElement;
	PgbsonToSinglePgbsonElement(currentValue, &currentValueElement);

	if (!BsonTypeIsNumber(currentValueElement.bsonValue.value_type))
	{
		PG_RETURN_POINTER(bytes);
	}

	/* restart aggregate if NaN or Infinity in current state or current values */
	/* or count is 0 */
	if (IsBsonValueNaN(&currentState->sxy) ||
		IsBsonValueInfinity(&currentState->sxy) ||
		IsBsonValueNaN(&currentValueElement.bsonValue) ||
		IsBsonValueInfinity(&currentValueElement.bsonValue) ||
		BsonValueAsInt64(&currentState->count) == 0)
	{
		PG_RETURN_NULL();
	}

	CalculateInvFuncForCovarianceOrVarianceWithYCAlgr(&currentValueElement.bsonValue,
													  &currentValueElement.bsonValue,
													  currentState);

	PG_RETURN_POINTER(bytes);
}


/* --------------------------------------------------------- */
/* Private helper methods */
/* --------------------------------------------------------- */

static bytea *
AllocateBsonCovarianceOrVarianceAggState()
{
	int bson_size = sizeof(BsonCovarianceAndVarianceAggState) + VARHDRSZ;
	bytea *combinedStateBytes = (bytea *) palloc0(bson_size);
	SET_VARSIZE(combinedStateBytes, bson_size);

	return combinedStateBytes;
}


static bytea *
AllocateBsonIntegralAndDerivativeAggState()
{
	int bson_size = sizeof(BsonIntegralAndDerivativeAggState) + VARHDRSZ;
	bytea *combinedStateBytes = (bytea *) palloc0(bson_size);
	SET_VARSIZE(combinedStateBytes, bson_size);

	return combinedStateBytes;
}


/*
 * Function to calculate variance/covariance state for inverse function.
 * If calculating variance, we use X and Y as the same value.
 *
 * According to a generalization of the
 * Youngs-Cramer algorithm formular, we update the state data like this
 * ```
 *  N^ = N - 1
 *  Sx^ = Sx - X
 *  Sy^ = Sy - Y
 *  Sxy^ = Sxy - N^/N * (Sx^/N^ - X) * (Sy^/N^ - Y)
 *  ```
 */
static void
CalculateInvFuncForCovarianceOrVarianceWithYCAlgr(const bson_value_t *newXValue,
												  const bson_value_t *newYValue,
												  BsonCovarianceAndVarianceAggState *
												  currentState)
{
	/* update the count of decimal values accordingly */
	if (newXValue->value_type == BSON_TYPE_DECIMAL128 ||
		newYValue->value_type == BSON_TYPE_DECIMAL128)
	{
		currentState->decimalCount--;
	}

	bson_value_t decimalCurrentXValue;
	decimalCurrentXValue.value_type = BSON_TYPE_DECIMAL128;
	decimalCurrentXValue.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		newXValue);

	bson_value_t decimalCurrentYValue;
	decimalCurrentYValue.value_type = BSON_TYPE_DECIMAL128;
	decimalCurrentYValue.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		newYValue);

	bson_value_t decimalOne;
	decimalOne.value_type = BSON_TYPE_DECIMAL128;
	decimalOne.value.v_decimal128 = GetDecimal128FromInt64(1);

	/* decimalN = currentState->count */
	bson_value_t decimalN;
	decimalN.value_type = BSON_TYPE_DECIMAL128;
	decimalN.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		&currentState->count);

	/* currentState->count -= 1.0 */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, currentState->count, decimalOne,
							currentState->count,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr count for these values: ");

	/* currentState->sx -= decimalCurrentXValue */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, currentState->sx,
							decimalCurrentXValue, currentState->sx,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr sx for these values: ");

	/* currentState->sy -= decimalCurrentYValue */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, currentState->sy,
							decimalCurrentYValue, currentState->sy,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr sy for these values: ");

	bson_value_t decimalXTmp;
	decimalXTmp.value_type = BSON_TYPE_DECIMAL128;

	bson_value_t decimalYTmp;
	decimalYTmp.value_type = BSON_TYPE_DECIMAL128;

	/* decimalXTmp = currentState->sx - decimalCurrentXValue * currentState->count */
	/* X * N^ */
	HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalCurrentXValue,
							currentState->count, decimalXTmp,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr decimalXTmp_1 for these values: ");

	/* Sx^ - X * N^ */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, currentState->sx, decimalXTmp,
							decimalXTmp,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr decimalXTmp_2 for these values: ");

	/* decimalYTmp = currentState->sy - decimalCurrentYValue * currentState->count */
	/* Y * N^ */
	HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalCurrentYValue,
							currentState->count, decimalYTmp,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr decimalYTmp_1 = for these values: ");

	/* Sy^ - Y * N^ */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, currentState->sy, decimalYTmp,
							decimalYTmp,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr decimalYTmp_2 for these values: ");

	bson_value_t decimalXYTmp;
	decimalXYTmp.value_type = BSON_TYPE_DECIMAL128;

	/* decimalXYTmp = decimalXTmp * decimalYTmp */
	/* (Sx^ - X * N^) * (Sy^ - Y * N^) */
	HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalXTmp, decimalYTmp,
							decimalXYTmp,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr decimalXYTmp for these values: ");

	bson_value_t decimalTmpNxx;
	decimalTmpNxx.value_type = BSON_TYPE_DECIMAL128;

	/* decimalTmpNxx = decimalN * currentState->count */
	HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalN, currentState->count,
							decimalTmpNxx,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr decimalTmpNxx for these values: ");

	bson_value_t decimalTmp;
	decimalTmp.value_type = BSON_TYPE_DECIMAL128;

	/* decimalTmp = decimalXYTmp / decimalTmpNxx */
	/* (Sx^ - X * N^) * (Sy^ - Y * N^) / (N * N^) = N^/N * (Sx^/N^ - X) * (Sy^/N^ - Y) */
	HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, decimalXYTmp, decimalTmpNxx,
							decimalTmp,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr decimalTmp for these values: ");

	/* currentState->sxy -= decimalTmp */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, currentState->sxy, decimalTmp,
							currentState->sxy,
							"Failed while calculating CalculateInvFuncForCovarianceOrVarianceWithYCAlgr sxy for these values: ");
}


/*
 * Function to calculate variance/covariance state for combine function.
 * If calculating variance, we use X and Y as the same value.
 *
 * The transition values combine using a generalization of the
 * Youngs-Cramer algorithm as follows:
 *
 *	N = N1 + N2
 *	Sx = Sx1 + Sx2
 *	Sy = Sy1 + Sy2
 *	Sxy = Sxy1 + Sxy2 + N1 * N2 * (Sx1/N1 - Sx2/N2) * (Sy1/N1 - Sy2/N2) / N;
 */
static void
CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr(const
													  BsonCovarianceAndVarianceAggState *
													  leftState, const
													  BsonCovarianceAndVarianceAggState *
													  rightState,
													  BsonCovarianceAndVarianceAggState *
													  currentState)
{
	/* If either of left or right node's count is 0, we can just copy the other node's state and return */
	if (IsDecimal128Zero(&leftState->count))
	{
		memcpy(currentState, rightState, sizeof(BsonCovarianceAndVarianceAggState));
		return;
	}
	else if (IsDecimal128Zero(&rightState->count))
	{
		memcpy(currentState, leftState, sizeof(BsonCovarianceAndVarianceAggState));
		return;
	}

	/* update the count of decimal values */
	currentState->decimalCount = leftState->decimalCount + rightState->decimalCount;

	/* currentState->count = leftState->count + rightState->count */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, leftState->count, rightState->count,
							currentState->count,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr count for these values: ");

	/* handle infinities first */
	/* if infinity values from left and right node with different signs, return NaN */
	/* if with the same sign, return the infinity value */
	/* if no infinity values, continue to calculate the covariance/variance */
	int isInfinityLeft = IsBsonValueInfinity(&leftState->sxy);
	int isInfinityRight = IsBsonValueInfinity(&rightState->sxy);
	if (isInfinityLeft * isInfinityRight == -1)
	{
		currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128NaN(&currentState->sxy);
		return;
	}
	else if (isInfinityLeft + isInfinityRight != 0)
	{
		currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
		if (isInfinityLeft + isInfinityRight > 0)
		{
			SetDecimal128PositiveInfinity(&currentState->sxy);
		}
		else
		{
			SetDecimal128NegativeInfinity(&currentState->sxy);
		}
		return;
	}

	/* currentState->sx = leftState->sx + rightState->sx */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, leftState->sx, rightState->sx,
							currentState->sx,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr sx for these values: ");

	/* currentState->sy = leftState->sy + rightState->sy */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, leftState->sy, rightState->sy,
							currentState->sy,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr sy for these values: ");

	bson_value_t decimalTmpLeft;
	decimalTmpLeft.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&decimalTmpLeft);

	bson_value_t decimalTmpRight;
	decimalTmpRight.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&decimalTmpRight);

	bson_value_t decimalXTmp;
	decimalXTmp.value_type = BSON_TYPE_DECIMAL128;

	/* decimalXTmp = leftState->sx / leftState->count - rightState->sx / rightState->count; */
	/* leftState->count and rightState->count won't be 0 as we have checked outside */

	/* Sx1/N1 */
	HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, leftState->sx, leftState->count,
							decimalTmpLeft,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalTmpLeft_1 for these values: ");

	/* Sx2/N2 */
	HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, rightState->sx, rightState->count,
							decimalTmpRight,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalTmpRight_1 for these values: ");

	/* Sx1/N1 - Sx2/N2 */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, decimalTmpLeft, decimalTmpRight,
							decimalXTmp,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalXTmp for these values: ");

	bson_value_t decimalYTmp;
	decimalYTmp.value_type = BSON_TYPE_DECIMAL128;

	/* decimalYTmp = leftState->sy / leftState->count - rightState->sy / rightState->count; */
	/* Sy1/N1 */
	HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, leftState->sy, leftState->count,
							decimalTmpLeft,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalTmpLeft_2 for these values: ");

	/* Sy2/N2 */
	HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, rightState->sy, rightState->count,
							decimalTmpRight,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalTmpRight_2 for these values: ");

	/* Sy1/N1 - Sy2/N2 */
	HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, decimalTmpLeft, decimalTmpRight,
							decimalYTmp,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalYTmp for these values: ");

	bson_value_t decimalNxx;
	decimalNxx.value_type = BSON_TYPE_DECIMAL128;

	/* decimalNxx = leftState->count * rightState->count; */
	HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, leftState->count,
							rightState->count, decimalNxx,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalNxx for these values: ");

	bson_value_t decimalXYTmp;
	decimalXYTmp.value_type = BSON_TYPE_DECIMAL128;

	/* decimalXYTmp = decimalXTmp * decimalYTmp; */
	HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalXTmp, decimalYTmp,
							decimalXYTmp,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalXYTmp for these values: ");

	bson_value_t decimalTmp;
	decimalTmp.value_type = BSON_TYPE_DECIMAL128;

	/* decimalTmp = decimalNxx * decimalXYTmp; */
	/* N1 * N2 * (Sx1/N1 - Sx2/N2) * (Sy1/N1 - Sy2/N2) */
	HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalNxx, decimalXYTmp,
							decimalTmp,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalTmp for these values: ");

	bson_value_t decimalD;
	decimalD.value_type = BSON_TYPE_DECIMAL128;
	SetDecimal128Zero(&decimalD);

	/* decimalD = decimalTmp / N; */
	/* N1 * N2 * (Sx1/N1 - Sx2/N2) * (Sy1/N1 - Sy2/N2) / N; */
	/* didn't check if N is 0, as we should not meet that case */
	HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, decimalTmp, currentState->count,
							decimalD,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr decimalD for these values: ");

	/* Sxy1 + Sxy2 */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, leftState->sxy, rightState->sxy,
							currentState->sxy,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr sxy_1 for these values: ");

	/* Sxy1 + Sxy2 + N1 * N2 * (Sx1/N1 - Sx2/N2) * (Sy1/N1 - Sy2/N2) / N */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, currentState->sxy, decimalD,
							currentState->sxy,
							"Failed while calculating CalculateCombineFuncForCovarianceOrVarianceWithYCAlgr sxy_2 for these values: ");
}


/*
 * Function to calculate variance/covariance state for transition function.
 * Use the Youngs-Cramer algorithm to incorporate the new value into the
 * transition values.
 * If calculating variance, we use X and Y as the same value.
 *
 * N = N + 1
 * Sx = Sx + X
 * Sy = Sy + Y
 * Sxy = Sxy + (N - 1) / N * (X - Sx / N) * (Y - Sy / N)
 *
 */
static void
CalculateSFuncForCovarianceOrVarianceWithYCAlgr(const bson_value_t *newXValue,
												const bson_value_t *newYValue,
												BsonCovarianceAndVarianceAggState *
												currentState)
{
	bson_value_t decimalOne;
	decimalOne.value_type = BSON_TYPE_DECIMAL128;
	decimalOne.value.v_decimal128 = GetDecimal128FromInt64(1);

	bson_value_t decimalXTmp;
	decimalXTmp.value_type = BSON_TYPE_DECIMAL128;

	bson_value_t decimalYTmp;
	decimalYTmp.value_type = BSON_TYPE_DECIMAL128;

	bson_value_t decimalXYTmp;
	decimalXYTmp.value_type = BSON_TYPE_DECIMAL128;

	bson_value_t decimalNxx;
	decimalNxx.value_type = BSON_TYPE_DECIMAL128;

	bson_value_t decimalCurrentXValue;
	decimalCurrentXValue.value_type = BSON_TYPE_DECIMAL128;
	decimalCurrentXValue.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		newXValue);

	bson_value_t decimalCurrentYValue;
	decimalCurrentYValue.value_type = BSON_TYPE_DECIMAL128;
	decimalCurrentYValue.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		newYValue);

	/* decimalN = currentState->count */
	bson_value_t decimalN;
	decimalN.value_type = BSON_TYPE_DECIMAL128;
	decimalN.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		&currentState->count);

	/* update the count of decimal values accordingly */
	if (newXValue->value_type == BSON_TYPE_DECIMAL128 ||
		newYValue->value_type == BSON_TYPE_DECIMAL128)
	{
		currentState->decimalCount++;
	}

	/* decimalN += 1.0 */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, decimalN, decimalOne, decimalN,
							"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalN for these values: ");

	/* NAN will be handled in later parts */
	/* focus on infinities first */
	/* We will check all the infinity values (if any) from Sxy(didn't update yet), X, Y */
	/* If all the infinity values have the same sign, we will return the infinity value */
	/* If any of the infinity values have different signs, we will return the NaN value */
	/* If no infinity values, we will continue the calculation */
	/* The return value of IsBsonValueInfinity */
	/* 0: finite number, 1: positive infinity, -1: negative infinity */
	int isInfinityX = IsBsonValueInfinity(newXValue);
	int isInfinityY = IsBsonValueInfinity(newYValue);
	int isInfinitySxy = IsBsonValueInfinity(&currentState->sxy);

	/* the product of two values is -1 means they are infinity values with different signs */
	if (isInfinityX * isInfinityY == -1 || isInfinityX * isInfinitySxy == -1 ||
		isInfinityY * isInfinitySxy == -1)
	/* infinities with different signs, return nan */
	{
		currentState->count = decimalN;
		currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
		SetDecimal128NaN(&currentState->sxy);
		return;
	}
	else if (isInfinityX || isInfinityY || isInfinitySxy)
	/* infinities with the same sign, return infinity */
	{
		currentState->count = decimalN;
		currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
		if ((isInfinityX > 0) || (isInfinityY > 0) || (isInfinitySxy > 0))
		{
			SetDecimal128PositiveInfinity(&currentState->sxy);
		}
		else
		{
			SetDecimal128NegativeInfinity(&currentState->sxy);
		}

		return;
	}


	/* currentState->sx += decimalCurrentXValue */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, currentState->sx, decimalCurrentXValue,
							currentState->sx,
							"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr sx for these values: ");

	/* currentState->sy += decimalCurrentYValue */
	HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, currentState->sy, decimalCurrentYValue,
							currentState->sy,
							"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr sy for these values: ");

	if (BsonValueAsDouble(&currentState->count) > 0.0)
	{
		/* decimalXTmp = decimalCurrentXValue * decimalN - currentState->sx; */
		HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalCurrentXValue, decimalN,
								decimalXTmp,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalXTmp_1 for these values: ");

		HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, decimalXTmp, currentState->sx,
								decimalXTmp,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalXTmp_2 for these values: ");

		/* decimalYTmp = decimalCurrentYValue * decimalN - currentState->sy; */
		HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalCurrentYValue, decimalN,
								decimalYTmp,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalYTmp_1 for these values: ");

		HANDLE_DECIMAL_OP_ERROR(SubtractDecimal128Numbers, decimalYTmp, currentState->sy,
								decimalYTmp,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalYTmp_2 for these values: ");


		/* currentState->sxy += decimalXTmp * decimalYTmp / (decimalN * currentState->count); */
		HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalXTmp, decimalYTmp,
								decimalXYTmp,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalXYTmp_1 for these values: ");

		HANDLE_DECIMAL_OP_ERROR(MultiplyDecimal128Numbers, decimalN, currentState->count,
								decimalNxx,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalNxx for these values: ");

		decimalXTmp = decimalXYTmp; /* use decimalXTmp as a temporary variable to take origin value of decimalXYTmp */
		HANDLE_DECIMAL_OP_ERROR(DivideDecimal128Numbers, decimalXTmp, decimalNxx,
								decimalXYTmp,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr decimalXYTmp_2 for these values: ");

		/* currentState->sxy += decimalXYTmp */
		HANDLE_DECIMAL_OP_ERROR(AddDecimal128Numbers, currentState->sxy, decimalXYTmp,
								currentState->sxy,
								"Failed while calculating CalculateSFuncForCovarianceOrVarianceWithYCAlgr sxy for these values: ");
	}
	else
	{
		/*
		 * At the first input, we normally can leave currentState->sxy as 0.
		 * However, if the first input is Inf or NaN, we'd better force currentState->sxy
		 * to Inf or NaN.
		 */
		if (IsBsonValueNaN(newXValue) || IsBsonValueNaN(newYValue) || isInfinityX *
			isInfinityY == -1)
		{
			currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
			SetDecimal128NaN(&currentState->sxy);
		}
		else if (isInfinityX + isInfinityY != 0)
		{
			currentState->sxy.value_type = BSON_TYPE_DECIMAL128;
			if (isInfinityX + isInfinityY > 0)
			{
				SetDecimal128PositiveInfinity(&currentState->sxy);
			}
			else
			{
				SetDecimal128NegativeInfinity(&currentState->sxy);
			}
		}
	}

	currentState->count = decimalN;
}


bool
ParseInputWeightForExpMovingAvg(const bson_value_t *opValue,
								bson_value_t *inputExpression,
								bson_value_t *weightExpression,
								bson_value_t *decimalWeightValue)
{
	bson_iter_t docIter;
	BsonValueInitIterator(opValue, &docIter);


	/*
	 * The $expMovingAvg accumulator expects a document in the form of
	 * { "input": <value>, "alpha": <value> } or { "input": <value>, "N": <value> }
	 * input is required parameter, both N and alpha are optional, but must specify either N or alpha, cannot specify both.
	 * paramsValid is initially 0 and is used to check if params are valid:
	 * if N is available, paramsValid |= 1;
	 * if Alpha is available, paramsValid |= 2;
	 * if input is available, paramsValid |= 4;
	 *
	 * So the opValue is only valid when paramsValid is equal to 5 (101) or 6(101).
	 */
	int32 paramsValid = InputValidFlags_Unknown;

	while (bson_iter_next(&docIter))
	{
		const char *key = bson_iter_key(&docIter);

		if (strcmp(key, "input") == 0)
		{
			*inputExpression = *bson_iter_value(&docIter);
			paramsValid |= InputValidFlags_Input;
		}
		else if (strcmp(key, "alpha") == 0)
		{
			/*
			 * Alpha is a float number, must be between 0 and 1 (exclusive).
			 */
			*weightExpression = *bson_iter_value(&docIter);

			if (BsonValueAsDouble(weightExpression) <= 0 || BsonValueAsDouble(
					weightExpression) >= 1)
			{
				ereport(ERROR, (errcode(MongoFailedToParse),
								errmsg(
									"'alpha' must be between 0 and 1 (exclusive), found alpha: %lf",
									BsonValueAsDouble(weightExpression))));
			}
			decimalWeightValue->value_type = BSON_TYPE_DECIMAL128;
			decimalWeightValue->value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
				weightExpression);
			paramsValid |= InputValidFlags_Alpha;
		}
		else if (strcmp(key, "N") == 0)
		{
			/*
			 * N is a integer, must be greater than 1.
			 */
			*weightExpression = *bson_iter_value(&docIter);

			if (BsonTypeIsNumber(weightExpression->value_type))
			{
				bool checkFixedInteger = true;
				if (!IsBsonValue64BitInteger(weightExpression, checkFixedInteger) &&
					!IsBsonValueNegativeNumber(weightExpression))
				{
					ereport(ERROR, (errcode(MongoFailedToParse),
									errmsg(
										"'N' field must be an integer, but found  N: %lf. To use a non-integer, use the 'alpha' argument instead",
										BsonValueAsDouble(weightExpression))));
				}
				else if (IsBsonValueNegativeNumber(weightExpression))
				{
					ereport(ERROR, (errcode(MongoFailedToParse),
									errmsg(
										"'N' must be greater than zero. Got %d",
										BsonValueAsInt32(weightExpression))));
				}
			}
			else
			{
				ereport(ERROR, (errcode(MongoFailedToParse),
								errmsg(
									"'N' field must be an integer, but found type %s",
									BsonTypeName(weightExpression->value_type))));
			}


			decimalWeightValue->value_type = BSON_TYPE_DECIMAL128;
			decimalWeightValue->value.v_decimal128 = GetBsonValueAsDecimal128(
				weightExpression);
			paramsValid |= InputValidFlags_N;
		}
		else
		{
			/*incorrect parameter,like "alpah" */
			ereport(ERROR, (errcode(MongoFailedToParse),
							errmsg(
								"Got unrecognized field in $expMovingAvg, $expMovingAvg sub object must have exactly two fields: An 'input' field, and either an 'N' field or an 'alpha' field")));
		}
	}

	if (paramsValid <= InputValidFlags_Input || paramsValid == (InputValidFlags_Input |
																InputValidFlags_N |
																InputValidFlags_Alpha))
	{
		ereport(ERROR, (errcode(MongoFailedToParse),
						errmsg(
							"$expMovingAvg sub object must have exactly two fields: An 'input' field, and either an 'N' field or an 'alpha' field")));
	}

	return (paramsValid & InputValidFlags_Alpha) ? true : false;
}


/*
 * Function to calculate expMovingAvg.
 *
 * If the parameter is N,
 * the calculation is: current result = current value * ( 2 / ( N + 1 ) ) + previous result * ( 1 - ( 2 / ( N + 1 ) ) )
 * To improve calculation accuracy, we need to convert the calculation to: (currentValue * 2 + preValue * ( N - 1 ) ) / ( N + 1 )
 *
 * If the parameter is alpha,
 * the calculation is: current result = current value * alpha + previous result * ( 1 - alpha )
 */
static bool
CalculateExpMovingAvg(bson_value_t *bsonCurrentValue, bson_value_t *bsonPerValue,
					  bson_value_t *bsonWeightValue, bool isAlpha,
					  bson_value_t *bsonResultValue)
{
	bool isDecimalOpSuccess = true;

	bson_value_t decimalCurrentValue;
	decimalCurrentValue.value_type = BSON_TYPE_DECIMAL128;
	decimalCurrentValue.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		bsonCurrentValue);

	bson_value_t decimalPreValue;
	decimalPreValue.value_type = BSON_TYPE_DECIMAL128;
	decimalPreValue.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(bsonPerValue);

	bson_value_t decimalWeightValue;
	decimalWeightValue.value_type = BSON_TYPE_DECIMAL128;
	decimalWeightValue.value.v_decimal128 = GetBsonValueAsDecimal128Quantized(
		bsonWeightValue);

	bson_value_t decimalOne;
	decimalOne.value_type = BSON_TYPE_DECIMAL128;
	decimalOne.value.v_decimal128 = GetDecimal128FromInt64(1);

	bson_value_t decimalTwo;
	decimalTwo.value_type = BSON_TYPE_DECIMAL128;
	decimalTwo.value.v_decimal128 = GetDecimal128FromInt64(2);

	if (isAlpha)
	{
		/*
		 *  current result = current value * alpha + previous result * ( 1 - alpha )
		 */
		bson_value_t decimalPreWeight;
		Decimal128Result decimalOpResult;

		/* 1 - alpha */
		decimalOpResult = SubtractDecimal128Numbers(&decimalOne, &decimalWeightValue,
													&decimalPreWeight);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/* previous result * ( 1 - alpha ) */
		decimalOpResult = MultiplyDecimal128Numbers(&decimalPreValue, &decimalPreWeight,
													&decimalPreValue);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/* current value * alpha */
		decimalOpResult = MultiplyDecimal128Numbers(&decimalCurrentValue,
													&decimalWeightValue,
													&decimalCurrentValue);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/* decimalPreValue = previous result * ( 1 - alpha ) */
		/* decimalCurrentValue = current value * alpha */
		/* bsonResultValue = decimalPreValue + decimalCurrentValue */
		decimalOpResult = AddDecimal128Numbers(&decimalPreValue, &decimalCurrentValue,
											   bsonResultValue);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}
	}
	else
	{
		/*
		 *  current result = current value * ( 2 / ( N + 1 ) ) + previous result * ( 1 - ( 2 / ( N + 1 ) ) )
		 *  To improve calculation accuracy, we need to convert the calculation:
		 *  (currentValue * 2 + preValue * ( N - 1 ) ) / ( N + 1 )
		 */
		bson_value_t decimalWeightSubtractOne;  /* N-1 */
		bson_value_t decimalWeightAddOne;  /* N+1 */

		Decimal128Result decimalOpResult;

		/*currentValue * 2 */
		decimalOpResult = MultiplyDecimal128Numbers(&decimalCurrentValue, &decimalTwo,
													&decimalCurrentValue);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/* N - 1 */
		decimalOpResult = SubtractDecimal128Numbers(&decimalWeightValue, &decimalOne,
													&decimalWeightSubtractOne);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/* preValue * (N - 1) */
		decimalOpResult = MultiplyDecimal128Numbers(&decimalPreValue,
													&decimalWeightSubtractOne,
													&decimalPreValue);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/* currentValue*2 + preValue * (N-1) */
		decimalOpResult = AddDecimal128Numbers(&decimalPreValue, &decimalCurrentValue,
											   bsonResultValue);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/*bsonWeightValue = N; */
		/* N + 1 */
		decimalOpResult = AddDecimal128Numbers(&decimalWeightValue, &decimalOne,
											   &decimalWeightAddOne);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}

		/* bsonResultValue = currentValue * 2 + preValue*(N - 1); */
		/* decimalWeightAddOne =  N + 1 */
		/* bsonResultValue / decimalWeightAddOne */
		decimalOpResult = DivideDecimal128Numbers(bsonResultValue, &decimalWeightAddOne,
												  bsonResultValue);
		if (decimalOpResult != Decimal128Result_Success && decimalOpResult !=
			Decimal128Result_Inexact)
		{
			isDecimalOpSuccess = false;
			return isDecimalOpSuccess;
		}
	}
	return isDecimalOpSuccess;
}


/* This function is used to handle the $integral and $derivative operators
 *  1. Calculate the integral or derivative value
 *  2. Update the result value in the currentState
 */
static void
HandleIntegralDerivative(bson_value_t *xBsonValue, bson_value_t *yBsonValue,
						 long timeUnitInt64,
						 BsonIntegralAndDerivativeAggState *currentState,
						 const bool isIntegralOperator)
{
	/* Intermidiate variables for calculation */
	bson_value_t timeUnitInMs, yValue, xValue;
	timeUnitInMs.value_type = BSON_TYPE_DOUBLE;
	timeUnitInMs.value.v_double = timeUnitInt64;
	yValue = *yBsonValue;
	xValue = *xBsonValue;

	/* Convert date time to double to avoid calucation error*/
	if (xBsonValue->value_type == BSON_TYPE_DATE_TIME)
	{
		xValue.value_type = BSON_TYPE_DOUBLE;
		xValue.value.v_double = BsonValueAsDouble(xBsonValue);
	}

	/* Calculation status */
	bool success = isIntegralOperator ?
				   IntegralOfTwoPointsByTrapezoidalRule(&xValue, &yValue,
														currentState,
														&timeUnitInMs) :
				   DerivativeOfTwoPoints(&xValue, &yValue, currentState,
										 &timeUnitInMs);

	/* If calculation failed, throw an error */
	if (!success)
	{
		char *opName = isIntegralOperator ? "$integral" : "$derivative";
		ereport(ERROR, errcode(MongoInternalError),
				errmsg(
					"Handling %s: yValue = %f, xValue = %f, currentState->anchorX = %f, currentState->anchorY = %f, currentState->result = %f",
					opName, BsonValueAsDouble(&yValue), BsonValueAsDouble(&xValue),
					BsonValueAsDouble(&currentState->anchorX), BsonValueAsDouble(
						&currentState->anchorY), BsonValueAsDouble(
						&currentState->result)),
				errdetail_log(
					"Handling %s: yValue = %f, xValue = %f, currentState->anchorX = %f, currentState->anchorY = %f, currentState->result = %f",
					opName, BsonValueAsDouble(&yValue), BsonValueAsDouble(&xValue),
					BsonValueAsDouble(&currentState->anchorX), BsonValueAsDouble(
						&currentState->anchorY), BsonValueAsDouble(
						&currentState->result)));
	}
}


/* This function is used to check the runtime syntax, input, and unit for $integral and $derivative operators */
static void
RunTimeCheckForIntegralAndDerivative(bson_value_t *xBsonValue, bson_value_t *yBsonValue,
									 long timeUnitInt64, bool isIntegralOperator)
{
	const char *opName = isIntegralOperator ? "$integral" : "$derivative";

	/* if xBsonValue is not a date and unit is specified, throw an error */
	if (IsBsonValueDateTimeFormat(xBsonValue->value_type) ||
		xBsonValue->value_type == BSON_TYPE_NULL)
	{
		if (!timeUnitInt64)
		{
			int errorCode = isIntegralOperator ? ERRCODE_HELIO_LOCATION5423902 :
							ERRCODE_HELIO_LOCATION5624901;
			const char *errorMsg = isIntegralOperator
								   ?
								   "%s (with no 'unit') expects the sortBy field to be numeric"
								   : "%s where the sortBy is a Date requires an 'unit'";
			ereport(ERROR, errcode(errorCode),
					errmsg(errorMsg, opName),
					errdetail_log(errorMsg, opName));
		}
	}
	/* if xBsonValue is a number and unit is specified, throw an error */
	else if (BsonTypeIsNumber(xBsonValue->value_type))
	{
		if (timeUnitInt64)
		{
			int errorCode = isIntegralOperator ? ERRCODE_HELIO_LOCATION5423901 :
							ERRCODE_HELIO_LOCATION5624900;
			const char *errorMsg = "%s with 'unit' expects the sortBy field to be a Date";
			ereport(ERROR, errcode(errorCode),
					errmsg(errorMsg, opName),
					errdetail_log(errorMsg, opName));
		}
	}

	/* if unit is specifed but x is not a date, throw an error */
	if (timeUnitInt64 && xBsonValue->value_type != BSON_TYPE_DATE_TIME)
	{
		ereport(ERROR, (errcode(MongoLocation5429513), errmsg(
							"Expected the sortBy field to be a Date, but it was %s",
							BsonTypeName(
								xBsonValue->value_type))));
	}
	/* if unit is not specified and x is a date, throw an error */
	else if (!timeUnitInt64 && xBsonValue->value_type == BSON_TYPE_DATE_TIME)
	{
		ereport(ERROR, (errcode(MongoLocation5429513), errmsg(
							"For windows that involve date or time ranges, a unit must be provided.")));
	}

	/* y must be a number */
	if (!BsonTypeIsNumber(yBsonValue->value_type))
	{
		ereport(ERROR, (errcode(ERRCODE_HELIO_LOCATION5423900),
						errmsg(
							"The input value of %s window function must be a vector"
							" of 2 value, the first value must be numeric or date type "
							"and the second must be numeric.", opName),
						errdetail_log("Input value is: %s",
									  BsonTypeName(yBsonValue->value_type))));
	}
}


/* This function is used to calculate the integral
 * of current state and current document in window by Trapezoidal Rule.
 * The result will be promoted to decimal if one of the input is decimal.
 */
bool
IntegralOfTwoPointsByTrapezoidalRule(bson_value_t *xValue,
									 bson_value_t *yValue,
									 BsonIntegralAndDerivativeAggState *currentState,
									 bson_value_t *timeUnitInMs)
{
	bool success = true;
	bson_value_t stateXValue = currentState->anchorX;
	const bson_value_t stateYValue = currentState->anchorY;

	bool overflowedFromInt64, convertInt64OverflowToDouble = false;

	/* get time delta */
	success &= SubtractNumberFromBsonValue(xValue, &stateXValue, &overflowedFromInt64);

	/* add anchorY axis */
	success &= AddNumberToBsonValue(yValue, &stateYValue, &overflowedFromInt64);

	/* get area */
	success &= MultiplyWithFactorAndUpdate(yValue, xValue, convertInt64OverflowToDouble);
	bson_value_t bsonValueTwo;
	bsonValueTwo.value_type = BSON_TYPE_DOUBLE;
	bsonValueTwo.value.v_double = 2.0;

	success &= DivideBsonValueNumbers(yValue, &bsonValueTwo);
	if (timeUnitInMs->value.v_double != 0.0)
	{
		success &= DivideBsonValueNumbers(yValue, timeUnitInMs);
	}

	success &= AddNumberToBsonValue(&currentState->result, yValue, &overflowedFromInt64);
	return success;
}


/* This function is used to calculate the derivative
 * of current state and current document in window by derivative rule.
 * The result will be promoted to decimal if one of the input is decimal.
 */
bool
DerivativeOfTwoPoints(bson_value_t *xValue, bson_value_t *yValue,
					  BsonIntegralAndDerivativeAggState *currentState,
					  bson_value_t *timeUnitInMs)
{
	bool success = true;
	bson_value_t stateXValue = currentState->anchorX;
	const bson_value_t stateYValue = currentState->anchorY;

	bool overflowedFromInt64 = false;

	/* get time delta */
	success &= SubtractNumberFromBsonValue(xValue, &stateXValue, &overflowedFromInt64);
	if (timeUnitInMs->value.v_double != 0.0)
	{
		success &= DivideBsonValueNumbers(xValue, timeUnitInMs);
	}

	/* get value delta */
	success &= SubtractNumberFromBsonValue(yValue, &stateYValue, &overflowedFromInt64);

	/* get derivative */
	success &= DivideBsonValueNumbers(yValue, xValue);
	currentState->result = *yValue;
	return success;
}


/* This function is used to calculate the square root of input value.
 * The output result is double.
 */
static void
CalculateSqrtForStdDev(const bson_value_t *inputResult, bson_value_t *outputResult)
{
	outputResult->value_type = BSON_TYPE_DOUBLE;
	double resultForSqrt = 0;
	if (inputResult->value_type == BSON_TYPE_DECIMAL128)
	{
		if (IsDecimal128InDoubleRange(inputResult))
		{
			resultForSqrt = BsonValueAsDouble(inputResult);
			if (resultForSqrt < 0)
			{
				ereport(ERROR, (errcode(ERRCODE_HELIO_INTERNALERROR)),
						errmsg("CalculateSqrtForStdDev: *inputResult = %f",
							   BsonValueAsDouble(inputResult)),
						errdetail_log(
							"CalculateSqrtForStdDev: *inputResult = %f",
							BsonValueAsDouble(inputResult)));
			}
			else
			{
				outputResult->value.v_double = sqrt(resultForSqrt);
			}
		}
		else
		{
			outputResult->value.v_double = NAN;
		}
	}
	else
	{
		resultForSqrt = BsonValueAsDouble(inputResult);
		outputResult->value.v_double = sqrt(resultForSqrt);
	}
}
