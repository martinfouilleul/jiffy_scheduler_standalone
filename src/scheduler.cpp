/************************************************************//**
*
*	@file: scheduler.cpp
*	@author: Martin Fouilleul
*	@date: 09/07/2020
*	@revision:
*
*****************************************************************/
#include<string.h> // memset()
#include<math.h>
#include"macro_helpers.h"
#include"scheduler.h"
#include"memory.h"
#include"platform_fibers.h"

#define LOG_SUBSYSTEM "Scheduler"

//----------------------------------------------------------------------------------
// Timescales sync structures
//----------------------------------------------------------------------------------
typedef enum { SCHED_SYNC_CLOCK,
               SCHED_SYNC_TASK } sched_sync_source;

typedef enum { SCHED_SYNC_SCALING = 0,
               SCHED_SYNC_CURVE } sched_sync_type;

typedef struct sched_timescale_descriptor
{
	sched_sync_source source;
	union
	{
		sched_task target; //NOTE(martin): if source == task
		//...
	};

	sched_sync_type sync;
	union
	{
		f64 scaling;
		sched_curve_descriptor* tempo;
	};

} sched_timescale_descriptor;

typedef enum { SCHED_STATE_UNSYNC,
               SCHED_STATE_TRACKING,
	       SCHED_STATE_SYNC } sched_sync_state;

//----------------------------------------------------------------------------------
// Fibers and tasks structures
//----------------------------------------------------------------------------------

const u64 SCHED_FIBER_STACK_SIZE = 1<<20;

typedef struct sched_task_info sched_task_info;

typedef enum { SCHED_STATUS_ACTIVE,
               SCHED_STATUS_SUSPENDED,
	       SCHED_STATUS_BACKGROUND,
               SCHED_STATUS_IDLE,
	       SCHED_STATUS_COMPLETED } sched_object_status;

typedef struct sched_fiber_info
{
	list_info waiting;
	list_info waitingElt;
	sched_object_signal waitingFor;
	sched_wakeup_code wakeupCode;

	sched_object_status status;
	i64 exitCode;

	list_info listElt;
	sched_task_info* task;
	i32 openHandles;

	f64 logicalLoc;
	u64 ticket;

	list_info jobQueueElt;

	fiber_context* context;
	sched_fiber_proc proc;
	void* userPointer;

} sched_fiber_info;

typedef struct sched_task_info
{
	list_info listElt;

	sched_task_info* parent;
	list_info parentElt;
	list_info children;

	i32 openHandles;

	sched_object_status status;
	i64 exitCode;
	list_info waiting;

	//sync / scale description
	sched_timescale_descriptor descriptor;
	sched_curve* tempoCurve;

	//runtime sync values
	f64 srcOffset;  //NOTE: offset of the start of the timescale, in the time source reference
	f64 selfLoc;    //NOTE: the current location in the timescale's units
	f64 srcLoc;     //NOTE: the current location in the time source's units
	f64 logicalLoc; //NOTE: location of the current event or = selfLoc

	sched_fiber_info* mainFiber;

	//NOTE: scheduled fibers
	list_info fibers;
	list_info suspended;

} sched_task_info;

//----------------------------------------------------------------------------------
// Handles structures
//----------------------------------------------------------------------------------
typedef enum { SCHED_HANDLE_INVALID = 0,
               SCHED_HANDLE_FREE,
               SCHED_HANDLE_FIBER,
	       SCHED_HANDLE_TASK } sched_handle_slot_kind;

typedef struct sched_handle_slot
{
	u32 generation;
	sched_handle_slot_kind kind;
	union
	{
		list_info freeListElt;
		sched_fiber_info* fiber;
		sched_task_info* task;
	};
} sched_handle_slot;

//----------------------------------------------------------------------------------
// Background Jobs queue
//----------------------------------------------------------------------------------

const u32 SCHED_BACKGROUND_QUEUE_THREAD_COUNT = 8;

typedef struct sched_job_queue
{
	_Atomic(bool) running;
	platform_mutex* mutex;
	platform_condition* condition;
	list_info list;

	platform_thread* threads[SCHED_BACKGROUND_QUEUE_THREAD_COUNT];

} sched_job_queue;

//----------------------------------------------------------------------------------
// Scheduler actions
//----------------------------------------------------------------------------------

const u32 SCHED_ACTION_INFO_SBO_SIZE = 128;

typedef struct sched_action_info
{
	list_info listElt;
	f64 delay;
	sched_action_callback callback;
	void* userPointer;
	bool allocatedData;
	char sbo[SCHED_ACTION_INFO_SBO_SIZE];

} sched_action_info;

//----------------------------------------------------------------------------------
// Scheduler structure
//----------------------------------------------------------------------------------
const f64 SCHEDULER_FUSION_THRESHOLD = 100e-9;

typedef enum { SCHED_MESSAGE_FOREGROUND,
               SCHED_MESSAGE_WAKEUP } sched_message_kind;

typedef struct sched_message
{
	list_info listElt;
	sched_message_kind kind;

	union
	{
		sched_fiber_info* fiber; //SCHED_MESSAGE_FOREGROUND
		sched_fiber fiberHandle; //SCHED_MESSAGE_WAKEUP

		//...
	};

} sched_message;

const u32 SCHED_MAX_HANDLE_SLOTS = 1024;

typedef struct sched_info
{
	mem_pool messagePool;
	mem_pool fiberPool;
	mem_pool taskPool;
	mem_pool stackPool;
	mem_pool actionPool;

	sched_handle_slot handleSlots[SCHED_MAX_HANDLE_SLOTS];
	u32 nextHandleSlot;
	list_info handleFreeList;

	platform_condition* msgCondition;
	platform_mutex* msgConditionMutex;
	ticket_spin_mutex msgQueueMutex;
	ticket_spin_mutex msgPoolMutex;
	_Atomic(bool) hasMessages;
	list_info messages;

	sched_job_queue jobQueue;

	list_info actions;
	list_info runningTasks;
	list_info suspendedTasks;
	sched_fiber_info* currentFiber;

	f64 lastTimeUpdate;
	f64 timeToSleepResidue;
	f64 startTime;
	u64 nextTicket;

	f64 lookAhead;
	f64 lookAheadWindow;

} sched_info;

//NOTE(martin): scheduler context
sched_info __schedInfo__;

sched_info* sched_get_context()
{
	return(&__schedInfo__);
}

//NOTE(martin): fiber entry point wrapper
i64 sched_fiber_start(fiber_context* context)
{
	sched_fiber_info* fiber = (sched_fiber_info*)context->user;
	return(fiber->proc(fiber->userPointer));
}

//-------------------------------------------------------------------------------------------------------
// Handle system
//-------------------------------------------------------------------------------------------------------

sched_handle_slot* sched_alloc_handle_slot(sched_info* sched)
{
	sched_handle_slot* slot = 0;
	if(ListEmpty(&sched->handleFreeList))
	{
		if(sched->nextHandleSlot >= SCHED_MAX_HANDLE_SLOTS)
		{
			LOG_ERROR("Too many in-flight handles (SCHED_MAX_HANDLE_SLOTS = %u)\n", SCHED_MAX_HANDLE_SLOTS);
			return(0);
		}

		slot = &(sched->handleSlots[sched->nextHandleSlot]);
		sched->nextHandleSlot++;
		slot->kind = SCHED_HANDLE_FREE;
		slot->generation = 1;
	}
	else
	{
		slot = ListEntry(ListPop(&sched->handleFreeList), sched_handle_slot, freeListElt);
	}
	DEBUG_ASSERT(slot->kind == SCHED_HANDLE_FREE);
	return(slot);
}

