/************************************************************//**
*
*	@file: sched_curves.cpp
*	@author: Martin Fouilleul
*	@date: 29/09/2020
*	@revision:
*
*****************************************************************/
#include<math.h>
#include"macro_helpers.h"
#include"sched_curves_internal.h"

#define LOG_SUBSYSTEM "Scheduler"
#include"debug_log.h"

//------------------------------------------------------------------------------------------------------
// Runge-Kutta Cash-Karp solver for autonomous ODE (ie. y' = f(y))
//------------------------------------------------------------------------------------------------------
typedef double (*rkck_autonomous_rhs)(double y, void* context);

double rkck_autonomous_step(double y_in,
		            double dydt,
		            double* y_out,
		            double step,
		            rkck_autonomous_rhs rhs,
		            void* context)
{
	//NOTE(martin): hard-coded Runge-Kutta Cash-Karp step for autonomous ODE y' = f(y)

	double k1 = dydt;
	double k2 = rhs(y_in + step*(1./5)*k1, context);
	double k3 = rhs(y_in + step*((3./40)*k1 + (9./40)*k2), context);
	double k4 = rhs(y_in + step*((3./10)*k1 - (9./10)*k2 + (6./5)*k3), context);
	double k5 = rhs(y_in + step*((-11./54)*k1 + (5./2)*k2 - (70./27)*k3 + (35./27)*k4), context);
	double k6 = rhs(y_in + step*((1631./55296)*k1 + (175./512)*k2 + (575./13824)*k3 + (44275./110592)*k4 + (253./4096)*k5), context);

	*y_out = y_in + step * (37./378 * k1
			       + 250./621 * k3
			       + 125./594 * k4
			       + 512./1771 * k6);

	double y_low = y_in + step * (2825./27648. * k1
	                              + 18575./48384 * k3
				      + 13525./55296 * k4
				      + 277./14336 * k5
				      + 1./4 * k6);

	return(*y_out - y_low);
}


void rkck_autonomous_adaptive_step(double* y,
				   double* t,
				   double* step,
				   rkck_autonomous_rhs rhs,
				   void* context,
				   double eps,
				   double min_step_size,
				   int *itCount,
				   double* total_err)
{
	//NOTE(martin): this routine computes an adaptive Cash-Karp step for an ODE of the form y' = f(y)
	//              y will be updated with the new value of y
	//
	//              step will be updated with the next guess for step
	//
	//              eps_scale is used to control either the absolute error value or the relative error to y or step values
	//
	//              itCount serves to update the total number of iteration computed
	//
	//              estErr is used to accumulate the total estimated error

	//NOTE(martin): constants used to grow/shrink the step size
	//              errGuard equals (5/safety) raised to the power (1/growPower)
	//              it ensures we don't expand the step size by more than a factor of 5

	const double safety = 0.9;
	const double growPower = -0.2;
	const double shrinkPower = -0.25;
	const double errGuard = 1.89e-4;

	double step_try = *step;
	double y_tmp = 0;
	double scaled_err = 0;
	double err = 0;
	double dydt = rhs(*y, context);

	DEBUG_ASSERT(!isinf(dydt) && !isnan(dydt));

	while(1)
	{
		err = rkck_autonomous_step(*y, dydt, &y_tmp, step_try, rhs, context);

		//NOTE(martin): scale the error by eps_scale and eps
		//             (this allows a variety of proportional vs. constant step error control)
		scaled_err = fabs(err/eps);

		LOG_DEBUG("[%i] err = %f, scaledErr = %f\n", *itCount, err, scaled_err);

		*itCount = *itCount+1;
		if(scaled_err <= 1)
		{
			//NOTE(martin): step succeeded
			break;
		}
		else
		{
			LOG_DEBUG("reducing step size\n");
			//NOTE(martin): step failed, reduce step size, but no more than by a factor of 10
			double step_tmp = safety * step_try * pow(scaled_err, shrinkPower);
			double next_step_try = step_try >= 0. ? maximum(step_tmp, 0.1 * step_try) : minimum(step_tmp, 0.1 * step_try);

			if(fabs(next_step_try) <= min_step_size)
			{
				LOG_WARNING("step size too small\n");
				goto common_return;
			}
			else
			{
				//NOTE(martin): check for step size underflow
				double t_next = *t + next_step_try;
				if(t_next == *t)
				{
					//NOTE(martin): an underflow occurred
					LOG_WARNING("Step underflow\n");
					goto common_return;
				}
				else
				{
					step_try = next_step_try;
				}
			}
		}
	}

	LOG_DEBUG("increasing step size\n");
	if(scaled_err > errGuard)
	{
		*step = safety * step_try * pow(scaled_err, growPower);
	}
	else
	{
		*step = 5 * step_try;
	}

common_return:
	*y = y_tmp;
	*t += step_try;
	*total_err += fabs(err);

}

