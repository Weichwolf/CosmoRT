/* Timer-Tick-Callback-Registry — Linux-Modell fuer periodische IRQ-Arbeit.
 *
 * Subsysteme registrieren statische tick_callback-Instanzen in ihren init()-
 * Pfaden. timer_handler ruft tick_run(now_ns), die die Liste abarbeitet.
 *
 * Callbacks laufen im Timer-IRQ-Kontext und duerfen weder schedule() noch
 * Sleep-Primitive nutzen. Reine Polling-/Expiry-Arbeit. */

#include "core/tick.h"

static struct tick_callback *tick_head;

void tick_register(struct tick_callback *cb, void (*fn)(void), uint64_t interval_ns) {
    cb->fn = fn;
    cb->interval_ns = interval_ns;
    cb->next_deadline_ns = 0;
    cb->next = tick_head;
    tick_head = cb;
}

__attribute__((hot))
void tick_run(uint64_t now_ns) {
    for (struct tick_callback *cb = tick_head; cb; cb = cb->next) {
        if (cb->interval_ns == TICK_EVERY) {
            cb->fn();
            continue;
        }
        if (now_ns >= cb->next_deadline_ns) {
            cb->next_deadline_ns = now_ns + cb->interval_ns;
            cb->fn();
        }
    }
}