//TODO(martin): see if we really want to use that and not a wrapper of sched_handle_slot_find_generic,
//              and if we really want the asserts in here
sched_handle_slot* sched_handle_slot_find_kind(sched_info* sched, u64 h, sched_handle_slot_kind kind)
{
	u32 generation = (u32)(h & 0xffffffff);
	u32 index = (u32)(h>>32);

	DEBUG_ASSERT(index < sched->nextHandleSlot);

	sched_handle_slot*  slot = &(sched->handleSlots[index]);
	DEBUG_ASSERT(slot->generation == generation);
	DEBUG_ASSERT(slot->kind == kind);
	return(slot);
}

sched_handle_slot_kind sched_handle_slot_find_generic(sched_info* sched, u64 h, sched_handle_slot** outSlot)
{
	u32 generation = (u32)(h & 0xffffffff);
	u32 index = (u32)(h>>32);

	if(index >= sched->nextHandleSlot)
	{
		return(SCHED_HANDLE_INVALID);
	}

	sched_handle_slot*  slot = &(sched->handleSlots[index]);
	if(slot->generation != generation)
	{
		return(SCHED_HANDLE_INVALID);
	}
	*outSlot = slot;
	return(slot->kind);
}

void sched_handle_slot_recycle(sched_info* sched, sched_handle_slot* slot)
{
	slot->kind = SCHED_HANDLE_FREE;
	slot->generation++;
	ListPush(&sched->handleFreeList, &slot->freeListElt);
}

u64 sched_handle_slot_get_packed_handle(sched_info* sched, sched_handle_slot* slot)
{
	u64 generation = (u64)slot->generation;
	u64 index = (u64)(slot - sched->handleSlots);
	u64 h = index<<32 | generation;
	return(h);
}

sched_task sched_alloc_task_handle(sched_info* sched, sched_task_info* task)
{
	sched_handle_slot* slot = sched_alloc_handle_slot(sched);
	if(!slot)
	{
		return((sched_task){.h = 0});
	}
	slot->kind = SCHED_HANDLE_TASK;
	slot->task = task;
	return((sched_task){.h = sched_handle_slot_get_packed_handle(sched, slot)});
}

sched_task_info* sched_handle_get_task_ptr(sched_info* sched, sched_task handle)
{
	sched_handle_slot* slot = sched_handle_slot_find_kind(sched, handle.h, SCHED_HANDLE_TASK);
	return(slot->task);
}

sched_fiber sched_alloc_fiber_handle(sched_info* sched, sched_fiber_info* fiber)
{
	sched_handle_slot* slot = sched_alloc_handle_slot(sched);
	if(!slot)
	{
		return((sched_fiber){.h = 0});
	}
	slot->kind = SCHED_HANDLE_FIBER;
	slot->fiber = fiber;
	return((sched_fiber){.h = sched_handle_slot_get_packed_handle(sched, slot)});
}

sched_fiber_info* sched_handle_get_fiber_ptr(sched_info* sched, sched_fiber handle)
{
	sched_handle_slot* slot = sched_handle_slot_find_kind(sched, handle.h, SCHED_HANDLE_FIBER);
	return(slot->fiber);
}

//-------------------------------------------------------------------------------------------------------
// Clock / Condition wrappers
//-------------------------------------------------------------------------------------------------------

#ifndef SCHED_OFFLINE // real-time scheduler wrappers

f64 sched_clock_get_time()
{
	return(ClockGetTime(SYS_CLOCK_MONOTONIC));
}

void sched_condition_timed_wait(platform_condition* cond, platform_mutex* mutex, f64 timeout)
{
	ConditionTimedWait(cond, mutex, timeout);
}

void sched_condition_wait(platform_condition* cond, platform_mutex* mutex)
{
	ConditionWait(cond, mutex);
}

#else // offline scheduler wrappers

f64 __offlineClock = 0;

f64 sched_clock_get_time()
{
	return(__offlineClock);
}

void sched_condition_timed_wait(platform_condition* cond, platform_mutex* mutex, f64 timeout)
{
	__offlineClock += timeout;
}

void sched_condition_wait(platform_condition* cond, platform_mutex* mutex)
{
	ConditionWait(cond, mutex);
}

#endif // #ifndef SCHED_OFFLINE

//-------------------------------------------------------------------------------------------------------
// Tasks position updates functions
//-------------------------------------------------------------------------------------------------------

f64 sched_task_update_pos_from_sync_scaling(sched_info* sched, sched_task_info* task, sched_steps timeElapsed)
{
	f64 posUpdate = task->descriptor.scaling * timeElapsed;
	task->selfLoc += posUpdate;
	task->srcLoc  += timeElapsed;
	task->logicalLoc = task->selfLoc;
	return(posUpdate);
}

f64 sched_task_update_pos_from_sync_curve(sched_info* sched, sched_task_info* task, sched_steps timeElapsed)
{
	DEBUG_ASSERT(task->tempoCurve);
	DEBUG_ASSERT(task->tempoCurve->eltCount);

	f64 newSrcLoc = task->srcLoc + timeElapsed;
	f64 newSelfLoc = 0;
	sched_curve_get_position_from_time(task->tempoCurve, newSrcLoc, &newSelfLoc);

	//TODO: potential loss of significance, get update directly from curve?
	f64 posUpdate = newSelfLoc - task->selfLoc;
	task->selfLoc = newSelfLoc;
	task->srcLoc = newSrcLoc;
	task->logicalLoc = task->selfLoc;

	return(posUpdate);
}

void sched_task_update_position(sched_info* sched, sched_task_info* task, sched_steps timeElapsed)
{
	//NOTE(martin): timeElapsed is expressed in parent's task. We first convert it
	//              to the local task and update the current location

	#if DEBUG
		f64 oldPos = task->selfLoc;
	#endif

	f64 posUpdate = 0;
	sched_curve* curve = 0;

	switch(task->descriptor.sync)
	{
		case SCHED_SYNC_SCALING:
			posUpdate = sched_task_update_pos_from_sync_scaling(sched, task, timeElapsed);
			break;
		case SCHED_SYNC_CURVE:
			posUpdate = sched_task_update_pos_from_sync_curve(sched, task, timeElapsed);
			break;
	}

	#if DEBUG
		if(oldPos == task->selfLoc && timeElapsed != 0)
		{
			LOG_WARNING("time update too small to update task's position\n");
		}
	#endif

	//NOTE(martin): now we update all children.
	for_each_in_list(&task->children, child, sched_task_info, parentElt)
	{
		if(child->status != SCHED_STATUS_SUSPENDED)
		{
			sched_task_update_position(sched, child, posUpdate);
		}
	}
}

void sched_update_task_positions(sched_info* sched, sched_steps timeElapsed)
{
	for_each_in_list(&sched->runningTasks, task, sched_task_info, listElt)
	{
		//NOTE(martin): we only call update on root (ie. metronome) tasks here.
		//              Each tasks updates its children after updating itself
		//              This avoid recomputing global to local delay for every task
		if(task->descriptor.source == SCHED_SYNC_CLOCK)
		{
			sched_task_update_position(sched, task, (sched_steps)timeElapsed);
		}
	}
}

//-------------------------------------------------------------------------------------------------------
// Tasks conversion functions
//-------------------------------------------------------------------------------------------------------

f64 sched_local_to_source_delay_from_scaling(sched_info* sched, sched_task_info* task, sched_steps steps)
{
	DEBUG_ASSERT(task->descriptor.scaling != 0);
	return(steps/task->descriptor.scaling);
}

f64 sched_local_to_source_delay_from_curve(sched_info* sched, sched_task_info* task, sched_steps steps)
{
	DEBUG_ASSERT(task->tempoCurve);
	DEBUG_ASSERT(task->tempoCurve->eltCount);
	f64 nextTime = 0;
	f64 nextPos = task->selfLoc + steps;
	sched_curve_get_time_from_position(task->tempoCurve, nextPos, &nextTime);

	//TODO(martin): prevent potential loss of significance, get timeUpdate directly from curve?
	f64 timeUpdate = nextTime - task->srcLoc;
	return(timeUpdate);
}