int rkck_autonomous(double* y_out,
		    double y_start,
		    double t_start,
		    double t_end,
		    double step_guess,
		    double min_step_size,
		    int max_step_count,
		    double eps,
		    rkck_autonomous_rhs rhs,
		    void* context,
		    int* itCount,
		    double* estErr)
{
	double y = y_start;
	double t = t_start;
	double step = step_guess;
	*itCount = 0;
	*estErr = 0;

	for(int i=0; i<max_step_count; i++)
	{
		//NOTE(martin): adjust step in case of possible overshoot
		if((t + step - t_end)*(t + step - t_start) > 0)
		{
			//NOTE(martin): step could underflow here the very first iteration, but not in the other, because
			//              that would have exited the loop in the previous iteration
			//              since we provide a guess that is a fraction of the interval, it should not occur
			//              (unless for degenerate small intervals, that we should avoid anyway)
			step = t_end - t;
		}

		//NOTE(martin): scale eps to use a sum of the errors relative to y and to the first increment
		//              add a tiny number to avoid divisions by zero.
		double dydt = rhs(y, context); //TODO(martin): return dydt from rkck_autonomous_adaptive_step() ?
		double scaled_eps = eps * (fabs(y) + fabs(step*dydt)) + 1e-30;

		//NOTE(martin): do one adaptive Cash-Karp step.
		rkck_autonomous_adaptive_step(&y, &t, &step, rhs, context, scaled_eps, min_step_size, itCount, estErr);

		if((t-t_end)*(t-t_start) >= 0)
		{
			//NOTE(martin): t reached the end of the interval
			*y_out = y;
			return(0);
		}
	}
	LOG_ERROR("too many steps\n");
	return(-1);
}

//------------------------------------------------------------------------------------------------------
// Simple Runge Kutta integration
//------------------------------------------------------------------------------------------------------
typedef double (*deriv_function_ptr)(double x, void* context);

double rkck_integrate_step(double y_in,
		 double t,
		 double dydt,
		 double* y_out,
		 double* dydt_out,
		 double step,
		 deriv_function_ptr deriv,
		 void* context)
{
	//NOTE(martin): hard-coded Runge-Kutta Cash-Karp step for ODE y'(t) = f(t)
	//              (ie. suitable for a simple integrator)

	double k1 = dydt;
	double k3 = deriv(t + 3./10 * step, context);
	double k4 = deriv(t + 3./5 * step, context);
	double k5 = deriv(t + step, context);
	double k6 = deriv(t + 7./8 * step, context);

	*dydt_out = k5;

	*y_out = y_in + step * (37./378 * k1
			       + 250./621 * k3
			       + 125./594 * k4
			       + 512./1771 * k6);

	double y_low = y_in + step * (2825./27648. * k1
	                              + 18575./48384 * k3
				      + 13525./55296 * k4
				      + 277./14336 * k5
				      + 1./4 * k6);

	return(*y_out - y_low);
}

