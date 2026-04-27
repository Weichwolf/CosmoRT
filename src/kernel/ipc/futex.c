/* CosmoRT Futex — Fast Userspace Mutex with Priority Inheritance
 *
 * Block via per-bucket wait_queue_head_t + DEFINE_WAIT-style stack-allocated
 * futex_waiter_t. Wake iterates bucket->wq.head under wq.lock, filters by
 * (uaddr, pid) and calls try_to_wake_up directly. No slab pool — waiter
 * struct lives on the kernel stack frame of the blocked thread.
 *
 * Timeout: hrtimer fires sched_wake on the waiting thread; the wait loop
 * re-evaluates hrtimer_now_ns() and returns -ETIMEDOUT.
 *
 * PI (Priority Inheritance): high-prio waiter on a PI-futex boosts the
 * current owner's priority. PI touches the bucket-protected (by hash slot
 * lock) priority field of the owner_t.
 */

#include "ipc/futex.h"
#include "sys/syscall.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/waitqueue.h"
#include "core/hrtimer.h"
#include "mm/gup.h"
#include "spinlock.h"
#include "hw/serial.h"
#include "config.h"
#include "arch/arch.h"
#include "uaccess.h"

extern void sched_add(thread_t *t);
extern void sched_wake(thread_t *t);

/* ── Bucket layout ─────────────────────────────── */

#define FUTEX_HASH_SIZE  64

/* futex_waiter_t.entry MUST be first member: futex_wake casts
 * wait_queue_entry_t* back to futex_waiter_t* via container-of-by-offset-0.
 * Keep `entry` at offset 0 to make this trivially safe. */
typedef struct futex_waiter {
    wait_queue_entry_t entry;
    uint64_t           uaddr;   /* virtual address (private) or PA (shared) */
    uint32_t           pid;     /* owning pid (private) or 0 (shared) */
} futex_waiter_t;

static wait_queue_head_t futex_bucket[FUTEX_HASH_SIZE];

void futex_init(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        init_waitqueue_head(&futex_bucket[i]);
    serial_puts("futex: init (");
    char buf[4]; int bi = 0, v = FUTEX_HASH_SIZE;
    do { buf[bi++] = '0' + v % 10; v /= 10; } while (v);
    while (bi--) serial_putchar(buf[bi]);
    serial_puts(" buckets, stack-allocated waiters)\n");
}

static int hash_uaddr(uint64_t uaddr, uint32_t pid) {
    uint64_t h = uaddr ^ ((uint64_t)pid * 2654435761ULL);
    return (int)(h % FUTEX_HASH_SIZE);
}

/* Translate user-VA to physical-page (FUTEX_SHARED key normalization). */
#define PTE_PRESENT_FLAG 0x1
#define PTE_PS_FLAG      0x80
#define PTE_ADDR_BITS    0x000FFFFFFFFFF000ULL
static uint64_t futex_va_to_pa(uint64_t va) {
    process_t *p = proc_current();
    if (!p || !p->pml4) return 0;
    uint64_t *pml4 = p->pml4;
    int i4 = (va >> 39) & 0x1FF;
    if (!(pml4[i4] & PTE_PRESENT_FLAG)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(pml4[i4] & PTE_ADDR_BITS);
    int i3 = (va >> 30) & 0x1FF;
    if (!(pdpt[i3] & PTE_PRESENT_FLAG)) return 0;
    if (pdpt[i3] & PTE_PS_FLAG)
        return (pdpt[i3] & PTE_ADDR_BITS) | (va & 0x3FFFFFFF);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[i3] & PTE_ADDR_BITS);
    int i2 = (va >> 21) & 0x1FF;
    if (!(pd[i2] & PTE_PRESENT_FLAG)) return 0;
    if (pd[i2] & PTE_PS_FLAG)
        return (pd[i2] & PTE_ADDR_BITS) | (va & 0x1FFFFF);
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[i2] & PTE_ADDR_BITS);
    int i1 = (va >> 12) & 0x1FF;
    if (!(pt[i1] & PTE_PRESENT_FLAG)) return 0;
    return (pt[i1] & PTE_ADDR_BITS) | (va & 0xFFF);
}