f64 sched_local_to_global_delay(sched_info* sched, sched_task_info* task, sched_steps steps)
{
	//NOTE(martin): compute steps in parent's units
	switch(task->descriptor.sync)
	{
		case SCHED_SYNC_SCALING:
			steps = sched_local_to_source_delay_from_scaling(sched, task, steps);
			break;

		case SCHED_SYNC_CURVE:
			steps = sched_local_to_source_delay_from_curve(sched, task, steps);
			break;
	}

	//NOTE(martin): now depending on the source, return or climb up the hierarchy
	switch(task->descriptor.source)
	{
		case SCHED_SYNC_CLOCK:
			return(steps);

		case SCHED_SYNC_TASK:
			DEBUG_ASSERT(task->parent);
			return(sched_local_to_global_delay(sched, task->parent, steps));
	}
}

//-------------------------------------------------------------------------------------------------------
// Scheduler messages handlers
//-------------------------------------------------------------------------------------------------------

void sched_fiber_reschedule_in_steps(sched_info* sched, sched_fiber_info* fiber, sched_steps steps);

void sched_do_foreground_cmd(sched_info* sched, sched_fiber_info* fiber)
{
	ListRemove(&fiber->listElt);
	fiber->status = SCHED_STATUS_ACTIVE;
	sched_fiber_reschedule_in_steps(sched, fiber, 0);
}

void sched_do_wakeup_cmd(sched_info* sched, sched_fiber fiberHandle)
{
	sched_fiber_info* fiber = sched_handle_get_fiber_ptr(sched, fiberHandle);
	if(fiber->status == SCHED_STATUS_SUSPENDED)
	{
		sched_fiber_resume(fiberHandle);
	}
}

//-------------------------------------------------------------------------------------------------------
// Scheduler message queue functions
//-------------------------------------------------------------------------------------------------------

void sched_wait_for_message(sched_info* sched)
{
	MutexLock(sched->msgConditionMutex);
	while(!sched->hasMessages)
	{
		//NOTE(martin): wait on the command condition, which releases the command mutex, which
		//              allow other threads to commit command buffers to the command queue
		sched_condition_wait(sched->msgCondition, sched->msgConditionMutex);
	}
	MutexUnlock(sched->msgConditionMutex);
}

void sched_wait_for_message_or_timeout(sched_info* sched, f64 timeout)
{
	//NOTE(martin): Wait on the command condition, which releases the command mutex, which
	//              allow other threads to commit command buffers to the command queue

	MutexLock(sched->msgConditionMutex);
	while(!sched->hasMessages && (timeout > 100e-6))
	{
		//NOTE(martin): the precision of ConditionTimedWait() is somewhat proportional to the timeout.
		//              To get a better precision, we wait multiple times with geometrically decreasing timeouts
		//              until we are close enough to our desired timeout. The ratio of 0.8 gives a sub ms precision
		//              with around 4 loop iteration for a 1s delay.
		//              This also allows to gracefully handle spurious wakeups.
		//TODO(martin): do more precise measurement of timeout accuracy and choose ratio accordingly.

		f64 lastStart = sched_clock_get_time();
		sched_condition_timed_wait(sched->msgCondition, sched->msgConditionMutex, timeout * 0.8);
		timeout -= sched_clock_get_time() - lastStart;
	}
	MutexUnlock(sched->msgConditionMutex);
}

sched_message* sched_next_message(sched_info* sched, sched_message* oldMessage)
{
	if(oldMessage)
	{
		TicketSpinMutexLock(&sched->msgPoolMutex);
		{
			mem_pool_release_block(&sched->messagePool, oldMessage);
		} TicketSpinMutexUnlock(&sched->msgPoolMutex);
	}

	sched_message* message = 0;
	TicketSpinMutexLock(&sched->msgQueueMutex);
	{
		message = ListPopEntry(&sched->messages, sched_message, listElt);
		if(!message)
		{
			sched->hasMessages = false;
		}
	} TicketSpinMutexUnlock(&sched->msgQueueMutex);

	return(message);
}

void sched_dispatch_commands(sched_info* sched)
{
	sched_message* message = 0;

	while((message = sched_next_message(sched, message)) != 0)
	{
		switch(message->kind)
		{
			case SCHED_MESSAGE_FOREGROUND:
				sched_do_foreground_cmd(sched, message->fiber);
				break;
			case SCHED_MESSAGE_WAKEUP:
				sched_do_wakeup_cmd(sched, message->fiberHandle);
				break;
			//...
		}
	}
}

sched_message* sched_message_acquire(sched_info* sched)
{
	TicketSpinMutexLock(&sched->msgPoolMutex);
		sched_message* message = mem_pool_alloc_type(&sched->messagePool, sched_message);
	TicketSpinMutexUnlock(&sched->msgPoolMutex);
	memset(message, 0, sizeof(sched_message));
	return(message);
}

void sched_message_commit(sched_info* sched, sched_message* message)
{
	MutexLock(sched->msgConditionMutex);
	{
		TicketSpinMutexLock(&sched->msgQueueMutex);
		{
			ListAppend(&sched->messages, &message->listElt);
			sched->hasMessages = true;
		} TicketSpinMutexUnlock(&sched->msgQueueMutex);

		ConditionSignal(sched->msgCondition);
	} MutexUnlock(sched->msgConditionMutex);
}

//-------------------------------------------------------------------------------------------------------
// Scheduling
//-------------------------------------------------------------------------------------------------------

void sched_fiber_reschedule_in_steps(sched_info* sched, sched_fiber_info* fiber, sched_steps steps)
{
	sched_task_info* task = fiber->task;
	fiber->logicalLoc = fiber->task->logicalLoc + steps;
	fiber->ticket = sched->nextTicket++;

	//NOTE(martin): insert new event in the event list. The event list is sorted by ascending location.
	bool found = false;
	for_each_in_list(&task->fibers, item, sched_fiber_info, listElt)
	{
		if((item->logicalLoc - fiber->logicalLoc) > 0)
		{
			ListInsertBefore(&item->listElt, &fiber->listElt);
			found = true;
			break;
		}
	}
	if(!found)
	{
		ListAppend(&task->fibers, &fiber->listElt);
	}
	task->status = SCHED_STATUS_ACTIVE;
}

//-------------------------------------------------------------------------------------------------------
// Fiber/tasks retirement/completion
//-------------------------------------------------------------------------------------------------------

void sched_task_recycle(sched_info* sched, sched_task_info* task)
{
	LOG_MESSAGE("recycle task %p\n", task);
	DEBUG_ASSERT(task->tempoCurve == 0, "curves should have been be freed earlier as part of termination");
	mem_pool_release_block(&sched->taskPool, task);
}

void sched_task_check_if_needs_recycling(sched_info* sched, sched_task_info* task)
{
	if(task->status == SCHED_STATUS_COMPLETED
	  && !task->openHandles)
	{
		sched_task_recycle(sched, task);
	}
}

void sched_fiber_recycle(sched_info* sched, sched_fiber_info* fiber)
{
	LOG_MESSAGE("recycle fiber %p\n", fiber);

	DEBUG_ASSERT(ListEmpty(&fiber->waiting));
	DEBUG_ASSERT(fiber->status == SCHED_STATUS_COMPLETED);
	DEBUG_ASSERT(fiber->openHandles <= 0);

	//NOTE(martin): free the fiber stack.
	//WARN(martin): We now that the fiber context pointer aliases the start of the stack.
	//              Maybe provide a special wrapper for that.
	mem_pool_release_block(&sched->stackPool, fiber->context);
}

void sched_fiber_check_if_needs_recycling(sched_info* sched, sched_fiber_info* fiber)
{
	if(fiber->status == SCHED_STATUS_COMPLETED
	  && !fiber->openHandles)
	{
		sched_fiber_recycle(sched, fiber);
	}
}

