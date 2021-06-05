/************************************************************//**
*
*	@file: main.cpp
*	@author: Martin Fouilleul
*	@date: 05/10/2020
*	@revision:
*
*****************************************************************/
#include<stdio.h>
#include"sched_main.cpp"

int main()
{
	//TODO(martin): read curve descriptor from input.
	//              for now read built-in curve
	const int eltCount = 2;
	sched_curve_descriptor_elt elements[eltCount] = {
		{.type = SCHED_CURVE_BEZIER, .startValue = 2, .endValue = 8, .length = 10, .p1x = 0.5, .p1y = 0, .p2x = 0.5, .p2y = 1},
		{.type = SCHED_CURVE_BEZIER, .startValue = 8, .endValue = 2, .length = 10, .p1x = 0.5, .p1y = 0, .p2x = 0.5, .p2y = 1}};

	sched_curve_descriptor desc = {.axes = SCHED_CURVE_POS_TEMPO, .eltCount = eltCount, .elements = elements};
	sched_curve* curve = sched_curve_create(&desc);

	printf("time from pos\n");
	{
		f64 time = 0;
		f64 pos = 0;
		f64 posStep = 0.001;

		f64 totalPosLength = 0;
		for(int i=0; i<curve->eltCount; i++)
		{
			if(curve->axes == SCHED_CURVE_POS_TEMPO)
			{
				totalPosLength += curve->elements[i].length;
			}
			else
			{
				totalPosLength += curve->elements[i].transformedLength;
			}
		}
		f64 posStepCount = totalPosLength / posStep + 1;

		for(int i=0; i<posStepCount; i++)
		{
			f64 newTime = 0;
			f64 newPos = pos + posStep;
			sched_curve_get_time_from_position(curve, newPos, &newTime);

			f64 timeUpdate = newTime - time;
			f64 posUpdate = newPos - pos;

			printf("%.12f ; %.12f\n", pos, time);

			time = newTime;
			pos = newPos;
		}
	}
	printf("pos from time\n");
	{
		f64 time = 0;
		f64 pos = 0;
		f64 timeStep = 0.001;

		f64 totalTimeLength = 0;
		for(int i=0; i<curve->eltCount; i++)
		{
			if(curve->axes == SCHED_CURVE_POS_TEMPO)
			{
				totalTimeLength += curve->elements[i].transformedLength;
			}
			else
			{
				totalTimeLength += curve->elements[i].length;
			}
		}
		f64 timeStepCount = totalTimeLength / timeStep + 1;

		for(int i=0; i<timeStepCount; i++)
		{
			f64 newPos = 0;
			f64 newTime = time + timeStep;
			sched_curve_get_position_from_time(curve, newTime, &newPos);

			f64 timeUpdate = newTime - time;
			f64 posUpdate = newPos - pos;

			ASSERT(posUpdate >= 0 && timeUpdate >= 0);

			printf("%.12f ; %.12f\n", pos, time);

			pos = newPos;
			time = newTime;
		}
	}
	sched_curve_destroy(curve);
	return(0);
}