void rkck_integrate_adaptive_step(double* y,
			double* t,
			double* dydt,
			double* step,
			deriv_function_ptr deriv,
			void* context,
			double eps,
			double min_step_size,
			int *itCount,
			double* total_err)
{
	//NOTE(martin): this routine computes an adaptive Cash-Karp step for an ODE of the form y'(t) = f(t)
	//              (ie. suitable for a simple integrator)
	//              y and t will be updated with the new values of y and t
	//
	//              dydt is the derivative at the beginning of the step and will be updated with the derivative
	//              at the end of the step, which can be reused in subsequent steps.
	//
	//              step will be updated with the next guess for step
	//
	//              eps_scale is used to control either the absolute error value or the relative error to y or step values
	//
	//              itCount serves to update the total number of iteration computed
	//
	//              estErr is used to accumulate the total estimated error

	//NOTE(martin): constants used to grow/shrink the step size
	//              errGuard equals (5/safety) raised to the power (1/growPower)
	//              it ensures we don't expand the step size by more than a factor of 5

	const double safety = 0.9;
	const double growPower = -0.2;
	const double shrinkPower = -0.25;
	const double errGuard = 1.89e-4;

	double step_try = *step;
	double y_tmp = 0;
	double dydt_tmp = 0;
	double scaled_err = 0;
	double err = 0;

	while(1)
	{
		err = rkck_integrate_step(*y, *t, *dydt, &y_tmp, &dydt_tmp, step_try, deriv, context);

		//NOTE(martin): scale the error by eps_scale and eps
		//             (this allows a variety of proportional vs. constant step error control)
		scaled_err = fabs(err/eps);

		LOG_DEBUG("[%i] err = %f, scaledErr = %f\n", *itCount, err, scaled_err);

		*itCount = *itCount+1;
		if(scaled_err <= 1)
		{
			//NOTE(martin): step succeeded
			break;
		}
		else
		{
			LOG_DEBUG("reducing step size\n");
			//NOTE(martin): step failed, reduce step size, but no more than by a factor of 10
			double step_tmp = safety * step_try * pow(scaled_err, shrinkPower);
			double next_step_try = step_try >= 0. ? maximum(step_tmp, 0.1 * step_try) : minimum(step_tmp, 0.1 * step_try);

			if(fabs(next_step_try) <= min_step_size)
			{
				LOG_WARNING("step size too small\n");
				goto common_return;
			}
			else
			{
				//NOTE(martin): check for step size underflow
				double t_next = *t + next_step_try;
				if(t_next == *t)
				{
					LOG_WARNING("Step underflow\n");
					goto common_return;
				}
				else
				{
					step_try = next_step_try;
				}
			}
		}
	}

	LOG_DEBUG("increasing step size\n");
	if(scaled_err > errGuard)
	{
		*step = safety * step_try * pow(scaled_err, growPower);
	}
	else
	{
		*step = 5 * step_try;
	}

common_return:
	*t += step_try;
	*y = y_tmp;
	*dydt = dydt_tmp;
	*total_err += fabs(err);
}

int rkck_integrate(double* y_out,
                   double y_start,
		   double t_start,
		   double t_end,
		   double step_guess,
		   double min_step_size,
		   int max_step_count,
		   double eps,
		   deriv_function_ptr deriv,
		   void* context,
		   int* itCount,
		   double* estErr)
{
	double y = y_start;
	double t = t_start;
	double dydt = deriv(t_start, context);
	double step = step_guess;
	*itCount = 0;
	*estErr = 0;

	for(int i=0; i<max_step_count; i++)
	{
		//NOTE(martin): adjust step in case of possible overshoot
		if((t + step - t_end)*(t + step - t_start) > 0)
		{
			//NOTE(martin): step could underflow here the very first iteration, but not in the other, because
			//              that would have exited the loop in the previous iteration
			//              since we provide a guess that is a fraction of the interval, it should not occur
			//              (unless for degenerate small intervals, that we should avoid anyway)
			step = t_end - t;
		}

		//NOTE(martin): scale eps to use a sum of the errors relative to y and to the first increment
		//              add a tiny number to avoid divisions by zero.
		double scaled_eps = eps * (fabs(y) + fabs(step*dydt)) + 1e-30;

		//NOTE(martin): do one adaptive Cash-Karp step.
		rkck_integrate_adaptive_step(&y, &t, &dydt, &step, deriv, context, scaled_eps, min_step_size, itCount, estErr);

		if((t-t_end)*(t-t_start) >= 0)
		{
			//NOTE(martin): t reached the end of the interval
			*y_out = y;
			return(0);
		}
	}
	LOG_ERROR("too many steps\n");
	return(-1);
}

//------------------------------------------------------------------------------------------------------
// Bezier curve functions
//------------------------------------------------------------------------------------------------------

void bezier_coeffs_init_with_control_points(bezier_coeffs* coeffs, f64 p0x, f64 p0y, f64 p1x, f64 p1y, f64 p2x, f64 p2y, f64 p3x, f64 p3y)
{
	/*NOTE(martin): convert the control points to the power basis, multiplying by M3

		     | 1  0  0  0|
		M3 = |-3  3  0  0|
		     | 3 -6  3  0|
		     |-1  3 -3  1|
		ie:
		    c0 = p0
		    c1 = -3*p0 + 3*p1
		    c2 = 3*p0 - 6*p1 + 3*p2
		    c3 = -p0 + 3*p1 - 3*p2 + p3
	*/
	coeffs->cx[0] = p0x;
	coeffs->cx[1] = -3*p0x + 3*p1x;
	coeffs->cx[2] = 3*p0x - 6*p1x + 3*p2x;
	coeffs->cx[3] = -p0x + 3*p1x - 3*p2x + p3x;

	coeffs->cy[0] = p0y;
	coeffs->cy[1] = -3*p0y + 3*p1y;
	coeffs->cy[2] = 3*p0y - 6*p1y + 3*p2y;
	coeffs->cy[3] = -p0y + 3*p1y - 3*p2y + p3y;
}

double bezier_sample_x(bezier_coeffs* coeffs, double s)
{
	return(((coeffs->cx[3]*s + coeffs->cx[2])*s + coeffs->cx[1])*s + coeffs->cx[0]);
}