void sched_task_complete(sched_info* sched, sched_task_info* task);

void sched_task_notify_parent_of_completion(sched_info* sched, sched_task_info* task)
{
	if(task->status == SCHED_STATUS_IDLE)
	{
		//NOTE(martin): check if all children are completed and complete this task if true
		for_each_in_list(&task->children, child, sched_task_info, parentElt)
		{
			if(child->status != SCHED_STATUS_COMPLETED)
			{
				return;
			}
		}
		sched_task_complete(sched, task);
	}
}

void sched_signal_waiting_fibers(sched_info* sched, list_info* waiting, sched_object_signal signal)
{
	for_each_in_list_safe(waiting, fiber, sched_fiber_info, waitingElt)
	{
		if(fiber->waitingFor == signal)
		{
			fiber->status = SCHED_STATUS_ACTIVE;
			fiber->wakeupCode = SCHED_WAKEUP_SIGNALED;
			ListRemove(&fiber->waitingElt);
			ListRemove(&fiber->listElt);
			sched_fiber_reschedule_in_steps(sched, fiber, 0);
		}
	}
}

void sched_signal_waiting_fibers_on_cancel(sched_info* sched, list_info* waiting)
{
	for_each_in_list_safe(waiting, fiber, sched_fiber_info, waitingElt)
	{
		fiber->status = SCHED_STATUS_ACTIVE;
		fiber->wakeupCode = SCHED_WAKEUP_CANCELLED;
		ListRemove(&fiber->waitingElt);
		ListRemove(&fiber->listElt);
		sched_fiber_reschedule_in_steps(sched, fiber, 0);
	}
}

void sched_task_complete(sched_info* sched, sched_task_info* task)
{
	task->status = SCHED_STATUS_COMPLETED;

	//NOTE(martin): remove from active tasks
	ListRemove(&task->listElt);

	//NOTE(martin): notify all fibers waiting on this task's retirement
	sched_signal_waiting_fibers(sched, &task->waiting, SCHED_SIG_COMPLETED);

	//NOTE(martin): notify parent that we are completed
	if(task->parent)
	{
		sched_task_notify_parent_of_completion(sched, task->parent);
	}

	//NOTE(martin): remove from task hierarchy
	ListRemove(&task->parentElt);

	//NOTE(martin): free active resources of the task
	if(task->tempoCurve)
	{
		sched_curve_destroy(task->tempoCurve);
		task->tempoCurve = 0;
	}

	//NOTE(martin): check if the task can be recycled
	sched_task_check_if_needs_recycling(sched, task);
}

void sched_task_retire(sched_info* sched, sched_task_info* task)
{
	task->status = SCHED_STATUS_IDLE;

	//NOTE(martin): notify all fibers waiting on this task's retirement
	sched_signal_waiting_fibers(sched, &task->waiting, SCHED_SIG_IDLE);

	//NOTE(martin): check if all children are completed and complete this task if true
	for_each_in_list(&task->children, child, sched_task_info, parentElt)
	{
		if(child->status != SCHED_STATUS_COMPLETED)
		{
			return;
		}
	}
	sched_task_complete(sched, task);
}

void sched_fiber_complete(sched_info* sched, sched_fiber_info* fiber)
{
	fiber->status = SCHED_STATUS_COMPLETED;

	//NOTE(martin): notify all fibers waiting on this fiber's completion
	sched_signal_waiting_fibers(sched, &fiber->waiting, SCHED_SIG_COMPLETED);

	//NOTE(martin): check if task as any fibers left
	if(ListEmpty(&fiber->task->fibers) && ListEmpty(&fiber->task->suspended))
	{
		//NOTE(martin): if so, set the exit code and retire the task
		fiber->task->exitCode = fiber->exitCode;
		sched_task_retire(sched, fiber->task);
	}

	//NOTE: now we may recycle the fiber if there are no more handles refering to it
	sched_fiber_check_if_needs_recycling(sched, fiber);
}
//-------------------------------------------------------------------------------------------------------
// Scheduler background queue
//-------------------------------------------------------------------------------------------------------

_Thread_local sched_fiber_info* __backgroundJobCurrentFiber = 0;

void* sched_background_job_main(void* userPointer)
{
	LOG_DEBUG("starting worker thread\n");

	sched_info* sched = (sched_info*)userPointer;
	sched_job_queue* queue = &sched->jobQueue;

	while(queue->running)
	{
		sched_fiber_info* fiber = 0;

		//NOTE(martin): wait for a fiber to be available in the queue
		MutexLock(queue->mutex);
		{
			while(ListEmpty(&queue->list) && queue->running)
			{
				ConditionWait(queue->condition, queue->mutex);
			}
			if(!queue->running)
			{
				MutexUnlock(queue->mutex);
				goto end;
			}
			fiber = ListPopEntry(&queue->list, sched_fiber_info, jobQueueElt);
		} MutexUnlock(queue->mutex);

		LOG_DEBUG("picked a background job\n");
		//NOTE(martin): yield to the fiber
		__backgroundJobCurrentFiber = fiber;
		fiber_yield(fiber->context);

		//NOTE(martin): fiber has yielded back, post a message to the scheduler to reschedule it
		sched_message* message = sched_message_acquire(sched);
			message->kind = SCHED_MESSAGE_FOREGROUND;
			message->fiber = fiber;
		sched_message_commit(sched, message);
	}

	end:
	// cleanup

	return(0);
}

void sched_background_queue_init(sched_info* sched)
{
	sched_job_queue* queue = &sched->jobQueue;

	queue->running = true;
	queue->mutex = MutexCreate();
	queue->condition = ConditionCreate();
	ListInit(&queue->list);

	for(int i=0; i<SCHED_BACKGROUND_QUEUE_THREAD_COUNT; i++)
	{
		char name[64];
		snprintf(name, 64, "sched_worker#%i", i);
		queue->threads[i] = ThreadCreateWithName(sched_background_job_main, sched, name);
	}
}

void sched_background_queue_cleanup(sched_job_queue* queue)
{
	/*NOTE(martin):
		We first set queue->running to false and signal all thread. After that, we know that worker threads
		are either soon to quit or blocked in a blocking call. In any case they won't try to take the queue
		lock again, so we can cancel the all.
	*/
	MutexLock(queue->mutex);
		queue->running = false;
		ConditionBroadcast(queue->condition);
	MutexUnlock(queue->mutex);

	for(int i=0; i<SCHED_BACKGROUND_QUEUE_THREAD_COUNT; i++)
	{
		ThreadCancel(queue->threads[i]);
	}
}

void sched_background_queue_push(sched_info* sched, sched_fiber_info* fiber)
{
	sched_job_queue* queue = &sched->jobQueue;

	//NOTE(martin): put the fiber in the queue and wakeup one background job
	MutexLock(queue->mutex);
	{
		ListAppend(&queue->list, &fiber->jobQueueElt);
		ConditionSignal(queue->condition);
	} MutexUnlock(queue->mutex);
}

//-------------------------------------------------------------------------------------------------------
// Actions
//-------------------------------------------------------------------------------------------------------

void sched_action_schedule(sched_info* sched, sched_action_info* action)
{
	//NOTE(martin): insert new action in the action list. The action list is sorted by ascending time.
	//              action delays are relative to the previous action (or to logical time for the first one)

	f64 delayFromNow = sched->lookAhead;
	f64 cumulatedDelay = 0;

	for_each_in_list(&sched->actions, item, sched_action_info, listElt)
	{
		f64 nextCumulatedDelay = cumulatedDelay + item->delay;

		if(nextCumulatedDelay > delayFromNow)
		{
			action->delay = delayFromNow - cumulatedDelay;
			item->delay = nextCumulatedDelay - delayFromNow;
			ListInsertBefore(&item->listElt, &action->listElt);
			return;
		}
		cumulatedDelay = nextCumulatedDelay;
	}

	//NOTE(martin): append to the action queue
	action->delay = delayFromNow - cumulatedDelay;
	ListAppend(&sched->actions, &action->listElt);
}

