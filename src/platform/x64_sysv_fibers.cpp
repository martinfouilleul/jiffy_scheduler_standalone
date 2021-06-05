/************************************************************//**
*
*	@file: x64_sysv_fibers.cpp
*	@author: Martin Fouilleul
*	@date: 27/05/2021
*	@revision:
*
*****************************************************************/
#include"platform_fibers.h"

//----------------------------------------------------------------------------------
// Fiber context switching
//----------------------------------------------------------------------------------

fiber_context* fiber_init(fiber_fun function, unsigned long long stackSize, char* stack)
{
	fiber_context* info = (fiber_context*)stack;
	info->function = function;
	info->user = 0;
	info->running = true;

	//NOTE(martin): stack is growing downwards and 16 byte aligned
	info->sp = (char*)((unsigned long long)(stack + stackSize) & ~0x0f);

	//TODO(martin): add guard pages at begining and end of stack

	asm(//NOTE(martin): we first save info (which is also &info->sp) in rdi before clobbering the red zone
	    "mov %0, %%rdi \n"
	    //NOTE(martin): store info in rax to return it from FiberInit() when we yield back (see _fiber_bootstrap)
	    "mov %%rdi, %%rax \n"
	    //NOTE(martin): now we save rbx, rbp, and r12-r15, that are callee-saved
	    "push %%rbx \n"
	    "push %%rbp \n"
	    "push %%r12 \n"
	    "push %%r13 \n"
	    "push %%r14 \n"
	    "push %%r15 \n"
	    //NOTE(martin): swap stack pointers
	    "mov %%rsp, %%rsi \n"
	    "mov (%%rdi), %%rsp \n"
	    "mov %%rsi, (%%rdi) \n"
	    //NOTE(martin): align new stack to 16 bytes
	    "pushq $0 \n"
	    //NOTE(martin): Jump to _fiber_bootstrap(). This function will yield immediately,
	    //              which will have the effect to return from FiberInit()
	    "jmp __fiber_bootstrap"
	    ::"r" (info)
	    : "rdi", "rsi", "rax", "memory");

	//NOTE(martin): we actually don't return from here, but it's here to avoid a warning
	return(info);
}

void fiber_yield(fiber_context* info)
{
	//NOTE(martin): we need to save rbx, rbp, and r12-r15, that are callee-saved
	asm(//NOTE(martin): we first save info (which is also &info->sp) in rdi before clobbering the red zone
	    "mov %0, %%rdi \n"
	    //NOTE(martin): now we save rbx, rbp, and r12-r15, that are callee-saved
	    "push %%rbx \n"
	    "push %%rbp \n"
	    "push %%r12 \n"
	    "push %%r13 \n"
	    "push %%r14 \n"
	    "push %%r15 \n"
	    //NOTE(martin): swap stack pointers
	    "mov %%rsp, %%rsi \n"
	    "mov (%%rdi), %%rsp \n"
	    "mov %%rsi, (%%rdi) \n"
	    //NOTE(martin): restore saved registers
	    "pop %%r15 \n"
	    "pop %%r14 \n"
	    "pop %%r13 \n"
	    "pop %%r12 \n"
	    "pop %%rbp \n"
	    "pop %%rbx \n"
	    ::"r" (info)
	    : "rdi", "rsi", "rax", "memory");
}

extern "C" void _fiber_bootstrap(fiber_context* info)
{
	//NOTE(martin): this will yield back and return from FiberInit()
	fiber_yield(info);

	//NOTE(martin): now the user has called yield for the first time, so we call the fiber function
	info->exitCode = info->function(info);

	//NOTE(martin): the function has returned, so we mark the fiber as finished and we yield back
	info->running = false;
	fiber_yield(info);

	//NOTE(martin): now, every time the user tries to yield to us we just yield back
	while(1)
	{
		//printf("error: yielding to a finished fiber\n");
		fiber_yield(info);
	}
}
