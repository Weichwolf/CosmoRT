/* EDF Scheduler — Earliest Deadline First.
 *
 * Simple cooperative EDF: tasks are functions called from the main loop.
 * The task with the earliest deadline whose period has elapsed runs first.
 * No preemption between RT tasks (cooperative within RT pool).
 */

#include "core/edf.h"
#include "hw/serial.h"
#include "core/timer.h"

#define MAX_TASKS 16

static rt_task_t tasks[MAX_TASKS];
static int num_tasks;

void edf_init(void) {
    for (int i = 0; i < MAX_TASKS; i++)
        tasks[i].active = 0;
    num_tasks = 0;
    serial_puts("EDF: init\n");
}

int edf_register(int priority, uint64_t period_ms,
                 void (*func)(void *), void *ctx, const char *name) {
    if (num_tasks >= MAX_TASKS) return -1;
    int id = num_tasks++;
    tasks[id].active = 1;
    tasks[id].period_ms = period_ms;
    tasks[id].deadline = timer_ms() + period_ms;
    tasks[id].last_run = 0;
    tasks[id].func = func;
    tasks[id].ctx = ctx;
    tasks[id].name = name;
    (void)priority;
    return id;
}

int edf_run_next(void) {
    uint64_t now = timer_ms();
    int best = -1;
    uint64_t earliest_deadline = ~0ULL;

    /* Find the task with the earliest deadline that's ready to run */
    for (int i = 0; i < num_tasks; i++) {
        if (!tasks[i].active) continue;
        /* Task is ready if its period has elapsed since last run */
        if (now - tasks[i].last_run < tasks[i].period_ms) continue;
        if (tasks[i].deadline < earliest_deadline) {
            earliest_deadline = tasks[i].deadline;
            best = i;
        }
    }

    if (best < 0) return 0; /* no task ready */

    /* Run it */
    tasks[best].last_run = now;
    tasks[best].deadline = now + tasks[best].period_ms;
    tasks[best].func(tasks[best].ctx);
    return 1;
}

int edf_task_count(void) { return num_tasks; }