double bezier_sample_y(bezier_coeffs* coeffs, double s)
{
	return(((coeffs->cy[3]*s + coeffs->cy[2])*s + coeffs->cy[1])*s + coeffs->cy[0]);
}

double bezier_dxds(bezier_coeffs* coeffs, double s)
{
	return((3*coeffs->cx[3]*s + 2*coeffs->cx[2])*s + coeffs->cx[1]);
}

double bezier_dyds(bezier_coeffs* coeffs, double s)
{
	return((3*coeffs->cy[3]*s + 2*coeffs->cy[2])*s + coeffs->cy[1]);
}

double bezier_solve_x(bezier_coeffs* coeffs, double x)
{
	// find s parameter for a given value of x

	//TODO: assert good value of x
	//      pass epsilon
	const double epsilon = 1e-12;

	//NOTE(martin): do 12 Newton-Raphson iteration first
	double s = 0.5;
	double delta = bezier_sample_x(coeffs, s) - x;

	for(int i=0; i<12; i++)
	{
		double coeff = bezier_dxds(coeffs, s);
		if(coeff < 1e-9)
		{
			LOG_WARNING("derivative is too small, can't continue\n");
			break;
		}
		s -= delta/coeff;
		delta = bezier_sample_x(coeffs, s) - x;

		if(fabs(delta) <= epsilon)
		{
			return(s);
		}
	}

	//NOTE(martin): fallback to the secant method
	//TODO check good value of last guess s

	//LOG_WARNING("bezier solver fallback to secant\n");
	//LOG_WARNING("bezier solver fallback to secant\n");

	if(s < 0 || s > 1)
	{
		s = 0.5;
	}
	double s0 = 0;
	double s1 = 1;
	while(s0 < s1)
	{
		delta = bezier_sample_x(coeffs, s) - x;

		if(fabs(delta) <= epsilon)
		{
			return(s);
		}
		if(delta > 0)
		{
			s1 = s;
		}
		else
		{
			s0 = s;
		}
		double s_tmp = (s1+s0)/2;
		if(s_tmp == s)
		{
			break;
		}
		s = s_tmp;
	}

	return(s);
}

//------------------------------------------------------------------------------------------------------
// Bezier tempo curve integrations
//------------------------------------------------------------------------------------------------------

double bezier_tempo_get_position_callback(double s, void* context)
{
	bezier_coeffs* coeffs = (bezier_coeffs*)context;
	return(bezier_sample_y(coeffs, s)*bezier_dxds(coeffs, s));
}

double bezier_tempo_get_position(bezier_coeffs* coeffs, double t)
{
	//NOTE(martin): given an non-autonomous tempo curve (representing the tempo with respect to time),
	//              find the position corresponding to a given time.

	//NOTE(martin): to find the position, we integrate the tempo curve from 0 to t. We make a variable
	//              change to integrate over s rather than t, which allows to compute parameter s only
	//              for the bound t.

	double s = bezier_solve_x(coeffs, t);

	double maxErr = 1e-9;
	double step_guess = 0.1;
	double result = 0;
	int itCount = 0;
	double estErr = 0;
	double min_step_size = 1e-9;
	u32 max_step_count = 10000;

	rkck_integrate(&result, 0, 0, s, step_guess, min_step_size, max_step_count, maxErr, bezier_tempo_get_position_callback, (void*)coeffs, &itCount, &estErr);

	return(result);
}

double bezier_tempo_get_time_callback(double t, void* context)
{
	bezier_coeffs* coeffs = (bezier_coeffs*)context;
	double s = bezier_solve_x(coeffs, t);
	return(1./bezier_sample_y(coeffs, s));
}

double bezier_tempo_get_time(bezier_coeffs* coeffs, double p)
{
	//NOTE(martin): given a non-autonomous tempo curve (representing the tempo with respect to time),
	//              find the time corresponding to a given position.

	//NOTE(martin): let P(x) be the position for time x, and T(x) the time for position x,
	//              the tempo curve C(t) gives the tempo P'(t) for time t.
	//              Note that P'(t) = [T^(-1)]'(t) = 1/T'[T^(-1)(x)]
	//		and so T'[T^(-1)(x)] = 1/P'(x)
	//
	//              hence, to find t for a given p we must solve the autonomous ODE T'(p) = 1/C(T(p)), (ie y' = 1/C(y))

	double maxErr = 1e-9;
	double step_guess = 0.1;
	double result = 0;
	int itCount = 0;
	double estErr = 0;
	double y_start = 0;
	double t_start = 0;
	double t_end = p;
	double min_step_size = 1e-9;
	u32 max_step_count = 10000;

	rkck_autonomous(&result, y_start, t_start, t_end, step_guess, min_step_size, max_step_count, maxErr, bezier_tempo_get_time_callback, (void*)coeffs, &itCount, &estErr);
	return(result);
}

