/************************************************************//**
*
*	@file: platform_clock.h
*	@author: Martin Fouilleul
*	@date: 07/03/2019
*	@revision:
*
*****************************************************************/
#ifndef __PLATFORM_CLOCK_H_
#define __PLATFORM_CLOCK_H_

#include"typedefs.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

typedef enum { SYS_CLOCK_MONOTONIC, // clock that increment monotonically
               SYS_CLOCK_UPTIME,    // clock that increment monotonically during uptime
	       SYS_CLOCK_DATE       // clock that is driven by the platform time
	     } clock_kind;

void ClockSystemInit(); // initialize the clock subsystem
u64  ClockGetTimestamp(clock_kind clock);
f64  ClockGetTime(clock_kind clock);
void ClockSleepNanoseconds(u64 nanoseconds); // sleep for a given number of nanoseconds

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus


#endif //__PLATFORM_CLOCK_H_
