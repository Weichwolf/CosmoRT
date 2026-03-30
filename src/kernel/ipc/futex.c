/* CosmoRT Futex — Fast Userspace Mutex with Priority Inheritance */

#include "ipc/futex.h"
#include "sys/syscall.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/mutex.h"
#include "mm/slab.h"
#include "hw/serial.h"
#include "config.h"
#include "arch/arch.h"
#include "core/event_queue.h"
#include "uaccess.h"

extern void sched_add(thread_t *t);

#define FUTEX_HASH_SIZE  64
#define FUTEX_WAITER_MAX 256

typedef struct futex_waiter {
    thread_t             *thread;
    uint64_t              uaddr;
    uint32_t              pid;
    struct futex_waiter  *next;
} futex_waiter_t;

static futex_waiter_t waiter_pool[FUTEX_WAITER_MAX];
static slab_t waiter_slab;

static struct {
    futex_waiter_t *head;
    mutex_t         lock;
} futex_hash[FUTEX_HASH_SIZE];

void futex_init(void) {
    slab_init(&waiter_slab, waiter_pool,
              (int)sizeof(futex_waiter_t), FUTEX_WAITER_MAX);
    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        futex_hash[i].head = 0;
        futex_hash[i].lock = (mutex_t)MUTEX_INIT;
    }
    serial_puts("futex: init (");
    char buf[4]; int bi = 0, v = FUTEX_HASH_SIZE;
    do { buf[bi++] = '0' + v % 10; v /= 10; } while (v);
    while (bi--) serial_putchar(buf[bi]);
    serial_puts(" buckets, ");
    bi = 0; v = FUTEX_WAITER_MAX;
    do { buf[bi++] = '0' + v % 10; v /= 10; } while (v);
    while (bi--) serial_putchar(buf[bi]);
    serial_puts(" waiters)\n");
}

static int hash_uaddr(uint64_t uaddr, uint32_t pid) {
    uint64_t h = uaddr ^ ((uint64_t)pid * 2654435761ULL);
    return (int)(h % FUTEX_HASH_SIZE);
}

static int futex_remove_waiter(uint64_t addr, uint32_t pid, thread_t *t) {
    int bucket = hash_uaddr(addr, pid);
    mutex_lock(&futex_hash[bucket].lock);

    futex_waiter_t **pp = &futex_hash[bucket].head;
    while (*pp) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == addr && w->pid == pid && w->thread == t) {
            *pp = w->next;
            slab_free(&waiter_slab, w);
            mutex_unlock(&futex_hash[bucket].lock);
            return 1;
        }
        pp = &w->next;
    }

    mutex_unlock(&futex_hash[bucket].lock);
    return 0;
}

static void pi_boost(thread_t *owner, int prio) {
    if (!owner) return;
    if (owner->priority >= prio) return;

    if (owner->saved_priority < 0)
        owner->saved_priority = owner->priority;

    __sync_synchronize();
    owner->priority = prio;
    __sync_synchronize();
}

static void pi_unboost(thread_t *owner) {
    if (!owner) return;
    if (owner->saved_priority < 0) return;

    owner->priority = owner->saved_priority;
    owner->saved_priority = -1;
}

static uint32_t futex_drain_events(event_queue_t *eq) {
    event_t ev;
    uint32_t result = 0;

    while (eq_pop(eq, &ev) == 0) {
        if (ev.type == EQ_FUTEX_WAKE) {
            result = EQ_FUTEX_WAKE;
        } else if (ev.type == EQ_TIMEOUT && result != EQ_FUTEX_WAKE) {
            result = EQ_TIMEOUT;
        }
    }
    return result;
}

extern uint64_t pml4[];

static volatile int futex_trace = 0;

static long futex_wait_pid(uint32_t *uaddr, uint32_t val, int timeout_ms, uint32_t pid) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    if (futex_trace) {
        serial_puts("FW t"); serial_hex64(t->tid);
        serial_puts(" a"); serial_hex64(addr);
        serial_puts(" v"); serial_hex64(val);
        serial_puts(" b"); serial_hex64(hash_uaddr(addr, pid));
        serial_putchar('\n');
    }

    uint32_t wake_reason = futex_drain_events(&t->eq);

    if (wake_reason == EQ_TIMEOUT) {
        futex_remove_waiter(addr, pid, t);
        return -ETIMEDOUT;
    }

    if (wake_reason == EQ_FUTEX_WAKE) {
    }

    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val)
        return -EAGAIN;

    int bucket = hash_uaddr(addr, pid);
    mutex_lock(&futex_hash[bucket].lock);

    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
        mutex_unlock(&futex_hash[bucket].lock);
        return -EAGAIN;
    }

    futex_waiter_t *w = (futex_waiter_t *)slab_alloc(&waiter_slab);
    if (!w) {
        mutex_unlock(&futex_hash[bucket].lock);
        return -ENOMEM;
    }
    w->thread = t;
    w->uaddr = addr;
    w->pid = pid;
    w->next = futex_hash[bucket].head;
    futex_hash[bucket].head = w;

    mutex_unlock(&futex_hash[bucket].lock);

    {
        event_t ev;
        int _wr = event_wait(&t->eq, &ev, timeout_ms);
        if (_wr == -4) {
            futex_remove_waiter(addr, pid, t);
            return -EINTR;
        }
    }
    return 0;
}

