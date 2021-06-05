/************************************************************//**
*
*	@file: scheduler.h
*	@author: Martin Fouilleul
*	@date: 09/07/2020
*	@revision:
*
*****************************************************************/
#ifndef __SCHEDULER_H_
#define __SCHEDULER_H_

#include"typedefs.h"
#include"lists.h"
#include"sched_curves.h"

//---------------------------------------------------------------
// Scheduler types
//---------------------------------------------------------------
typedef f64 sched_steps;

typedef i64(*sched_fiber_proc)(void* userPointer);
typedef void(*sched_action_callback)(void* userPointer);

typedef struct sched_object_handle { u64 h; } sched_object_handle;
typedef struct sched_task { u64 h; } sched_task;
typedef struct sched_fiber { u64 h; } sched_fiber;

#define sched_generic_handle(handle) (sched_object_handle){.h = (handle).h}

typedef u64 sched_object_signal;
const sched_object_signal SCHED_SIG_IDLE   = 0x01,
                          SCHED_SIG_COMPLETED = 0x01<<1;

typedef enum { SCHED_WAKEUP_INVALID_HANDLE,
               SCHED_WAKEUP_HANDLE_ERROR,
	       SCHED_WAKEUP_TIMEOUT,
	       SCHED_WAKEUP_SIGNALED,
	       SCHED_WAKEUP_CANCELLED } sched_wakeup_code;

//---------------------------------------------------------------
// Scheduler API
//---------------------------------------------------------------

//NOTE: start / end the scheduler. This will create a first task for the calling function
void sched_init();
void sched_end();

//NOTE: tasks
sched_task sched_task_create(sched_fiber_proc proc, void* userPointer);
sched_task sched_task_create_detached(sched_fiber_proc proc, void* userPointer);
sched_task sched_task_create_for_parent(sched_task parent, sched_fiber_proc proc, void* userPointer);
//TODO: sched_task_create_shared()

sched_task sched_task_self();
void sched_task_cancel(sched_task task);
void sched_task_suspend(sched_task task);
void sched_task_resume(sched_task task);

//NOTE: task's timescales
void sched_task_timescale_set_scaling(sched_task task, f64 scaling);
void sched_task_timescale_set_tempo_curve(sched_task task, sched_curve_descriptor* descriptor);

//NOTE(martin): fibers
sched_fiber sched_fiber_create(sched_fiber_proc proc, void* userPointer, sched_steps steps);
sched_fiber sched_fiber_create_for_task(sched_task task, sched_fiber_proc proc, void* userPointer, sched_steps steps);

sched_fiber sched_fiber_self();
void sched_fiber_cancel(sched_fiber fiber);
void sched_fiber_suspend(sched_fiber fiber);
void sched_fiber_resume(sched_fiber fiber);

//NOTE(martin): fiber self scheduling
void sched_cancel();
void sched_suspend();
void sched_wait(sched_steps steps);
void sched_fiber_put_on_object_waitlist(sched_fiber fiber, sched_object_handle handle, sched_object_signal signal, sched_steps timeout);
sched_wakeup_code sched_wait_for_handle_generic(sched_object_handle handle, sched_object_signal signal, sched_steps timeout);

#define sched_wait_for_handle(handle, signal, timeout) sched_wait_for_handle_generic(sched_generic_handle(handle), signal, timeout)
#define sched_wait_idling(handle) sched_wait_for_handle_generic(sched_generic_handle(handle), SCHED_SIG_IDLE, -1)
#define sched_wait_completion(handle) sched_wait_for_handle_generic(sched_generic_handle(handle), SCHED_SIG_COMPLETED, -1)

//NOTE(martin): handles management functions
int sched_handle_get_exit_code_generic(sched_object_handle handle, u64* exitCode);
void sched_handle_release_generic(sched_object_handle handle);
sched_object_handle sched_handle_duplicate_generic(sched_object_handle handle);

#define sched_handle_get_exit_code(handle, exitCode) _Generic((handle), \
                                                              sched_object_handle: sched_handle_get_exit_code_generic, \
                                                              sched_task: sched_handle_get_exit_code_generic, \
				                              sched_fiber: sched_handle_get_exit_code_generic)(sched_generic_handle(handle), exitCode)

#define sched_handle_release(handle) _Generic((handle), \
                                              sched_object_handle: sched_handle_release_generic, \
                                              sched_task: sched_handle_release_generic, \
				              sched_fiber: sched_handle_release_generic)(sched_generic_handle(handle))

#define sched_handle_duplicate(handle) _Generic((handle), \
					      sched_object_handle: sched_handle_duplicate_generic, \
                                              sched_task: sched_handle_duplicate_generic, \
				              sched_fiber: sched_handle_duplicate_generic)(sched_generic_handle(handle))

//NOTE(martin): background jobs
void sched_background();
void sched_foreground();

//NOTE(martin): buffered actions
void sched_action(sched_action_callback callback, u32 size, char* data);
void sched_action_no_copy(sched_action_callback callback, void* userPointer);

#endif //__SCHEDULER_H_
