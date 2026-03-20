/* EDF (Earliest Deadline First) Scheduler + RT Task Queue.
 *
 * Soft real-time scheduler for tasks with deadlines.
 * Tasks: Render, Audio, USB-Isoch, Network.
 * Each task has a period and a deadline.
 * The task with the earliest absolute deadline runs first.
 */
#ifndef EDF_H
#define EDF_H

#include <stdint.h>

/* Task priority levels (lower = more urgent) */
#define RT_PRIO_AUDIO    0  /* 5ms deadline (200Hz) */
#define RT_PRIO_USB      1  /* 8ms deadline (125Hz) */
#define RT_PRIO_RENDER   2  /* 16ms deadline (60Hz) */
#define RT_PRIO_NETWORK  3  /* 100ms deadline */
#define RT_PRIO_MAX      4

/* RT Task */
typedef struct {
    int active;
    uint64_t period_ms;     /* task period */
    uint64_t deadline;      /* absolute deadline (timer_ms) */
    uint64_t last_run;      /* last execution time */
    void (*func)(void *ctx);/* task function */
    void *ctx;              /* task context */
    const char *name;
} rt_task_t;

/* Initialize EDF scheduler */
void edf_init(void);

/* Register an RT task. Returns task ID or -1. */
int edf_register(int priority, uint64_t period_ms,
                 void (*func)(void *), void *ctx, const char *name);

/* Run the highest-priority ready task (called from main loop). */
int edf_run_next(void);

/* Get scheduler stats */
int edf_task_count(void);

#endif