/* Resolve key from raw uaddr. shared=1 → PA-key (cross-process via PA, pid=0).
 * shared=0 → private (vaddr, pid). For shared, FUTEX_WAKE never reads *uaddr
 * (only WAIT does), so a child that only WAKEs on a MAP_SHARED page parent
 * already faulted has nothing to demand-fault the page in. Linux uses
 * get_user_pages here; we use mm_gup_one as the slow-path fallback. */
static void futex_key(uint32_t *uaddr, int shared,
                      uint64_t *out_addr, uint32_t *out_pid) {
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    if (shared) {
        uint64_t pa = futex_va_to_pa(addr);
        if (!pa && p)
            pa = mm_gup_one(p, addr, 0);
        if (pa) { addr = pa; pid = 0; }
    }
    *out_addr = addr;
    *out_pid = pid;
}

/* Iteration helpers operate on bucket->head directly under bucket->lock.
 * The wait_queue_entry_t list is circular doubly-linked; head==NULL means
 * empty. We do NOT call wake_up_nr because futex needs key-filtered wake:
 * the bucket holds waiters for many addresses sharing the same hash. */

/* Caller holds bucket->lock. Removes `e` from the list, sets next/prev=0. */
static void futex_wq_remove(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    if (!e->next) return; /* already off */
    if (e->next == e) {
        wq->head = 0;
    } else {
        e->prev->next = e->next;
        e->next->prev = e->prev;
        if (wq->head == e) wq->head = e->next;
    }
    e->next = 0;
    e->prev = 0;
}

/* ── PI helpers ────────────────────────────────── */

/* Boost owner's priority to at least `prio`. Caller holds bucket->lock so
 * priority modifications serialize against concurrent PI ops on the same
 * bucket. */
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

/* ── FUTEX_WAIT ─────────────────────────────────── */

static volatile int futex_trace = 0;

/* hrtimer fires from IRQ; sched_wake transitions BLOCKED -> RUNNABLE.
 * The wait loop re-evaluates the deadline after schedule() returns. */
static void futex_timeout_fn(hrtimer_t *t) {
    thread_t *th = (thread_t *)t->data;
    if (th) sched_wake(th);
}

static long futex_wait(uint32_t *uaddr, uint32_t val, int timeout_ms, int shared) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    uint64_t addr; uint32_t pid;
    futex_key(uaddr, shared, &addr, &pid);

    if (futex_trace) {
        serial_puts("FW t"); serial_hex64(t->tid);
        serial_puts(" a"); serial_hex64(addr);
        serial_puts(" v"); serial_hex64(val);
        serial_puts(" b"); serial_hex64(hash_uaddr(addr, pid));
        serial_putchar('\n');
    }

    /* Atomic check before queuing: spec-mandated (Linux returns -EAGAIN here). */
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val)
        return -EAGAIN;

    int bucket = hash_uaddr(addr, pid);
    wait_queue_head_t *wq = &futex_bucket[bucket];

    futex_waiter_t fw;
    fw.entry.task  = t;
    fw.entry.flags = 0;
    fw.entry.func  = autoremove_wake_function;
    fw.entry.next  = 0;
    fw.entry.prev  = 0;
    fw.uaddr = addr;
    fw.pid   = pid;

    hrtimer_t timer;
    int has_timer = 0;
    uint64_t deadline_ns = 0;
    if (timeout_ms > 0) {
        deadline_ns = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
        hrtimer_init(&timer, futex_timeout_fn, t);
        has_timer = 1;
    } else if (timeout_ms == 0) {
        /* timeout_ms == 0 means "non-blocking": only Linux's poll-style
         * semantics for FUTEX_WAIT_BITSET. Standard FUTEX_WAIT with a
         * zero timespec returns ETIMEDOUT immediately. */
        return -ETIMEDOUT;
    }

    long rc = 0;
    for (;;) {
        prepare_to_wait(wq, &fw.entry, THREAD_BLOCKED);

        /* Re-check the value under wq->lock-style serialization: any waker
         * that already changed *uaddr races with us correctly because the
         * waker's wake_up takes wq->lock, observes our entry, and transitions
         * our state only after we are queued. */
        if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
            rc = -EAGAIN; break;
        }
        if (signal_deliverable()) {
            rc = -EINTR; break;
        }
        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            rc = -ETIMEDOUT; break;
        }

        if (has_timer) hrtimer_start(&timer, deadline_ns);
        schedule();

        /* Directed wake from FUTEX_WAKE/REQUEUE: try_to_wake_up sets
         * WQ_FLAG_AUTOREMOVE on our entry and removes it from the bucket.
         * Linux's futex_wait_queue_me classifies this as plain success
         * (return 0) without re-checking *uaddr — the waker is responsible
         * for whatever userspace state changes the WAITer cares about. */
        if (fw.entry.flags & WQ_FLAG_AUTOREMOVE) {
            rc = 0; break;
        }
        if (has_timer && hrtimer_now_ns() >= deadline_ns) {
            rc = -ETIMEDOUT; break;
        }
        if (signal_deliverable()) {
            rc = -EINTR; break;
        }
        /* Spurious wake (cross-subsystem) — re-evaluate, sleep again. */
    }
    finish_wait(wq, &fw.entry);
    if (has_timer) hrtimer_cancel(&timer);
    return rc;
}