static long futex_wake_pid(uint32_t *uaddr, uint32_t max_wake, uint32_t pid) {
    int bucket = hash_uaddr((uint64_t)(uintptr_t)uaddr, pid);
    long woken = 0;
    if (futex_trace) {
        serial_puts("FK t"); serial_hex64(thread_current()->tid);
        serial_puts(" a"); serial_hex64((uint64_t)(uintptr_t)uaddr);
        serial_puts(" n"); serial_hex64(max_wake);
        serial_puts(" b"); serial_hex64(bucket);
        serial_putchar('\n');
    }

    mutex_lock(&futex_hash[bucket].lock);

    futex_waiter_t **pp = &futex_hash[bucket].head;
    while (*pp && (uint32_t)woken < max_wake) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == (uint64_t)(uintptr_t)uaddr && w->pid == pid) {
            thread_t *target = w->thread;
            *pp = w->next;
            slab_free(&waiter_slab, w);
            event_post(target, EQ_FUTEX_WAKE, 0);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }

    mutex_unlock(&futex_hash[bucket].lock);
    if (futex_trace) {
        serial_puts("FK="); serial_hex64(woken); serial_putchar('\n');
    }
    return woken;
}

static long futex_wait(uint32_t *uaddr, uint32_t val, int timeout_ms) {
    process_t *p = proc_current();
    return futex_wait_pid(uaddr, val, timeout_ms, p ? p->pid : 0);
}
static long futex_wake(uint32_t *uaddr, uint32_t max_wake) {
    process_t *p = proc_current();
    return futex_wake_pid(uaddr, max_wake, p ? p->pid : 0);
}

static long futex_lock_pi_pid(uint32_t *uaddr, uint32_t pid) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    uint32_t old = __sync_val_compare_and_swap(uaddr, 0, tid);
    if (old == 0)
        return 0;

    if ((old & FUTEX_TID_MASK) == tid)
        return -EDEADLK;

    __sync_fetch_and_or(uaddr, FUTEX_WAITERS);

    futex_drain_events(&self->eq);

    {
        int bucket = hash_uaddr((uint64_t)(uintptr_t)uaddr, pid);
        mutex_lock(&futex_hash[bucket].lock);

        uint32_t owner_tid = old & FUTEX_TID_MASK;
        thread_t *owner = thread_find_by_tid((int)owner_tid);
        if (owner)
            pi_boost(owner, self->priority);

        old = __sync_val_compare_and_swap(uaddr, 0, tid);
        if (old == 0) {
            mutex_unlock(&futex_hash[bucket].lock);
            return 0;
        }
        old = __sync_val_compare_and_swap(uaddr, FUTEX_WAITERS, tid | FUTEX_WAITERS);
        if (old == FUTEX_WAITERS) {
            mutex_unlock(&futex_hash[bucket].lock);
            return 0;
        }

        futex_waiter_t *w = (futex_waiter_t *)slab_alloc(&waiter_slab);
        if (!w) {
            mutex_unlock(&futex_hash[bucket].lock);
            return -ENOMEM;
        }
        w->thread = self;
        w->uaddr = (uint64_t)(uintptr_t)uaddr;
        w->pid = pid;
        w->next = futex_hash[bucket].head;
        futex_hash[bucket].head = w;

        mutex_unlock(&futex_hash[bucket].lock);

        event_t ev;
        int _wr = event_wait(&self->eq, &ev, -1);
        if (_wr == -4) {
            mutex_lock(&futex_hash[bucket].lock);
            futex_waiter_t **rpp = &futex_hash[bucket].head;
            while (*rpp) {
                if ((*rpp)->thread == self) {
                    futex_waiter_t *rm = *rpp;
                    *rpp = rm->next;
                    slab_free(&waiter_slab, rm);
                    break;
                }
                rpp = &(*rpp)->next;
            }
            mutex_unlock(&futex_hash[bucket].lock);
            return -EINTR;
        }
    }

    return 0;
}

static long futex_unlock_pi_pid(uint32_t *uaddr, uint32_t pid) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
    if ((cur & FUTEX_TID_MASK) != tid)
        return -EPERM;

    pi_unboost(self);

    if (cur & FUTEX_WAITERS) {
        __sync_val_compare_and_swap(uaddr, cur, FUTEX_WAITERS);
        futex_wake_pid(uaddr, 1, pid);
    } else {
        __sync_val_compare_and_swap(uaddr, cur, 0);
    }

    return 0;
}

