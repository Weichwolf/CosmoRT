/* CosmoRT RT-Core Prioritised Polling — single-threaded, no locks.
 *
 * RT-Core iterates priority levels 0 (highest) to RT_PRIO_COUNT-1.
 * Each handler processes up to max_work items per invocation.
 * After each handler, higher priorities are re-checked (preemption).
 */
#ifndef RT_POLL_H
#define RT_POLL_H

/* Priority levels for RT-Core work items */
enum rt_prio {
    RT_PRIO_AUDIO   = 0,  /* <5ms deadline */
    RT_PRIO_INPUT   = 1,  /* <1ms deadline (HID events) */
    RT_PRIO_NET_RX  = 2,  /* Network receive */
    RT_PRIO_NET_TX  = 3,  /* Network transmit (TX ring drain) */
    RT_PRIO_VSYNC   = 4,  /* Display VSync, DMA completion */
    RT_PRIO_TIMER   = 5,  /* Timer wheel */
    RT_PRIO_COUNT
};

/* Handler signature: returns number of work items processed.
 * max_work limits work per invocation (0 = unlimited). */
typedef int (*rt_poll_fn)(int max_work);

/* Register a handler at a given priority. Static slot per prio. */
void rt_poll_register(enum rt_prio prio, rt_poll_fn fn, int max_work);

/* Initialize rt_poll: registers stub handlers for unimplemented subsystems. */
void rt_poll_init(void);

/* Run one polling cycle. Called from timer IRQ on RT-Core. */
void rt_poll_run(void);

#endif