void sched_action_execute(sched_info* sched, sched_action_info* action)
{
	action->callback(action->userPointer);

	if(action->allocatedData)
	{
		free(action->userPointer);
	}
	mem_pool_release_block(&sched->actionPool, action);
}

//-------------------------------------------------------------------------------------------------------
// Scheduler run-loop
//-------------------------------------------------------------------------------------------------------

typedef enum { SCHED_PICKED_ACTION,
               SCHED_PICKED_FIBER,
	       SCHED_PICKED_MESSAGE } sched_picked_event_kind;

int sched_pick_event(sched_info* sched, sched_fiber_info** outFiber, sched_action_info** outAction)
{
	//NOTE(martin): get the next action and its delay from the action list

	sched_action_info* nextAction = 0;
	f64 actionDelay = DBL_MAX;

	list_info* head = ListBegin(&sched->actions);
	if(head != ListEnd(&sched->actions))
	{
		nextAction = ListEntry(head, sched_action_info, listElt);
		actionDelay = nextAction->delay;
	}

	//NOTE(martin): get the next fiber and its delay: check head of each task's event queue,
	//              translate dates to global task and choose the soonest
	sched_fiber_info* nextFiber = 0;
	f64 fiberDelay = DBL_MAX;
	f64 fiberDelayFromLogicalTime = DBL_MAX;

	for_each_in_list(&sched->runningTasks, task, sched_task_info, listElt)
	{
		list_info* head = ListBegin(&task->fibers);
		if(head != ListEnd(&task->fibers))
		{
			sched_fiber_info* fiber = ListEntry(head, sched_fiber_info, listElt);

			f64 localDelay = fiber->logicalLoc - task->selfLoc;
			f64 delay = sched_local_to_global_delay(sched, task, localDelay);

			LOG_DEBUG("event globalDelay: %f, event logicalLoc: %f, task logicalLoc: %f \n",
				     delay,
				     fiber->logicalLoc,
				     task->selfLoc);

			if(delay < fiberDelay)
			{
				fiberDelay = delay;
				nextFiber = fiber;
			}
			else if( ((delay - fiberDelay) < SCHEDULER_FUSION_THRESHOLD)
			       &&(fiber->ticket < nextFiber->ticket))
			{
				nextFiber = fiber;
			}
		}
	}
	if(nextFiber)
	{
		fiberDelayFromLogicalTime = fiberDelay + sched->lookAhead;
	}

	f64 logicalTimeout = 0;
	bool nextEventIsAction = false;
	if(nextFiber || nextAction)
	{
		//NOTE(martin): compute how much logical time we must sleep
		f64 windowShiftToNextFiber = maximum(0, fiberDelayFromLogicalTime - sched->lookAheadWindow);

		nextEventIsAction = actionDelay < windowShiftToNextFiber;
		logicalTimeout = nextEventIsAction ? actionDelay : windowShiftToNextFiber;

		if(logicalTimeout > 0)
		{
			//NOTE(martin): compute real timeout and sleep
			f64 workingTime = sched_clock_get_time() - sched->lastTimeUpdate;
			f64 realTimeout = logicalTimeout + sched->timeToSleepResidue - workingTime; //TODO: call timeToSleepResidue realTimeoutResidue

			if(realTimeout <= 0)
			{
				sched->timeToSleepResidue = realTimeout;
			}
			else
			{
				sched_wait_for_message_or_timeout(sched, realTimeout);
			}
		}
	}
	else
	{
		//NOTE(martin): or just wait a message
		sched_wait_for_message(sched);
	}

	//NOTE(martin): wakeup from sleep
	if(!sched->hasMessages)
	{
		//NOTE(martin): wakeup after timeout. If we had a timeout, we must have a scheduled fiber or action.
		DEBUG_ASSERT(nextFiber || nextAction);

		f64 now = sched_clock_get_time();
		f64 timeElapsed = now - sched->lastTimeUpdate;
		sched->lastTimeUpdate = now;
		sched->timeToSleepResidue += (logicalTimeout - timeElapsed);

		//NOTE(martin): update delays and look-ahead according to if we are scheduling an action of fiber
		if(nextEventIsAction)
		{
			f64 fiberTimeUpdate = maximum(0, logicalTimeout - sched->lookAhead);
			sched->lookAhead = maximum(0, sched->lookAhead - logicalTimeout);

			//NOTE(martin): update tasks' positions
			sched_update_task_positions(sched, fiberTimeUpdate);

			ListRemove(&nextAction->listElt);
			*outAction = nextAction;
			return(SCHED_PICKED_ACTION);
		}
		else
		{
			if(nextAction)
			{
				nextAction->delay -= logicalTimeout;
			}

			f64 fiberTimeUpdate = taskDelay;
			sched->lookAhead = fiberDelayFromLogicalTime - logicalTimeout;

			//NOTE(martin): update tasks' positions
			sched_update_task_positions(sched, fiberTimeUpdate);

			ListRemove(&nextFiber->listElt);
			*outFiber = nextFiber;
			return(SCHED_PICKED_FIBER);
		}
	}
	else
	{
		//NOTE(martin): wakeup after message.
		f64 now = sched_clock_get_time();
		f64 timeElapsed = now - sched->lastTimeUpdate;
		sched->lastTimeUpdate = now;
		sched->timeToSleepResidue = 0;

		//TODO: collapse with similar snippets above.
		f64 fiberTimeUpdate = maximum(0, timeElapsed - sched->lookAhead);
		sched->lookAhead = maximum(0, sched->lookAhead - timeElapsed);

		//NOTE(martin): update tasks' positions
		sched_update_task_positions(sched, fiberTimeUpdate);

		//NOTE(martin): update next action time
		if(nextAction)
		{
			nextAction->delay -= timeElapsed;
		}

		return(SCHED_PICKED_MESSAGE);
	}
}

i64 sched_run(void* userPointer)
{
	sched_info* sched = sched_get_context();

	sched->lastTimeUpdate = sched_clock_get_time();
	sched->startTime = sched->lastTimeUpdate;

	while(true)
	{
		sched_fiber_info* fiber = 0;
		sched_action_info* action = 0;

		switch(sched_pick_event(sched, &fiber, &action))
		{
			case SCHED_PICKED_ACTION:
			{
				sched->currentFiber = 0;

				//TODO: correctly set logical loc / real time loc

				//NOTE(martin): execute the action
				sched_action_execute(sched, action);
			} break;

			case SCHED_PICKED_FIBER:
			{
				//NOTE(martin): we picked a fiber, execute it
				//NOTE(martin): if the fiber was suspended and is awaken after its timeout, clear its status flag,
				//              set its wakeup code and remove it from the wait list its in.
				if(fiber->status == SCHED_STATUS_SUSPENDED)
				{
					fiber->status = SCHED_STATUS_ACTIVE;
					fiber->wakeupCode = SCHED_WAKEUP_TIMEOUT;
					ListRemove(&fiber->waitingElt);
				}

				//NOTE(martin): set the task loc and yield to fiber
				fiber->task->logicalLoc = fiber->logicalLoc;
				sched->currentFiber = fiber;
				fiber_yield(fiber->context);

				//NOTE(martin): fiber yielded back, check if it's status
				if(!fiber->context->running)
				{
					fiber->exitCode = fiber->context->exitCode;
					sched_fiber_complete(sched, fiber);
				}
				else if(fiber->status == SCHED_STATUS_BACKGROUND)
				{
					//NOTE(martin): fiber requested to be put in the background queue
					sched_background_queue_push(sched, fiber);
				}
			} break;

			case SCHED_PICKED_MESSAGE:
			{
				//NOTE(martin): we have pending commands, dispatch them.
				sched_dispatch_commands(sched);
			} break;
		}
	}
	return(0);
}
//-------------------------------------------------------------------------------------------------------
// Misc. helpers
//-------------------------------------------------------------------------------------------------------