static int timespec_to_ms(const void *ts) {
    if (!ts) return -1;

    struct { long tv_sec; long tv_nsec; } kts;
    if (copy_from_user(&kts, ts, sizeof(kts)) != 0)
        return -1;

    if (kts.tv_sec < 0 || kts.tv_nsec < 0) return 0;

    long ms = kts.tv_sec * 1000 + kts.tv_nsec / 1000000;
    if (ms <= 0 && (kts.tv_sec > 0 || kts.tv_nsec > 0))
        ms = 1;
    if (ms > 0x7FFFFFFFL) ms = 0x7FFFFFFFL;
    return (int)ms;
}

static long futex_requeue(uint32_t *uaddr1, uint32_t wake_max,
                          uint32_t requeue_max, uint32_t *uaddr2,
                          int cmp, uint32_t cmpval) {
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr1 = (uint64_t)(uintptr_t)uaddr1;
    uint64_t addr2 = (uint64_t)(uintptr_t)uaddr2;

    int b1 = hash_uaddr(addr1, pid);
    int b2 = hash_uaddr(addr2, pid);

    if (b1 == b2) {
        mutex_lock(&futex_hash[b1].lock);
    } else if (b1 < b2) {
        mutex_lock(&futex_hash[b1].lock);
        mutex_lock(&futex_hash[b2].lock);
    } else {
        mutex_lock(&futex_hash[b2].lock);
        mutex_lock(&futex_hash[b1].lock);
    }

    if (cmp) {
        if (__atomic_load_n(uaddr1, __ATOMIC_ACQUIRE) != cmpval) {
            if (b1 == b2) {
                mutex_unlock(&futex_hash[b1].lock);
            } else if (b1 < b2) {
                mutex_unlock(&futex_hash[b2].lock);
                mutex_unlock(&futex_hash[b1].lock);
            } else {
                mutex_unlock(&futex_hash[b1].lock);
                mutex_unlock(&futex_hash[b2].lock);
            }
            return -EAGAIN;
        }
    }

    long woken = 0, requeued = 0;

    futex_waiter_t **pp = &futex_hash[b1].head;
    while (*pp && (uint32_t)woken < wake_max) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == addr1 && w->pid == pid) {
            thread_t *target = w->thread;
            *pp = w->next;
            slab_free(&waiter_slab, w);
            event_post(target, EQ_FUTEX_WAKE, 0);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }

    pp = &futex_hash[b1].head;
    while (*pp && (uint32_t)requeued < requeue_max) {
        futex_waiter_t *w = *pp;
        if (w->uaddr == addr1 && w->pid == pid) {
            *pp = w->next;
            w->uaddr = addr2;
            w->next = futex_hash[b2].head;
            futex_hash[b2].head = w;
            requeued++;
        } else {
            pp = &(*pp)->next;
        }
    }

    if (b1 == b2) {
        mutex_unlock(&futex_hash[b1].lock);
    } else if (b1 < b2) {
        mutex_unlock(&futex_hash[b2].lock);
        mutex_unlock(&futex_hash[b1].lock);
    } else {
        mutex_unlock(&futex_hash[b1].lock);
        mutex_unlock(&futex_hash[b2].lock);
    }

    return woken + requeued;
}

long do_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | 0x100);
    uint32_t epid = 0;
    if (op & FUTEX_PRIVATE_FLAG) {
        process_t *fp = proc_current();
        epid = fp ? fp->pid : 0;
    }
    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait_pid(uaddr, val, timespec_to_ms(timeout), epid);
    case FUTEX_WAKE:
        return futex_wake_pid(uaddr, val, epid);
    case FUTEX_REQUEUE:
        return futex_requeue(uaddr, val, (uint32_t)(uintptr_t)timeout,
                             uaddr2, 0, 0);
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(uaddr, val, (uint32_t)(uintptr_t)timeout,
                             uaddr2, 1, val3);
    case FUTEX_LOCK_PI:
        return futex_lock_pi_pid(uaddr, epid);
    case FUTEX_UNLOCK_PI:
        return futex_unlock_pi_pid(uaddr, epid);
    case FUTEX_WAKE_OP: {
        long r1 = futex_wake(uaddr, val);
        if (uaddr2) {
            long r2 = futex_wake(uaddr2, val3);
            if (r2 > 0 && r1 >= 0) r1 += r2;
        }
        return r1;
    }
    case 9:
        return futex_wait(uaddr, val, timespec_to_ms(timeout));
    case 10:
        return futex_wake(uaddr, val);
    default:
        serial_puts("futex: unknown op ");
        serial_hex64((uint64_t)(unsigned)cmd);
        serial_putchar('\n');
        return -ENOSYS;
    }
}
