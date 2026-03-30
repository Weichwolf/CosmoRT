/* CosmoRT Prioritised Polling
 *
 * Single-threaded on Core 0. No locks.
 * Static registration: fixed array, no malloc.
 *
 * After each handler that did work, re-scan from highest priority.
 * This guarantees audio/input preempt network bursts.
 * Bounded: each handler is max_work-limited per invocation,
 * and total restarts are capped to prevent livelock.
 */

#include "core/rt_poll.h"
#include "core/smp.h"

/* ── Static handler table ────────────────────────── */

typedef struct {
    rt_poll_fn fn;
    int        max_work;
} rt_poll_slot_t;

static rt_poll_slot_t slots[RT_PRIO_COUNT];

void rt_poll_register(enum rt_prio prio, rt_poll_fn fn, int max_work) {
    if ((unsigned)prio < RT_PRIO_COUNT) {
        slots[prio].fn       = fn;
        slots[prio].max_work = max_work;
    }
}

/* ── Stub handlers for subsystems not yet implemented ── */

static int audio_poll_stub(int max_work) { (void)max_work; return 0; }
static int input_poll_stub(int max_work) { (void)max_work; return 0; }
static int vsync_poll_stub(int max_work) { (void)max_work; return 0; }

void rt_poll_init(void) {
    rt_poll_register(RT_PRIO_AUDIO, audio_poll_stub, 1);
    rt_poll_register(RT_PRIO_INPUT, input_poll_stub, 8);
    rt_poll_register(RT_PRIO_VSYNC, vsync_poll_stub, 1);
}

/* ── Main polling loop ───────────────────────────── */

#define MAX_RESTARTS 4  /* prevent livelock from continuous high-prio work */

void rt_poll_run(void) {
    if (smp_core_id() != 0) return;

    int restarts = 0;

    for (int p = 0; p < RT_PRIO_COUNT; p++) {
        if (!slots[p].fn) continue;

        int done = slots[p].fn(slots[p].max_work);

        /* If lower-prio handler did work, re-check higher priorities */
        if (done > 0 && p > 0 && restarts < MAX_RESTARTS) {
            restarts++;
            p = -1; /* becomes 0 after p++ */
        }
    }
}