/* ── FUTEX_WAKE ─────────────────────────────────── */

static long futex_wake(uint32_t *uaddr, uint32_t max_wake, int shared) {
    uint64_t addr; uint32_t pid;
    futex_key(uaddr, shared, &addr, &pid);

    int bucket = hash_uaddr(addr, pid);
    wait_queue_head_t *wq = &futex_bucket[bucket];
    long woken = 0;

    if (futex_trace) {
        serial_puts("FK t"); serial_hex64(thread_current()->tid);
        serial_puts(" a"); serial_hex64((uint64_t)(uintptr_t)uaddr);
        serial_puts(" n"); serial_hex64(max_wake);
        serial_puts(" b"); serial_hex64(bucket);
        serial_putchar('\n');
    }

    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);

    wait_queue_entry_t *cur = wq->head;
    while (cur && (uint32_t)woken < max_wake) {
        wait_queue_entry_t *next = cur->next;
        int last = (next == wq->head);

        futex_waiter_t *w = (futex_waiter_t *)cur; /* entry is first member */
        if (w->uaddr == addr && w->pid == pid) {
            futex_wq_remove(wq, cur);
            if (try_to_wake_up(cur->task, TASK_NORMAL))
                cur->flags |= WQ_FLAG_AUTOREMOVE;
            woken++;
        }

        if (last) break;
        cur = next;
    }

    spin_unlock_irq(&wq->lock, flags);
    if (futex_trace) {
        serial_puts("FK="); serial_hex64(woken); serial_putchar('\n');
    }
    return woken;
}

/* ── FUTEX_LOCK_PI ──────────────────────────────── */

