#ifndef TICK_H
#define TICK_H

#include <stdint.h>

#define TICK_EVERY          0
#define TICK_NS_PER_MS      1000000ull

struct tick_callback {
    void                  (*fn)(void);
    uint64_t                interval_ns;
    uint64_t                next_deadline_ns;
    struct tick_callback   *next;
};

void tick_register(struct tick_callback *cb, void (*fn)(void), uint64_t interval_ns);
void tick_run(uint64_t now_ns);

#endif