double bezier_autonomous_tempo_get_time_callback(double s, void* context)
{
	bezier_coeffs* coeffs = (bezier_coeffs*)context;
	return(bezier_dxds(coeffs, s)/bezier_sample_y(coeffs, s));
}

double bezier_autonomous_tempo_get_time(bezier_coeffs* coeffs, double p)
{
	//NOTE(martin): given an autonomous tempo curve (representing the tempo with respect to the timescale position),
	//              find the time corresponding to a given position.

	//NOTE(martin): the curve C(p) = P'(T(p)) so to get the tempo we use T'(p) = 1/P'(T(p)) = 1/C(p)
	//              We then integrate this function to get the time T for a given position p
	//              We do a variable change in the integral to integrate over s rather than p, which
	//              allows to compute parameter s only for the bound p

	double s = bezier_solve_x(coeffs, p);

	double maxErr = 1e-9;
	double step_guess = 0.1;
	double result = 0;
	int itCount = 0;
	double estErr = 0;
	double min_step_size = 1e-9;
	u32 max_step_count = 10000;

	rkck_integrate(&result, 0, 0, s, step_guess, min_step_size, max_step_count, maxErr, bezier_autonomous_tempo_get_time_callback, (void*)coeffs, &itCount, &estErr);
	return(result);
}

double bezier_autonomous_tempo_get_position_callback(double p, void* context)
{
	bezier_coeffs* coeffs = (bezier_coeffs*)context;
	double s = bezier_solve_x(coeffs, p);
	return(bezier_sample_y(coeffs, s));
}

double bezier_autonomous_tempo_get_position(bezier_coeffs* coeffs, double time)
{
	//NOTE(martin): given an autonomous tempo curve (representing the tempo with respect to the timescale position),
	//              find the position corresponding to a given time.

	//NOTE(martin): let P(x) be the position for time x, and T(x) the time for position x,
	//              the tempo curve C(t) gives the tempo P'(t) for time t.
	//
	//              hence, to find p for a given t we must solve the autonomous ODE P'(t) = C(P(t)), (ie y' = C(y))

	double maxErr = 1e-9;
	double step_guess = 0.1;
	double result = 0;
	int itCount = 0;
	double estErr = 0;
	double y_start = 0;
	double t_start = 0;
	double t_end = time;
	double min_step_size = 1e-9;
	u32 max_step_count = 10000;

	rkck_autonomous(&result, y_start, t_start, t_end, step_guess, min_step_size, max_step_count, maxErr, bezier_autonomous_tempo_get_position_callback, (void*)coeffs, &itCount, &estErr);
	return(result);
}

//------------------------------------------------------------------------------------------------------
// tempo curves integration
//------------------------------------------------------------------------------------------------------

f64 sched_pos_tempo_integrate_over_time(sched_curve_elt* elt, f64 t)
{
	//NOTE(martin): do the integration for the remaining of time update in that element
	//for now we consider that the curve is a linear patch and gives the scaling for a given position
	f64 posUpdate = 0;
	f64 C0 = elt->startValue;

	switch(elt->type)
	{
		case SCHED_CURVE_CONST:
		{
			posUpdate = t*C0;
		} break;

		case SCHED_CURVE_LINEAR:
		{
			f64 alpha = (elt->endValue - elt->startValue)/elt->length; //TODO(martin): could precompute

			if(abs(alpha) > 1e-9)
			{
				posUpdate = C0/alpha*(exp(alpha*t) - 1);
			}
			else
			{
				//NOTE(martin): if alpha is approaching zero, the above formula would diverge and provide inaccurate results.
				//              (and possibly raise a divivison-by-zero exception).
				//              Instead, we take the fourth-order series expansion.
				f64 t2 = t*t;
				f64 t3 = t2*t;
				f64 t4 = t2*t2;
				f64 t5 = t4*t;
				f64 alpha2 = alpha*alpha;
				f64 alpha3 = alpha2*alpha;
				f64 alpha4 = alpha2*alpha2;

				posUpdate = C0*(t + alpha*t2/2 + alpha2*t3/6 + alpha3*t4/24 + alpha4*t5/120);
			}
		} break;

		case SCHED_CURVE_BEZIER:
		{
			posUpdate = bezier_autonomous_tempo_get_position(&elt->coeffs, t);
		} break;
	}
	return(posUpdate);
}

