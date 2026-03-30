/* CosmoRT RT Poll — prioritised single-threaded polling on core 0 */

#include "core/rt_poll.h"
#include "core/smp.h"

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

static int audio_poll_stub(int max_work) { (void)max_work; return 0; }
static int input_poll_stub(int max_work) { (void)max_work; return 0; }
static int vsync_poll_stub(int max_work) { (void)max_work; return 0; }

void rt_poll_init(void) {
    rt_poll_register(RT_PRIO_AUDIO, audio_poll_stub, 1);
    rt_poll_register(RT_PRIO_INPUT, input_poll_stub, 8);
    rt_poll_register(RT_PRIO_VSYNC, vsync_poll_stub, 1);
}

#define MAX_RESTARTS 4

void rt_poll_run(void) {
    if (smp_core_id() != 0) return;

    int restarts = 0;

    for (int p = 0; p < RT_PRIO_COUNT; p++) {
        if (!slots[p].fn) continue;

        int done = slots[p].fn(slots[p].max_work);

        if (done > 0 && p > 0 && restarts < MAX_RESTARTS) {
            restarts++;
            p = -1;
        }
    }
}
