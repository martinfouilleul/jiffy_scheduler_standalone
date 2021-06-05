/************************************************************//**
*
*	@file: sched_curves.h
*	@author: Martin Fouilleul
*	@date: 29/09/2020
*	@revision:
*
*****************************************************************/
#ifndef __SCHED_CURVES_H_
#define __SCHED_CURVES_H_

#include"typedefs.h"

typedef enum { SCHED_CURVE_POS_TEMPO = 0,
               SCHED_CURVE_TIME_TEMPO } sched_curve_axes;

typedef enum { SCHED_CURVE_CONST = 0,
               SCHED_CURVE_LINEAR,
	       SCHED_CURVE_BEZIER } sched_curve_type;

typedef struct sched_curve_descriptor_elt
{
	sched_curve_type type;
	f64 length;
	f64 startValue;
	f64 endValue;

	//NOTE(martin): bezier control points
	f64 p1x;
	f64 p1y;
	f64 p2x;
	f64 p2y;

} sched_curve_descriptor_elt;

typedef struct sched_curve_descriptor
{
	sched_curve_axes axes;
	u32 eltCount;
	sched_curve_descriptor_elt* elements;

} sched_curve_descriptor;

struct sched_curve;

sched_curve* sched_curve_create(sched_curve_descriptor* descriptor);
void sched_curve_destroy(sched_curve* curve);

//NOTE(martin): curve pos/time conversion functions return values are:
//		-1 if the abscissa was before the beginning of the curve,
//              +1 if the abscissa was after the end of the curve
//		0 if the abscissa was inside the curve domain

int sched_curve_get_position_from_time(sched_curve* curve, f64 time, f64* outPos);
int sched_curve_get_time_from_position(sched_curve* curve, f64 pos, f64* outTime);

#endif //__SCHED_CURVES_H_