f64 sched_pos_tempo_integrate_over_pos(sched_curve_elt* elt, f64 p)
{
	//NOTE(martin): do the integration for the remaining of pos update in that element
	//for now we consider that the curve is a linear patch and gives the scaling for a given position
	f64 timeUpdate = 0;
	f64 C0 = elt->startValue;

	switch(elt->type)
	{
		case SCHED_CURVE_CONST:
		{
			timeUpdate = p/C0;
		} break;

		case SCHED_CURVE_LINEAR:
		{
			f64 alpha = (elt->endValue - elt->startValue)/elt->length;

			if(abs(alpha) > 1e-9)
			{
				timeUpdate = log((C0 + alpha*p)/C0)/alpha;
			}
			else
			{
				//NOTE(martin): if alpha is approaching zero, the above formula would diverge and provide inaccurate results.
				//              (and possibly raise a division-by-zero exception).
				//              Instead, we take the fourth-order series expansion.
				f64 p2 = p*p;
				f64 p3 = p2*p;
				f64 p4 = p2*p2;
				f64 p5 = p4*p;
				f64 C02 = C0*C0;
				f64 C03 = C02*C0;
				f64 C04 = C02*C02;
				f64 C05 = C04*C0;
				f64 alpha2 = alpha*alpha;
				f64 alpha3 = alpha2*alpha;
				f64 alpha4 = alpha2*alpha2;

				timeUpdate = p/C0 - alpha*p2/(2*C02) + alpha2*p3/(3*C03) - alpha3*p4/(4*C04) + alpha4*p5/(5*C05);
			}
		} break;

		case SCHED_CURVE_BEZIER:
		{
			timeUpdate = bezier_autonomous_tempo_get_time(&elt->coeffs, p);
		} break;
	}
	return(timeUpdate);
}

f64 sched_time_tempo_integrate_over_time(sched_curve_elt* elt, f64 t)
{
	//NOTE(martin): do the integration for the remaining of time update in that element
	//for now we consider that the curve is a linear patch and gives the scaling for a given position
	f64 posUpdate = 0;
	f64 C0 = elt->startValue;

	switch(elt->type)
	{
		case SCHED_CURVE_CONST:
		{
			posUpdate = t*C0;
		} break;

		case SCHED_CURVE_LINEAR:
		{
			f64 alpha = (elt->endValue - elt->startValue)/elt->length; //TODO(martin): could precompute
			posUpdate = C0*t + 0.5*alpha*t*t;
		} break;

		case SCHED_CURVE_BEZIER:
		{
			posUpdate = bezier_tempo_get_position(&elt->coeffs, t);
		} break;
	}
	return(posUpdate);
}

f64 sched_time_tempo_integrate_over_pos(sched_curve_elt* elt, f64 p)
{
	//NOTE(martin): do the integration for the remaining of pos update in that element
	//for now we consider that the curve is a linear patch and gives the scaling for a given position
	f64 timeUpdate = 0;
	f64 C0 = elt->startValue;

	switch(elt->type)
	{
		case SCHED_CURVE_CONST:
		{
			timeUpdate = p/C0;
		} break;

		case SCHED_CURVE_LINEAR:
		{
			f64 alpha = (elt->endValue - elt->startValue)/elt->length;

			if(abs(alpha) > 1e-9)
			{
				timeUpdate = (sqrt(C0*C0 + 2*alpha*p) - C0)/alpha;
			}
			else
			{
				//NOTE(martin): if alpha is approaching zero, the above formula would diverge and provide inaccurate results.
				//              (and possibly raise a division-by-zero exception).
				//              Instead, we take the fourth-order series expansion.
				f64 p2 = p*p;
				f64 p3 = p2*p;
				f64 p4 = p2*p2;
				f64 p5 = p4*p;

				f64 C02 = C0*C0;
				f64 C03 = C0*C02;
				f64 C05 = C03*C02;
				f64 C07 = C05*C02;
				f64 C09 = C07*C02;

				f64 alpha2 = alpha*alpha;
				f64 alpha3 = alpha2*alpha;
				f64 alpha4 = alpha2*alpha2;

				timeUpdate = p/C0 - alpha*p2/(2*C03) + alpha2*p3/(2*C05) - alpha3*p4*5/(8*C07) + alpha4*p5*7/(8*C09);
			}
		} break;

		case SCHED_CURVE_BEZIER:
		{
			timeUpdate = bezier_tempo_get_time(&elt->coeffs, p);
		} break;
	}
	return(timeUpdate);
}


//------------------------------------------------------------------------------------------------------
// curves create/destroy
//------------------------------------------------------------------------------------------------------

