/* CosmoRT Event Queue — per-thread lock-free event delivery */
#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdint.h>

#define EQ_CHILD_EXITED    1
#define EQ_CHILD_STOPPED   2
#define EQ_CHILD_CONTINUED 3
#define EQ_PIPE_DATA       4
#define EQ_PIPE_CLOSED     5
#define EQ_FUTEX_WAKE      6
#define EQ_EPOLL_READY     7
#define EQ_SOCKET_DATA     8
#define EQ_SOCKET_CONNECT  9
#define EQ_TIMEOUT         10
#define EQ_IPC_MSG         11

typedef struct {
    uint32_t type;
    uint64_t data;
} event_t;

#define EQ_MAX_EVENTS 16
#define EQ_MASK       (EQ_MAX_EVENTS - 1)

typedef struct {
    event_t  events[EQ_MAX_EVENTS];
    volatile uint32_t head;
    volatile uint32_t tail;
} event_queue_t;

static inline void event_queue_init(event_queue_t *eq) {
    eq->head = 0;
    eq->tail = 0;
}

static inline int event_pending(event_queue_t *eq) {
    uint32_t h = eq->head;
    __asm__ volatile("" ::: "memory");
    uint32_t t = eq->tail;
    return (int)(h - t);
}

static inline int eq_push(event_queue_t *eq, uint32_t type, uint64_t data) {
    uint32_t h = eq->head;
    uint32_t t = eq->tail;

    if (h - t >= EQ_MAX_EVENTS) {
        eq->tail = t + 1;
    }

    eq->events[h & EQ_MASK].type = type;
    eq->events[h & EQ_MASK].data = data;
    __asm__ volatile("" ::: "memory");
    eq->head = h + 1;
    return 0;
}

static inline int eq_pop(event_queue_t *eq, event_t *out) {
    uint32_t h = eq->head;
    __asm__ volatile("" ::: "memory");
    uint32_t t = eq->tail;

    if (h == t) return -1;

    *out = eq->events[t & EQ_MASK];
    __asm__ volatile("" ::: "memory");
    eq->tail = t + 1;
    return 0;
}

static inline int event_drain(event_queue_t *eq, uint32_t type, event_t *out, int max) {
    uint32_t h = eq->head;
    uint32_t t = eq->tail;
    int count = 0;
    uint32_t new_head = t;

    for (uint32_t i = t; i != h; i++) {
        event_t *e = &eq->events[i & EQ_MASK];
        if (e->type == type && count < max) {
            out[count++] = *e;
        } else {
            if (new_head != i)
                eq->events[new_head & EQ_MASK] = *e;
            new_head++;
        }
    }

    eq->head = new_head;
    return count;
}

struct thread;
void event_post(struct thread *target, uint32_t type, uint64_t data);

int event_wait(event_queue_t *eq, event_t *out, int timeout_ms);

void thread_block_ms(int timeout_ms);

void epoll_sleeper_add_ext(struct thread *t);

void epoll_wake_all(void);

void epoll_check_timeouts(void);

uint64_t epoll_nearest_deadline_tsc(int core_id);

#endif /* EVENT_QUEUE_H */
