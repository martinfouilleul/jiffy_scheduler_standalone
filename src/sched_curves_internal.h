/************************************************************//**
*
*	@file: sched_curves_internal.h
*	@author: Martin Fouilleul
*	@date: 09/10/2020
*	@revision:
*
*****************************************************************/
#ifndef __SCHED_CURVES_INTERNAL_H_
#define __SCHED_CURVES_INTERNAL_H_

#include"sched_curves.h"

//------------------------------------------------------------------------------------------------------
// tempo curve structs
//------------------------------------------------------------------------------------------------------

typedef struct bezier_coeffs
{
	f64 cx[4];
	f64 cy[4];
} bezier_coeffs;

typedef struct sched_curve_elt
{
	sched_curve_type type;

	f64 startValue; // tempo at the beginning of the interval
	f64 endValue;   // tempo at the end of the interval
	f64 length;     // length of the interval in the first axis unit (ie time or position)

	//NOTE(martin): precomputed tranformed length, ie time length if the main axis of the curve is position,
	//              and position length if the main axis of the curve is time.
	f64 transformedLength;

	f64 start;
	f64 end;
	f64 transformedStart;
	f64 transformedEnd;

	//TODO precomputed slope, curve power basis coefficients, etc...
	bezier_coeffs coeffs;

} sched_scaling_elt;

typedef struct sched_curve
{
	sched_curve_axes axes;
	u32 eltCount;
	sched_curve_elt* elements;

} sched_curve;


void bezier_coeffs_init_with_control_points(bezier_coeffs* coeffs, f64 p0x, f64 p0y, f64 p1x, f64 p1y, f64 p2x, f64 p2y, f64 p3x, f64 p3y);

#endif //__SCHED_CURVES_INTERNAL_H_