int sched_curve_replace(sched_curve* curve, sched_curve_descriptor* descriptor)
{
	//NOTE(martin): recompute the whole curve from descriptor
	curve->axes = descriptor->axes;
	curve->eltCount = descriptor->eltCount;

	//NOTE(martin): initialize elements with descriptor elements, and precompute breakpoints values
	f64 start = 0;
	f64 transformedStart = 0;

	for(int i=0; i<descriptor->eltCount; i++)
	{
		sched_curve_descriptor_elt* descElt = &(descriptor->elements[i]);
		sched_curve_elt* elt = &(curve->elements[i]);

		elt->type = descElt->type;
		elt->startValue = descElt->startValue;
		elt->endValue = descElt->endValue;
		elt->length = descElt->length;

		if(elt->length == 0 && elt->type != SCHED_CURVE_CONST)
		{
			LOG_ERROR("non-const zero length element in curve descriptor\n");
			return(-1);
		}
		if(elt->startValue <= 0 || (elt->endValue <= 0 && elt->type != SCHED_CURVE_CONST))
		{
			LOG_ERROR("negative or null tempo in curve descriptor\n");
			return(-2);
		}

		switch(elt->type)
		{
			case SCHED_CURVE_CONST:
				elt->endValue = elt->startValue;
				break;

			case SCHED_CURVE_BEZIER:
			{
				//TODO(martin): check control points constraints !
				f64 p0x = 0;
				f64 p0y = elt->startValue;
				f64 p1x = descElt->p1x * elt->length;
				f64 p1y = descElt->p1y * (elt->endValue-elt->startValue) + elt->startValue;
				f64 p2x = descElt->p2x * elt->length;
				f64 p2y = descElt->p2y * (elt->endValue-elt->startValue) + elt->startValue;
				f64 p3x = elt->length;
				f64 p3y = elt->endValue;

				bezier_coeffs_init_with_control_points(&elt->coeffs, p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y);
			} break;

			//TODO(martin): precompute slope for linear elements
			default:
				break;
		}
		//TODO(martin): hoist that up ?
		switch(curve->axes)
		{
			case SCHED_CURVE_POS_TEMPO:
				elt->transformedLength = sched_pos_tempo_integrate_over_pos(elt, elt->length);
				break;
			case SCHED_CURVE_TIME_TEMPO:
				elt->transformedLength = sched_time_tempo_integrate_over_time(elt, elt->length);
				break;
		}
		elt->start = start;
		elt->transformedStart = transformedStart;
		elt->end = start + elt->length;
		elt->transformedEnd = transformedStart + elt->transformedLength;

		start = elt->end;
		transformedStart = elt->transformedEnd;
	}
	return(0);
}

sched_curve* sched_curve_create(sched_curve_descriptor* descriptor)
{
	sched_curve* curve = (sched_curve*)malloc(sizeof(sched_curve) + sizeof(sched_curve_elt) * descriptor->eltCount);
	curve->elements = (sched_curve_elt*)(((char*)curve) + sizeof(sched_curve));
	curve->eltCount = descriptor->eltCount;

	if(sched_curve_replace(curve, descriptor))
	{
		sched_curve_destroy(curve);
		return(0);
	}
	else
	{
		return(curve);
	}
}

void sched_curve_destroy(sched_curve* curve)
{
	free(curve);
}

//------------------------------------------------------------------------------------------------------
// find curve element for time/pos in a curve
//------------------------------------------------------------------------------------------------------

#define sched_curve_elt_start_time(curve, elt) \
	((curve->axes == SCHED_CURVE_POS_TEMPO)? elt->transformedStart : elt->start)

#define sched_curve_elt_start_pos(curve, elt) \
	((curve->axes == SCHED_CURVE_POS_TEMPO)? elt->start : elt->transformedStart)

#define sched_curve_elt_end_time(curve, elt) \
	((curve->axes == SCHED_CURVE_POS_TEMPO)? elt->transformedEnd : elt->end)

#define sched_curve_elt_end_pos(curve, elt) \
	((curve->axes == SCHED_CURVE_POS_TEMPO)? elt->end : elt->transformedEnd)


