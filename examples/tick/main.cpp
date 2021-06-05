#include<stdio.h>
#include"scheduler.h"

void print_action_callback(void* userPointer)
{
	int* count = (int*)userPointer;
	printf("count = %i\n", *count);
}

i64 my_task_proc(void* userPointer)
{
	for(int i=0; i<20; i++)
	{
		sched_action(print_action_callback, sizeof(int), (char*)&i);
		sched_wait(1);
	}
	return(0);
}

i64 my_input_fiber(void* userPointer)
{
	sched_background();
		while(getchar() != 'q') ;
	sched_foreground();
	sched_task_cancel(*(sched_task*)userPointer);
	return(0);
}

int main()
{
	const int eltCount = 2;
	sched_curve_descriptor_elt elements[eltCount] = {
		{.type = SCHED_CURVE_BEZIER, .startValue = 2, .endValue = 8, .length = 10, .p1x = 0.5, .p1y = 0, .p2x = 0.5, .p2y = 1},
		{.type = SCHED_CURVE_BEZIER, .startValue = 8, .endValue = 2, .length = 10, .p1x = 0.5, .p1y = 0, .p2x = 0.5, .p2y = 1}};
	sched_curve_descriptor desc = {.axes = SCHED_CURVE_POS_TEMPO, .eltCount = eltCount, .elements = elements};

	sched_init();
	sched_task task = sched_task_create(my_task_proc, 0);
	sched_task_timescale_set_tempo_curve(task, &desc);

	sched_fiber_create(my_input_fiber, &task, 0);

	sched_wait_completion(task);
	sched_handle_release(task);
	sched_end();

	return(0);
}