sched_task_info* sched_task_alloc_init(sched_info* sched, sched_task_info* parent)
{
	sched_task_info* task = mem_pool_alloc_type(&sched->taskPool, sched_task_info);
	task->openHandles = 0;
	task->status = SCHED_STATUS_ACTIVE;

	task->srcOffset = 0;
	task->selfLoc = 0;
	task->srcLoc = 0;
	task->logicalLoc = 0;

	if(parent)
	{
		task->descriptor = (sched_timescale_descriptor){.source = SCHED_SYNC_TASK,
								.sync   = SCHED_SYNC_SCALING,
								.scaling  = 1.};
	}
	else
	{
		task->descriptor = (sched_timescale_descriptor){.source = SCHED_SYNC_CLOCK,
								.sync   = SCHED_SYNC_SCALING,
								.scaling  = 1.};
	}
	task->tempoCurve = 0; //sched_curve_create(task->descriptor.tempo);

	//NOTE(martin): init fibers lists
	ListInit(&task->fibers);
	ListInit(&task->suspended);
	ListInit(&task->waiting);

	//NOTE(martin): put the task in the task hierarchy
	task->parent = parent;
	if(parent)
	{
		ListAppend(&parent->children, &task->parentElt);
	}
	else
	{
		ListInit(&task->parentElt);
	}
	ListInit(&task->children);

	return(task);
}

sched_fiber_info* sched_fiber_alloc_init(sched_info* sched, sched_task_info* task, sched_fiber_proc proc, void* userPointer)
{
	sched_fiber_info* fiber = mem_pool_alloc_type(&sched->fiberPool, sched_fiber_info);
	DEBUG_ASSERT(fiber);
	fiber->openHandles = 0;
	fiber->status = SCHED_STATUS_ACTIVE;

	fiber->task = task;
	fiber->proc = proc;
	fiber->userPointer = userPointer;

	ListInit(&fiber->listElt);
	ListInit(&fiber->jobQueueElt);
	ListInit(&fiber->waiting);
	ListInit(&fiber->waitingElt);

	//NOTE: create fiber stack and fiber info
	char* stack = (char*)mem_pool_alloc_block(&sched->stackPool);
	fiber->context = fiber_init(sched_fiber_start, SCHED_FIBER_STACK_SIZE, stack);
	fiber->context->user = fiber;

	return(fiber);
}

sched_fiber_info* sched_fiber_create_with_task_ptr(sched_info* sched, sched_task_info* task, sched_fiber_proc proc, void* userPointer, sched_steps steps)
{
	sched_fiber_info* fiber = sched_fiber_alloc_init(sched, task, proc, userPointer);
	sched_fiber_reschedule_in_steps(sched, fiber, 0);
	return(fiber);
}

//*******************************************************************************************************
// Public API
//*******************************************************************************************************

//------------------------------------------------------------------------------------------------------
//NOTE: tasks
//------------------------------------------------------------------------------------------------------

sched_task sched_task_create_for_parent_ptr(sched_info* sched, sched_task_info* parent, sched_fiber_proc proc, void* userPointer)
{
	sched_task_info* task = sched_task_alloc_init(sched, parent);
	sched_task handle = sched_alloc_task_handle(sched, task);
	task->openHandles = 1;

	ListAppend(&sched->runningTasks, &task->listElt);

	sched_fiber_info* fiber = sched_fiber_create_with_task_ptr(sched, task, proc, userPointer, 0);
	task->mainFiber = fiber;
	return(handle);
}

sched_task sched_task_create(sched_fiber_proc proc, void* userPointer)
{
	sched_info* sched = sched_get_context();
	DEBUG_ASSERT(sched->currentFiber);

	return(sched_task_create_for_parent_ptr(sched, sched->currentFiber->task, proc, userPointer));
}

sched_task sched_task_create_for_parent(sched_task parent, sched_fiber_proc proc, void* userPointer)
{
	sched_info* sched = sched_get_context();
	sched_task_info* parentPtr = sched_handle_get_task_ptr(sched, parent);
	DEBUG_ASSERT(parentPtr);

	return(sched_task_create_for_parent_ptr(sched, parentPtr, proc, userPointer));
}

void sched_task_timescale_set_scaling(sched_task task, f64 scaling)
{
	sched_info* sched = sched_get_context();
	sched_task_info* taskPtr = sched_handle_get_task_ptr(sched, task);

	if(taskPtr->tempoCurve)
	{
		sched_curve_destroy(taskPtr->tempoCurve);
		taskPtr->tempoCurve = 0;
	}
	taskPtr->descriptor.sync = SCHED_SYNC_SCALING;
	taskPtr->descriptor.scaling = scaling;
}
void sched_task_timescale_set_tempo_curve(sched_task task, sched_curve_descriptor* descriptor)
{
	sched_info* sched = sched_get_context();
	sched_task_info* taskPtr = sched_handle_get_task_ptr(sched, task);

	if(taskPtr->tempoCurve)
	{
		sched_curve_destroy(taskPtr->tempoCurve);
		taskPtr->tempoCurve = 0;
	}
	taskPtr->descriptor.sync = SCHED_SYNC_CURVE;
	taskPtr->tempoCurve = sched_curve_create(descriptor);
}

//------------------------------------------------------------------------------------------------------
//NOTE(martin): fibers creation
//------------------------------------------------------------------------------------------------------

sched_fiber sched_fiber_create_for_task(sched_task task, sched_fiber_proc proc, void* userPointer, sched_steps steps)
{
	sched_info* sched = sched_get_context();
	sched_task_info* taskPtr = sched_handle_get_task_ptr(sched, task);
	sched_fiber_info* fiber = sched_fiber_create_with_task_ptr(sched, taskPtr, proc, userPointer, steps);
	sched_fiber handle = sched_alloc_fiber_handle(sched, fiber);
	fiber->openHandles = 1;

	return(handle);
}

sched_fiber sched_fiber_create(sched_fiber_proc proc, void* userPointer, sched_steps steps)
{
	sched_info* sched = sched_get_context();
	DEBUG_ASSERT(sched->currentFiber);
	sched_fiber_info* fiber = sched_fiber_create_with_task_ptr(sched, sched->currentFiber->task, proc, userPointer, steps);
	sched_fiber handle = sched_alloc_fiber_handle(sched, fiber);
	fiber->openHandles = 1;

	return(handle);
}

//------------------------------------------------------------------------------------------------------
//NOTE(martin): scheduling / waits
//------------------------------------------------------------------------------------------------------
void sched_wait(sched_steps steps)
{
	//NOTE(martin): reschedule fiber
	sched_info* sched = sched_get_context();
	sched_fiber_info* fiber = sched->currentFiber;

	sched_fiber_reschedule_in_steps(sched, fiber, steps);

	//NOTE(martin): yield to the scheduler fiber
	fiber_yield(fiber->context);
}

typedef enum { SCHED_WAIT_OK = 0,
               SCHED_WAIT_INVALID_HANDLE,
	       SCHED_WAIT_SIGNALED,
	       SCHED_WAIT_TIMEOUT } sched_wait_code;