static long futex_lock_pi(uint32_t *uaddr, int shared) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    /* Fast path: uncontended — CAS 0 → our TID */
    uint32_t old = __sync_val_compare_and_swap(uaddr, 0, tid);
    if (old == 0)
        return 0;

    /* Deadlock: we already own it (PTHREAD_MUTEX_ERRORCHECK) */
    if ((old & FUTEX_TID_MASK) == tid)
        return -EDEADLK;

    /* OWNER_DIED on robust PI mutex: previous owner died holding the lock.
     * Linux: acquire with our TID + preserve OWNER_DIED, return 0; userspace
     * (musl trylock_owner) detects via old & 0x40000000 and returns EOWNERDEAD. */
    if ((old & FUTEX_OWNER_DIED) && (old & FUTEX_TID_MASK) == 0) {
        uint32_t nval = tid | FUTEX_OWNER_DIED | (old & FUTEX_WAITERS);
        if (__sync_val_compare_and_swap(uaddr, old, nval) == old)
            return 0;
        /* CAS lost: someone else changed value, fall through to retry path. */
        old = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
        if ((old & FUTEX_TID_MASK) == 0) {
            /* No owner, retry direct acquire. */
            uint32_t nv = tid | (old & (FUTEX_OWNER_DIED | FUTEX_WAITERS));
            if (__sync_val_compare_and_swap(uaddr, old, nv) == old)
                return 0;
        }
    }

    /* Mark contended */
    __sync_fetch_and_or(uaddr, FUTEX_WAITERS);

    uint64_t addr; uint32_t pid;
    futex_key(uaddr, shared, &addr, &pid);
    int bucket = hash_uaddr(addr, pid);
    wait_queue_head_t *wq = &futex_bucket[bucket];

    futex_waiter_t fw;
    fw.entry.task  = self;
    fw.entry.flags = 0;
    fw.entry.func  = autoremove_wake_function;
    fw.entry.next  = 0;
    fw.entry.prev  = 0;
    fw.uaddr = addr;
    fw.pid   = pid;

    /* Boost owner under bucket->lock so it can't be freed between find
     * and boost — bucket->lock serializes against pi_unboost in unlock_pi. */
    uint64_t flags;
    spin_lock_irq(&wq->lock, &flags);

    uint32_t owner_tid = old & FUTEX_TID_MASK;
    thread_t *owner = thread_find_by_tid((int)owner_tid);
    if (owner)
        pi_boost(owner, self->priority);

    /* Re-check before blocking — lock may have been released */
    old = __sync_val_compare_and_swap(uaddr, 0, tid);
    if (old == 0) {
        spin_unlock_irq(&wq->lock, flags);
        return 0;
    }
    old = __sync_val_compare_and_swap(uaddr, FUTEX_WAITERS, tid | FUTEX_WAITERS);
    if (old == FUTEX_WAITERS) {
        spin_unlock_irq(&wq->lock, flags);
        return 0;
    }

    spin_unlock_irq(&wq->lock, flags);

    /* Block via wait_event-style loop (no timeout for LOCK_PI) */
    long rc = 0;
    for (;;) {
        prepare_to_wait(wq, &fw.entry, THREAD_BLOCKED);

        /* Try acquire — owner may have released */
        old = __sync_val_compare_and_swap(uaddr, 0, tid);
        if (old == 0) { rc = 0; break; }
        old = __sync_val_compare_and_swap(uaddr, FUTEX_WAITERS, tid | FUTEX_WAITERS);
        if (old == FUTEX_WAITERS) { rc = 0; break; }

        /* OWNER_DIED arrived (robust PI): acquire preserving the bit. */
        old = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
        if ((old & FUTEX_OWNER_DIED) && (old & FUTEX_TID_MASK) == 0) {
            uint32_t nval = tid | FUTEX_OWNER_DIED | (old & FUTEX_WAITERS);
            if (__sync_val_compare_and_swap(uaddr, old, nval) == old) {
                rc = 0; break;
            }
        }

        if (signal_deliverable()) {
            rc = -EINTR; break;
        }
        schedule();
    }
    finish_wait(wq, &fw.entry);
    return rc;
}

/* ── FUTEX_UNLOCK_PI ────────────────────────────── */

static long futex_unlock_pi(uint32_t *uaddr, int shared) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
    if ((cur & FUTEX_TID_MASK) != tid)
        return -EPERM;

    pi_unboost(self);

    if (cur & FUTEX_WAITERS) {
        __sync_val_compare_and_swap(uaddr, cur, FUTEX_WAITERS);
        futex_wake(uaddr, 1, shared);
    } else {
        __sync_val_compare_and_swap(uaddr, cur, 0);
    }
    return 0;
}

/* ── Timespec → milliseconds ────────────────────── */

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

/* ── FUTEX_REQUEUE / FUTEX_CMP_REQUEUE ─────────── */