sched_curve_elt* sched_curve_find_element_for_time(sched_curve* curve, f64 time, f64* outStartTime, f64* outStartPos)
{
	//TODO could return status of which side we were on if outside the curve ?
	f64 eltStartPos = 0;
	f64 eltStartTime = 0;
	sched_curve_elt* foundElt = 0;

	//WARN(martin): the loop is done on a temporary element (elt) instead of directly on foundElt.
	//              this is so that we can return a 0 elt if no matching interval was found,
	//              but still correctly return last starting positions and time.
	//              So don't iterate directly on foundElt because that would introduce a (not so) subtle bug !
	//              That seems stupid, but eerf... been there, done that.

	u32 eltCount = curve->eltCount;
	for(int i=0; i<eltCount; i++)
	{
		sched_curve_elt* elt = &(curve->elements[i]);
		f64 eltEndTime = sched_curve_elt_end_time(curve, elt);
		if(eltEndTime >= time)
		{
			//NOTE(martin): since elements are contiguous and sorted, we pick the first whose end
			//              is greater or equal to time
			foundElt = elt;
			break;
		}
		eltStartPos = sched_curve_elt_end_pos(curve, elt);
		eltStartTime = sched_curve_elt_end_time(curve, elt);
	}
	*outStartPos = eltStartPos;
	*outStartTime = eltStartTime;
	return(foundElt);
}

sched_curve_elt* sched_curve_find_element_for_pos(sched_curve* curve, f64 pos, f64* outStartTime, f64* outStartPos)
{
	f64 eltStartPos = 0;
	f64 eltStartTime = 0;
	sched_curve_elt* foundElt = 0;

	u32 eltCount = curve->eltCount;
	for(int i=0; i<eltCount; i++)
	{
		sched_curve_elt* elt = &(curve->elements[i]);
		f64 eltEndPos = sched_curve_elt_end_pos(curve, elt);
		if(eltEndPos >= pos)
		{
			//NOTE(martin): since elements are contiguous and sorted, we pick the first whose end
			//              is greater or equal to time
			foundElt = elt;
			break;
		}
		eltStartPos = sched_curve_elt_end_pos(curve, elt);
		eltStartTime = sched_curve_elt_end_time(curve, elt);
	}
	*outStartPos = eltStartPos;
	*outStartTime = eltStartTime;
	return(foundElt);
}

//------------------------------------------------------------------------------------------------------
// curves time/pos conversions
//------------------------------------------------------------------------------------------------------

int sched_curve_get_position_from_time(sched_curve* curve, f64 time, f64* outPos)
{
	//NOTE(martin): select which curve element we must integrate
	f64 eltStartPos = 0;
	f64 eltStartTime = 0;

	sched_curve_elt* elt = sched_curve_find_element_for_time(curve, time, &eltStartTime, &eltStartPos);

	if(!elt)
	{
		//NOTE(martin): extend end tempo after the end of the curve.
		//NOTE(martin): update pos with a constant tempo equal to the last known tempo
		sched_curve_elt* endElt = &(curve->elements[(curve->eltCount)-1]);
		f64 endTempo = endElt->endValue;
		f64 offset = time - eltStartTime;
		*outPos = eltStartPos + offset*endTempo;
		return(1);
	}
	else
	{
		//NOTE(martin): do the integration for the remaining of time update in that element
		f64 t = time - eltStartTime;

		switch(curve->axes)
		{
			case SCHED_CURVE_POS_TEMPO:
			{
				f64 update = sched_pos_tempo_integrate_over_time(elt, t);
				*outPos = eltStartPos + update;
			} break;

			case SCHED_CURVE_TIME_TEMPO:
			{
				f64 update = sched_time_tempo_integrate_over_time(elt, t);
				*outPos = eltStartPos + update;
			} break;
		}
		return(0);
	}
}

int sched_curve_get_time_from_position(sched_curve* curve, f64 pos, f64* outTime)
{
	//NOTE(martin): select which curve element we must integrate
	f64 eltStartPos = 0;
	f64 eltStartTime = 0;
	sched_curve_elt* elt = sched_curve_find_element_for_pos(curve, pos, &eltStartTime, &eltStartPos);

	if(!elt)
	{
		//NOTE(martin): extend end tempo after the end of the curve.
		//NOTE(martin): update with a constant tempo equal to the last known tempo
		sched_curve_elt* endElt = &(curve->elements[(curve->eltCount)-1]);
		f64 endTempo = endElt->endValue;
		f64 offset = pos - eltStartPos;
		*outTime = eltStartTime + offset/endTempo;
		return(1);
	}
	else
	{
		//NOTE(martin): do the integration for the remaining of pos update in that element
		f64 p = pos - eltStartPos;

		switch(curve->axes)
		{
			case SCHED_CURVE_POS_TEMPO:
			{
				f64 update = sched_pos_tempo_integrate_over_pos(elt, p);
				*outTime = eltStartTime + update;
			} break;

			case SCHED_CURVE_TIME_TEMPO:
			{
				f64 update = sched_time_tempo_integrate_over_pos(elt, p);
				*outTime = eltStartTime + update;
			} break;
		}
		return(0);
	}
}

#undef LOG_SUBSYSTEM