sched_wait_code sched_fiber_put_on_object_waitlist_ptr(sched_info* sched,
                                            sched_fiber_info* fiber,
					    sched_object_handle handle,
					    sched_object_signal signal,
					    sched_steps timeout)
{
	//NOTE(martin): get the handle status and waiting list
	sched_handle_slot* slot = 0;
	list_info* waiting = 0;
	sched_object_status status;

	switch(sched_handle_slot_find_generic(sched, handle.h, &slot))
	{
		case SCHED_HANDLE_INVALID:
		case SCHED_HANDLE_FREE:
			return(SCHED_WAIT_INVALID_HANDLE);

		case SCHED_HANDLE_TASK:
		{
			status = slot->task->status;
			waiting = &(slot->task->waiting);
		} break;

		case SCHED_HANDLE_FIBER:
		{
			status = slot->fiber->status;
			waiting = &(slot->fiber->waiting);
		} break;
	}

	//NOTE(martin): return immediately if the handle is already signaled
	if(  (status == SCHED_STATUS_IDLE && signal == SCHED_SIG_IDLE)
	  || ((status == SCHED_STATUS_COMPLETED) && (signal == SCHED_STATUS_IDLE || signal == SCHED_STATUS_COMPLETED )))
	{
		return(SCHED_WAIT_SIGNALED);
	}

	//NOTE(martin): schedule the fiber according to its timeout, mark it as suspended and put it in the waiting list.
	//              if the timeout is < 0, we put the fiber in the suspended list
	if(timeout == 0)
	{
		return(SCHED_WAIT_TIMEOUT);
	}
	else
	{
		ListRemove(&fiber->waitingElt);
		ListRemove(&fiber->listElt);

		if(timeout < 0)
		{
			fiber->logicalLoc = 0;
			ListAppend(&fiber->task->suspended, &fiber->listElt);
		}
		else
		{
			sched_fiber_reschedule_in_steps(sched, fiber, timeout);
		}
	}
	fiber->status = SCHED_STATUS_SUSPENDED;
	fiber->waitingFor = signal;
	ListAppend(waiting, &fiber->waitingElt);
	return(SCHED_WAIT_OK);
}

void sched_fiber_put_on_object_waitlist(sched_fiber fiber, sched_object_handle handle, sched_object_signal signal, sched_steps timeout)
{
	sched_info* sched = sched_get_context();
	sched_fiber_info* fiberPtr = sched_handle_get_fiber_ptr(sched, fiber);
	if(fiberPtr)
	{
		sched_fiber_put_on_object_waitlist_ptr(sched, fiberPtr, handle, signal, timeout);
		//////////////////////////////////////////////////////////////////////////////////////////////////////////
		//TODO(martin): should probably check wait code and if the fiber has NOT been put in the waitlist,
		//              reschedule it immediately... (otherwise the the fiber stays scheduled as it was...)
		//////////////////////////////////////////////////////////////////////////////////////////////////////////
	}
}

sched_wakeup_code sched_wait_for_handle_generic(sched_object_handle handle, sched_object_signal signal, sched_steps timeout)
{
	sched_info* sched = sched_get_context();
	sched_fiber_info* fiber = sched->currentFiber;

	sched_wait_code code = sched_fiber_put_on_object_waitlist_ptr(sched, fiber, handle, signal, timeout);
	switch(code)
	{
		case SCHED_WAIT_OK:
			break;
		case SCHED_WAIT_INVALID_HANDLE:
			return(SCHED_WAKEUP_INVALID_HANDLE);
		case SCHED_WAIT_SIGNALED:
			return(SCHED_WAKEUP_SIGNALED);
		case SCHED_WAIT_TIMEOUT:
			return(SCHED_WAKEUP_TIMEOUT);
	}

	//NOTE(martin): yield to the scheduler, then return wakeup code
	fiber_yield(fiber->context);
	return(fiber->wakeupCode);
}

void sched_fiber_suspend_ptr(sched_info* sched, sched_fiber_info* fiber)
{
	ListRemove(&fiber->listElt);

	fiber->status = SCHED_STATUS_SUSPENDED;
	fiber->logicalLoc = 0;
	ListAppend(&fiber->task->suspended, &fiber->listElt);

	if(sched->currentFiber == fiber)
	{
		fiber_yield(fiber->context);
	}
}

void sched_fiber_suspend(sched_fiber fiber)
{
	sched_info* sched = sched_get_context();
	sched_fiber_info* fiberPtr = sched_handle_get_fiber_ptr(sched, fiber);
	ASSERT(fiberPtr);
	sched_fiber_suspend_ptr(sched, fiberPtr);
}

void sched_suspend()
{
	sched_info* sched = sched_get_context();
	sched_fiber_suspend_ptr(sched, sched->currentFiber);
}

void sched_fiber_resume(sched_fiber fiber)
{
	sched_info* sched = sched_get_context();
	sched_fiber_info* fiberPtr = sched_handle_get_fiber_ptr(sched, fiber);
	ASSERT(fiberPtr);

	fiberPtr->status = SCHED_STATUS_ACTIVE;
	ListRemove(&fiberPtr->listElt);
	sched_fiber_reschedule_in_steps(sched, fiberPtr, 0);
}

void sched_fiber_cancel_ptr(sched_info* sched, sched_fiber_info* fiber)
{
	//NOTE(martin): remove the fiber from waiting lists and scheduling lists,
	//              then complete the fiber
	ListRemove(&fiber->waitingElt);
	ListRemove(&fiber->listElt);

	//NOTE(martin): notify waiting fibers of cancellation
	sched_signal_waiting_fibers_on_cancel(sched, &fiber->waiting);

	sched_fiber_complete(sched, fiber);
}

void sched_fiber_cancel(sched_fiber fiber)
{
	sched_info* sched = sched_get_context();
	sched_fiber_info* fiberPtr = sched_handle_get_fiber_ptr(sched, fiber);
	ASSERT(fiberPtr);

	sched_fiber_cancel_ptr(sched, fiberPtr);
}

sched_task sched_task_self()
{
	sched_info* sched = sched_get_context();
	return(sched_alloc_task_handle(sched, sched->currentFiber->task));
}

void sched_task_suspend(sched_task task)
{
	sched_info* sched = sched_get_context();
	sched_task_info* taskPtr = sched_handle_get_task_ptr(sched, task);
	ASSERT(taskPtr);

	//NOTE(martin): mark the task as suspended and remove it from the running tasks list.
	//              This way the task won't be considered in sched_pick_event(), nor will it
	//              be updated by sched_task_update_position().
	ListRemove(&taskPtr->listElt);
	taskPtr->status = SCHED_STATUS_SUSPENDED;
	ListAppend(&sched->suspendedTasks, &taskPtr->listElt);
}

void sched_task_resume(sched_task task)
{
	sched_info* sched = sched_get_context();
	sched_task_info* taskPtr = sched_handle_get_task_ptr(sched, task);
	ASSERT(taskPtr);

	ListRemove(&taskPtr->listElt);
	taskPtr->status = SCHED_STATUS_ACTIVE;
	ListAppend(&sched->runningTasks, &taskPtr->listElt);
}

void sched_task_cancel_ptr(sched_info* sched, sched_task_info* task)
{
	//NOTE(martin): cancel all subtasks
	for_each_in_list_safe(&task->children, child, sched_task_info, parentElt)
	{
		sched_task_cancel_ptr(sched, child);
	}

	//NOTE(martin): notify waiting fibers of cancellation
	sched_signal_waiting_fibers_on_cancel(sched, &task->waiting);

	//NOTE(martin): cancel all fibers of the task. This will retire the task,
	//              and complete it since it has no more children.
	for_each_in_list_safe(&task->fibers, fiber, sched_fiber_info, listElt)
	{
		sched_fiber_cancel_ptr(sched, fiber);
	}
}

void sched_task_cancel(sched_task task)
{
	sched_info* sched = sched_get_context();
	sched_task_info* taskPtr = sched_handle_get_task_ptr(sched, task);
	ASSERT(taskPtr);

	sched_task_cancel_ptr(sched, taskPtr);
}

//------------------------------------------------------------------------------------------------------
//NOTE(martin): misc handles functions
//------------------------------------------------------------------------------------------------------
int sched_handle_get_exit_code_generic(sched_object_handle handle, i64* exitCode)
{
	sched_info* sched = sched_get_context();

	//TODO(martin): return error if still active...
	sched_handle_slot* slot = 0;
	switch(sched_handle_slot_find_generic(sched, handle.h, &slot))
	{
		case SCHED_HANDLE_INVALID:
		case SCHED_HANDLE_FREE:
			return(-1);

		case SCHED_HANDLE_TASK:
		{
			*exitCode = slot->task->exitCode;
		} break;

		case SCHED_HANDLE_FIBER:
		{
			*exitCode = slot->fiber->exitCode;
		} break;
	}
	return(0);
}

