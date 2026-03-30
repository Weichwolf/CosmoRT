/* CosmoRT RT-Core Prioritised Polling — single-threaded, no locks. */
#ifndef RT_POLL_H
#define RT_POLL_H

enum rt_prio {
    RT_PRIO_AUDIO   = 0,
    RT_PRIO_INPUT   = 1,
    RT_PRIO_NET_RX  = 2,
    RT_PRIO_NET_TX  = 3,
    RT_PRIO_VSYNC   = 4,
    RT_PRIO_TIMER   = 5,
    RT_PRIO_HASH    = 6,
    RT_PRIO_AGE     = 7,
    RT_PRIO_DEDUP   = 8,
    RT_PRIO_COUNT
};

typedef int (*rt_poll_fn)(int max_work);

void rt_poll_register(enum rt_prio prio, rt_poll_fn fn, int max_work);

void rt_poll_init(void);

void rt_poll_run(void);

#endif
