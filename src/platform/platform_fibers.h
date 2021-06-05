/************************************************************//**
*
*	@file: platform_fibers.h
*	@author: Martin Fouilleul
*	@date: 27/05/2021
*	@revision:
*
*****************************************************************/
#ifndef __PLATFORM_FIBERS_H_
#define __PLATFORM_FIBERS_H_

#include"typedefs.h"

struct fiber_context;
typedef i64 (*fiber_fun)(fiber_context*);

struct fiber_context
{
	//WARN(martin): we use the fact that sp has the same address as fiber,
	//              so be careful to keep it as the first field
	void* sp;
	fiber_fun function;
	void* user;
	bool running;
	i64 exitCode;
};

fiber_context* fiber_init(fiber_fun function, unsigned long long stackSize, char* stack);
void fiber_yield(fiber_context* info);
extern "C" void _fiber_bootstrap(fiber_context* info);

#endif //__PLATFORM_FIBERS_H_