void sched_handle_release_generic(sched_object_handle handle)
{
	sched_info* sched = sched_get_context();
	sched_handle_slot* slot = 0;
	switch(sched_handle_slot_find_generic(sched, handle.h, &slot))
	{
		case SCHED_HANDLE_INVALID:
		case SCHED_HANDLE_FREE:
			return;

		case SCHED_HANDLE_TASK:
		{
			sched_task_info* task = slot->task;
			task->openHandles--;
			sched_task_check_if_needs_recycling(sched, task);
		} break;

		case SCHED_HANDLE_FIBER:
		{
			sched_fiber_info* fiber = slot->fiber;
			fiber->openHandles--;
			sched_fiber_check_if_needs_recycling(sched, fiber);
		} break;
	}
	//NOTE(martin): recycle the handle slot
	sched_handle_slot_recycle(sched, slot);
}

/*
sched_object_handle sched_handle_duplicate_generic(sched_object_handle handle)
{
	//TODO
}
*/

//------------------------------------------------------------------------------------------------------
//NOTE(martin): background jobs control
//------------------------------------------------------------------------------------------------------

void sched_background()
{
	//TODO(martin): ensure this function can't be called from background!

	sched_info* sched = sched_get_context();
	sched_fiber_info* fiber = sched->currentFiber;

	DEBUG_ASSERT(ListEmpty(&fiber->listElt), "fiber should have been removed from its task's list by sched_pick_event()");
	DEBUG_ASSERT(ListEmpty(&fiber->waitingElt), "fiber is executing, so it should have been removed from any waiting list");

	//NOTE(martin): set the fiber status to background, put it at the end of the task's fibers list, and yield.
	//              The scheduler thread will notice the background status and put the fiber on the background jobs queue.
	fiber->logicalLoc = 0;
	ListAppend(&fiber->task->suspended, &fiber->listElt);
	fiber->status = SCHED_STATUS_BACKGROUND;
	fiber_yield(fiber->context);
}

void sched_foreground()
{
	//NOTE(martin): yield back to the background job entry point. This will put the fiber back in the scheduler's queue
	DEBUG_ASSERT(__backgroundJobCurrentFiber);
	fiber_yield(__backgroundJobCurrentFiber->context);
}

//------------------------------------------------------------------------------------------------------
//NOTE(martin): actions functions
//------------------------------------------------------------------------------------------------------

void sched_action(sched_action_callback callback, u32 size, char* data)
{
	sched_info* sched = sched_get_context();
	sched_action_info* action = mem_pool_alloc_type(&sched->actionPool, sched_action_info);

	action->callback = callback;

	if(size <= SCHED_ACTION_INFO_SBO_SIZE)
	{
		action->userPointer = &action->sbo;
		action->allocatedData = false;
	}
	else
	{
		action->userPointer = (void*)malloc(size);
		action->allocatedData = true;
	}
	memcpy(action->userPointer, data, size);

	sched_action_schedule(sched, action);
}

void sched_action_no_copy(sched_action_callback callback, void* userPointer)
{
	sched_info* sched = sched_get_context();
	sched_action_info* action = mem_pool_alloc_type(&sched->actionPool, sched_action_info);

	action->callback = callback;
	action->userPointer = userPointer;
	action->allocatedData = false;

	sched_action_schedule(sched, action);
}

//------------------------------------------------------------------------------------------------------
//NOTE(martin): sched init / end functions
//------------------------------------------------------------------------------------------------------
void sched_init()
{
	sched_info* sched = sched_get_context();

	//NOTE(martin): init memory pools
	mem_pool_init(&sched->messagePool, sizeof(sched_message));
	mem_pool_init(&sched->fiberPool, sizeof(sched_fiber_info));
	mem_pool_init(&sched->taskPool, sizeof(sched_task_info));
	mem_pool_init(&sched->stackPool, SCHED_FIBER_STACK_SIZE);
	mem_pool_init(&sched->actionPool, sizeof(sched_action_info));

	//NOTE(martin): init handle map
	sched->nextHandleSlot = 0;
	ListInit(&sched->handleFreeList);

	//NOTE(martin): init command locks
	sched->msgCondition = ConditionCreate();
	sched->msgConditionMutex = MutexCreate();
	TicketSpinMutexInit(&sched->msgQueueMutex);
	TicketSpinMutexInit(&sched->msgPoolMutex);
	sched->hasMessages = false;
	ListInit(&sched->messages);

	//NOTE(martin): init scheduling variables
	sched->lastTimeUpdate = 0;
	sched->timeToSleepResidue = 0;
	sched->startTime = 0;
	sched->nextTicket = 0;

	sched->lookAhead = 0;
	sched->lookAheadWindow = 10e-3; // set default lookAheadWindow to 10ms.

	ListInit(&sched->actions);
	ListInit(&sched->runningTasks);
	ListInit(&sched->suspendedTasks);
	sched->currentFiber = 0;

	//NOTE(martin): init job queue
	sched_background_queue_init(sched);

	//NOTE(martin): create the main task, and the main fiber, without scheduling it. Upon return it contains the entry point to the
	//              scheduler fiber, but after the first yield, from the point of view of the scheduler, the context
	//              will contain the entry point to the main fiber.

	sched_task_info* task = sched_task_alloc_init(sched, 0);
	sched_fiber_info* fiber = sched_fiber_alloc_init(sched, task, sched_run, 0);
	task->mainFiber = fiber;

	//NOTE(martin): put the task in the running list, and set the current fiber, so that we're in the same state as if we just
	//              returned from the scheduler's fiber.
	ListAppend(&sched->runningTasks, &task->listElt);
	sched->currentFiber = fiber;

	//NOTE(martin): now wait for 0 steps, to jumpstart the sched_run() fiber.
	sched_wait(0);
}

void sched_end()
{
	sched_info* sched = sched_get_context();

	//NOTE(martin): clean background queue, join threads
	sched_background_queue_cleanup(&sched->jobQueue);

	sched_task_info* task = 0;
	//NOTE(martin): cancel all tasks
	//TODO(martin): should really be able to free all pools and be done with it ?...
	while((task = ListPopEntry(&sched->runningTasks, sched_task_info, listElt)) != 0)
	{
		sched_task_cancel_ptr(sched, task);
	}
	while((task = ListPopEntry(&sched->suspendedTasks, sched_task_info, listElt)) != 0)
	{
		sched_task_cancel_ptr(sched, task);
	}

	//NOTE(martin): release action memory
	for_each_in_list(&sched->actions, action, sched_action_info, listElt)
	{
		if(action->allocatedData)
		{
			free(action->userPointer);
		}
	}

	//NOTE(martin): release memory from all pools
	mem_pool_release(&sched->actionPool);
	mem_pool_release(&sched->fiberPool);
	mem_pool_release(&sched->taskPool);
	mem_pool_release(&sched->stackPool);

	//NOTE(martin): destroy command locks
	ConditionDestroy(sched->msgCondition);
	MutexDestroy(sched->msgConditionMutex);

	//NOTE(martin): clear context
	memset(sched, 0, sizeof(sched_info));
}


//////////////////////////// WIP ////////////////////////////////////

void sched_command_fiber_wakeup(sched_fiber fiber)
{
	sched_info* sched = sched_get_context();

	sched_message* message = sched_message_acquire(sched);
	{
		message->kind = SCHED_MESSAGE_WAKEUP;
		message->fiberHandle = fiber;
	} sched_message_commit(sched, message);
}



#undef LOG_SUBSYSTEM