static long futex_requeue(uint32_t *uaddr1, uint32_t wake_max,
                          uint32_t requeue_max, uint32_t *uaddr2,
                          int cmp, uint32_t cmpval) {
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr1 = (uint64_t)(uintptr_t)uaddr1;
    uint64_t addr2 = (uint64_t)(uintptr_t)uaddr2;

    int b1 = hash_uaddr(addr1, pid);
    int b2 = hash_uaddr(addr2, pid);
    wait_queue_head_t *wq1 = &futex_bucket[b1];
    wait_queue_head_t *wq2 = &futex_bucket[b2];

    /* Lock both buckets — ordered by index to prevent deadlock */
    uint64_t flags;
    if (b1 == b2) {
        spin_lock_irq(&wq1->lock, &flags);
    } else if (b1 < b2) {
        spin_lock_irq(&wq1->lock, &flags);
        spin_lock(&wq2->lock);
    } else {
        spin_lock_irq(&wq2->lock, &flags);
        spin_lock(&wq1->lock);
    }

    if (cmp) {
        if (__atomic_load_n(uaddr1, __ATOMIC_ACQUIRE) != cmpval) {
            if (b1 == b2) {
                spin_unlock_irq(&wq1->lock, flags);
            } else if (b1 < b2) {
                spin_unlock(&wq2->lock);
                spin_unlock_irq(&wq1->lock, flags);
            } else {
                spin_unlock(&wq1->lock);
                spin_unlock_irq(&wq2->lock, flags);
            }
            return -EAGAIN;
        }
    }

    long woken = 0, requeued = 0;

    /* Phase 1: wake up to wake_max waiters on uaddr1 */
    wait_queue_entry_t *cur = wq1->head;
    while (cur && (uint32_t)woken < wake_max) {
        wait_queue_entry_t *next = cur->next;
        int last = (next == wq1->head);

        futex_waiter_t *w = (futex_waiter_t *)cur;
        if (w->uaddr == addr1 && w->pid == pid) {
            futex_wq_remove(wq1, cur);
            if (try_to_wake_up(cur->task, TASK_NORMAL))
                cur->flags |= WQ_FLAG_AUTOREMOVE;
            woken++;
        }

        if (last) break;
        cur = next;
    }

    /* Phase 2: requeue up to requeue_max remaining waiters from b1 to b2.
     * The waiter struct lives on the blocked thread's stack — we cannot
     * relocate it. Instead, re-thread the entry into wq2->head in place
     * and update fw->uaddr to addr2. The remote thread is still parked
     * inside its prepare_to_wait/schedule loop and will re-evaluate when
     * woken via wq2's wake path. */
    cur = wq1->head;
    while (cur && (uint32_t)requeued < requeue_max) {
        wait_queue_entry_t *next = cur->next;
        int last = (next == wq1->head);

        futex_waiter_t *w = (futex_waiter_t *)cur;
        if (w->uaddr == addr1 && w->pid == pid) {
            /* Detach from wq1 */
            futex_wq_remove(wq1, cur);
            /* Update key */
            w->uaddr = addr2;
            /* Insert at tail of wq2 — keeps relative order */
            cur->next = 0;
            cur->prev = 0;
            if (!wq2->head) {
                wq2->head = cur;
                cur->next = cur;
                cur->prev = cur;
            } else {
                wait_queue_entry_t *tail = wq2->head->prev;
                cur->prev = tail;
                cur->next = wq2->head;
                tail->next = cur;
                wq2->head->prev = cur;
            }
            requeued++;
        }

        if (last) break;
        cur = next;
    }

    if (b1 == b2) {
        spin_unlock_irq(&wq1->lock, flags);
    } else if (b1 < b2) {
        spin_unlock(&wq2->lock);
        spin_unlock_irq(&wq1->lock, flags);
    } else {
        spin_unlock(&wq1->lock);
        spin_unlock_irq(&wq2->lock, flags);
    }

    return woken + requeued;
}

/* ── Dispatcher ─────────────────────────────────── */

long do_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | 0x100);
    int shared = !(op & FUTEX_PRIVATE_FLAG);

    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, val, timespec_to_ms(timeout), shared);
    case FUTEX_WAKE:
        return futex_wake(uaddr, val, shared);
    case FUTEX_REQUEUE:
        return futex_requeue(uaddr, val, (uint32_t)(uintptr_t)timeout,
                             uaddr2, 0, 0);
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(uaddr, val, (uint32_t)(uintptr_t)timeout,
                             uaddr2, 1, val3);
    case FUTEX_LOCK_PI:
        return futex_lock_pi(uaddr, shared);
    case FUTEX_UNLOCK_PI:
        return futex_unlock_pi(uaddr, shared);
    case FUTEX_WAKE_OP: {
        long r1 = futex_wake(uaddr, val, shared);
        if (uaddr2) {
            long r2 = futex_wake(uaddr2, val3, shared);
            if (r2 > 0 && r1 >= 0) r1 += r2;
        }
        return r1;
    }
    case 9: /* FUTEX_WAIT_BITSET */
        return futex_wait(uaddr, val, timespec_to_ms(timeout), shared);
    case 10: /* FUTEX_WAKE_BITSET */
        return futex_wake(uaddr, val, shared);
    default:
        serial_puts("futex: unknown op ");
        serial_hex64((uint64_t)(unsigned)cmd);
        serial_putchar('\n');
        return -ENOSYS;
    }
}
