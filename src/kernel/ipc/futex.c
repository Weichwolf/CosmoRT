/* CosmoRT Futex — Fast Userspace Mutex with Priority Inheritance
 *
 * Waitqueue-backed blocking (Phase 10.2). Each hash bucket owns a
 * wait_queue_head_t; waiters wrap wait_queue_entry_t + (uaddr, pid) on
 * the caller's stack, so there is no global slab/pool — a rogue process
 * can only exhaust its own kernel stacks.
 *
 * Fast path (uncontended) never enters the kernel. Contended paths take
 * the bucket lock exactly once, publish the waiter under that lock, and
 * sleep via schedule_timeout_interruptible. The waker scans the bucket
 * queue under the same lock and calls wake_up_locked() per match, so
 * state transitions and queue membership stay atomic (Linux
 * prepare_to_wait pattern, no missed wakeups).
 *
 * PI (Priority Inheritance): when a high-priority thread blocks on a PI
 * futex owned by a low-priority thread, the owner is temporarily boosted.
 * This prevents priority inversion per IEEE 1003.1b PTHREAD_PRIO_INHERIT.
 */

#include "ipc/futex.h"
#include "sys/syscall.h"
#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/waitqueue.h"
#include "core/hrtimer.h"
#include "spinlock.h"
#include "hw/serial.h"
#include "config.h"
#include "arch/arch.h"
#include "uaccess.h"

/* Timer callback: wake the thread whose wait_head we attached to.
 * Timer data holds the blocking thread_t. Routing via sched_wake preserves
 * the waitqueue invariant (wake under wait_head->lock). */
static void futex_timer_wake(hrtimer_t *tim) {
    extern void sched_wake(thread_t *t);
    sched_wake((thread_t *)tim->data);
}

/* ── Hash bucket ─────────────────────────────────── */

#define FUTEX_HASH_SIZE  64

typedef struct futex_bucket {
    wait_queue_head_t wq;
} futex_bucket_t;

static futex_bucket_t futex_hash[FUTEX_HASH_SIZE];

/* Waiter = stack-allocated wrapper linking a thread into a bucket's wq
 * with the futex key (uaddr, pid). The embedded wait_queue_entry_t is
 * queued on futex_hash[bucket].wq under the wq lock. */
typedef struct futex_waiter {
    wait_queue_entry_t entry;
    uint64_t           uaddr;
    uint32_t           pid;
    uint32_t           woken;  /* set by futex_wake when this entry is claimed */
} futex_waiter_t;

void futex_init(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        init_waitqueue_head(&futex_hash[i].wq);
    serial_puts("futex: init (");
    char buf[4]; int bi = 0, v = FUTEX_HASH_SIZE;
    do { buf[bi++] = '0' + v % 10; v /= 10; } while (v);
    while (bi--) serial_putchar(buf[bi]);
    serial_puts(" buckets, waitqueue-backed)\n");
}

static int hash_uaddr(uint64_t uaddr, uint32_t pid) {
    uint64_t h = uaddr ^ ((uint64_t)pid * 2654435761ULL);
    return (int)(h % FUTEX_HASH_SIZE);
}

/* Translate user-VA to physical-page. Liefert (pa | page-offset) oder 0
 * wenn nicht gemappt. Fuer FUTEX_SHARED — dann haengt die Wait-Queue an
 * der Kernel-eindeutigen Page-Identitaet statt an (mm, va). */
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
    /* 1GB huge page (PDPT-level PS) — nicht von unserem Mapper benutzt,
     * aber defensiv behandelt. */
    if (pdpt[i3] & PTE_PS_FLAG)
        return (pdpt[i3] & PTE_ADDR_BITS) | (va & 0x3FFFFFFF);
    uint64_t *pd = (uint64_t *)phys_to_virt(pdpt[i3] & PTE_ADDR_BITS);
    int i2 = (va >> 21) & 0x1FF;
    if (!(pd[i2] & PTE_PRESENT_FLAG)) return 0;
    /* 2MB huge page (PD-level PS) — vmm nutzt das fuer Userspace-Stack/Heap. */
    if (pd[i2] & PTE_PS_FLAG)
        return (pd[i2] & PTE_ADDR_BITS) | (va & 0x1FFFFF);
    uint64_t *pt = (uint64_t *)phys_to_virt(pd[i2] & PTE_ADDR_BITS);
    int i1 = (va >> 12) & 0x1FF;
    if (!(pt[i1] & PTE_PRESENT_FLAG)) return 0;
    return (pt[i1] & PTE_ADDR_BITS) | (va & 0xFFF);
}

/* ── PI helpers ────────────────────────────────── */

/* Boost owner's priority to at least `prio`.
 * Must be called with the futex bucket lock held — this serializes
 * priority modifications against concurrent PI operations. */
static void pi_boost(thread_t *owner, int prio) {
    if (!owner) return;
    if (owner->priority >= prio) return;

    if (owner->saved_priority < 0)
        owner->saved_priority = owner->priority;

    __sync_synchronize();
    owner->priority = prio;
    __sync_synchronize();
}

/* Restore owner's original priority after PI unlock. */
static void pi_unboost(thread_t *owner) {
    if (!owner) return;
    if (owner->saved_priority < 0) return;

    owner->priority = owner->saved_priority;
    owner->saved_priority = -1;
}

/* ── FUTEX_WAIT ─────────────────────────────────── */

/* Futex trace: always on for debugging condvar hang */
static volatile int futex_trace = 0;

static long futex_wait(uint32_t *uaddr, uint32_t val, int timeout_ms, int shared) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    process_t *p = t->proc;
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    /* Shared-Futex: Key = physische Adresse (prozess-uebergreifende
     * MAP_SHARED-Pages finden sich). Private-Futex behaelt (vaddr, pid) —
     * ist billiger und fuer pthread/musl-Mutex in single-mm ausreichend. */
    if (shared) {
        uint64_t pa = futex_va_to_pa(addr);
        if (pa) { addr = pa; pid = 0; }
    }
    if (futex_trace) {
        serial_puts("FW t"); serial_hex64(t->tid);
        serial_puts(" a"); serial_hex64(addr);
        serial_puts(" v"); serial_hex64(val);
        serial_puts(" b"); serial_hex64(hash_uaddr(addr, pid));
        serial_putchar('\n');
    }

    int bucket = hash_uaddr(addr, pid);
    futex_bucket_t *b = &futex_hash[bucket];

    /* Fast path: *uaddr != val → wake already happened (or value mismatch). */
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val)
        return -EAGAIN;

    /* Build stack-allocated waiter. Lives until we return. */
    futex_waiter_t w = {
        .entry = { .task = t, .flags = WQ_FLAG_EXCLUSIVE,
                   .next = 0, .prev = 0 },
        .uaddr = addr,
        .pid   = pid,
        .woken = 0,
    };

    /* Timeout setup (hrtimer). deadline_ns == 0 means infinite. */
    uint64_t deadline_ns = 0;
    hrtimer_t timer;
    int has_timer = 0;
    if (timeout_ms > 0) {
        deadline_ns = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
        hrtimer_init(&timer, futex_timer_wake, t);
        has_timer = 1;
    }

    /* Publish waiter in bucket wq, then re-check *uaddr under the wq lock.
     * If wake already happened between the fast-path check and publication,
     * we drop back out with -EAGAIN. The bucket lock serializes this check
     * against concurrent futex_wake on the same key. */
    uint64_t irqf;
    spin_lock_irq(&b->wq.lock, &irqf);
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
        spin_unlock_irq(&b->wq.lock, irqf);
        if (has_timer) hrtimer_cancel(&timer);
        return -EAGAIN;
    }
    /* Insert entry at tail under bucket lock, then bind + set state. */
    if (!b->wq.head) {
        b->wq.head = &w.entry;
        w.entry.next = &w.entry;
        w.entry.prev = &w.entry;
    } else {
        wait_queue_entry_t *tail = b->wq.head->prev;
        w.entry.prev = tail;
        w.entry.next = b->wq.head;
        tail->next = &w.entry;
        b->wq.head->prev = &w.entry;
    }
    t->wait_entry = &w.entry;
    t->wait_head  = &b->wq;
    __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
    spin_unlock_irq(&b->wq.lock, irqf);

    if (has_timer) hrtimer_start(&timer, deadline_ns);

    /* Wait loop. Wake conditions:
     *   (1) futex_wake claimed us (w.woken set), or
     *   (2) signal pending (interruptible), or
     *   (3) timeout elapsed, or
     *   (4) spurious wake (SMP IPI, unrelated event) — loop again.
     * Each iteration re-arms THREAD_BLOCKED under the bucket lock so a
     * waker racing with the re-arm cannot miss us. */
    extern void schedule(void);
    long rc;
    for (;;) {
        /* Test conditions. If none hit, re-sleep. */
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) { rc = 0; break; }
        if (signal_deliverable()) { rc = -EINTR; break; }
        if (has_timer && hrtimer_now_ns() >= deadline_ns) { rc = -ETIMEDOUT; break; }

        /* Arm BLOCKED state under bucket lock. wake_up / futex_wake take
         * the same lock, so we can't miss a wake that fires after the
         * last condition check. */
        spin_lock_irq(&b->wq.lock, &irqf);
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) {
            spin_unlock_irq(&b->wq.lock, irqf);
            rc = 0; break;
        }
        __atomic_store_n(&t->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
        spin_unlock_irq(&b->wq.lock, irqf);

        schedule();
    }

    if (has_timer) hrtimer_cancel(&timer);

    /* Dequeue + clear state under bucket lock. Thread is RUNNING from here. */
    spin_lock_irq(&b->wq.lock, &irqf);
    if (w.entry.next != 0) {
        if (w.entry.next == &w.entry) {
            b->wq.head = 0;
        } else {
            w.entry.prev->next = w.entry.next;
            w.entry.next->prev = w.entry.prev;
            if (b->wq.head == &w.entry) b->wq.head = w.entry.next;
        }
        w.entry.next = w.entry.prev = 0;
    }
    t->wait_entry = 0;
    t->wait_head  = 0;
    __atomic_store_n(&t->state, THREAD_RUNNING, __ATOMIC_RELEASE);
    spin_unlock_irq(&b->wq.lock, irqf);

    return rc;
}

/* ── FUTEX_WAKE ─────────────────────────────────── */

/* Wake up to max_wake waiters whose key matches (addr, pid).
 * Caller holds no locks. We take the bucket lock and iterate the wq
 * list; for each match, flip state BLOCKED→RUNNABLE + enqueue. */
extern void sched_add(thread_t *t);

static long futex_wake(uint32_t *uaddr, uint32_t max_wake, int shared) {
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    if (shared) {
        uint64_t pa = futex_va_to_pa(addr);
        if (pa) { addr = pa; pid = 0; }
    }
    int bucket = hash_uaddr(addr, pid);
    futex_bucket_t *b = &futex_hash[bucket];
    long woken = 0;
    if (futex_trace) {
        serial_puts("FK t"); serial_hex64(thread_current()->tid);
        serial_puts(" a"); serial_hex64((uint64_t)(uintptr_t)uaddr);
        serial_puts(" n"); serial_hex64(max_wake);
        serial_puts(" b"); serial_hex64(bucket);
        serial_putchar('\n');
    }

    uint64_t flags;
    spin_lock_irq(&b->wq.lock, &flags);

    wait_queue_entry_t *start = b->wq.head;
    if (start) {
        wait_queue_entry_t *cur = start;
        for (;;) {
            wait_queue_entry_t *next = cur->next;
            futex_waiter_t *w = (futex_waiter_t *)cur;
            thread_t *task = cur->task;

            if (w->uaddr == addr && w->pid == pid && task) {
                /* Claim: mark woken + flip state under bucket lock. */
                __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
                int old = __sync_val_compare_and_swap(&task->state,
                        THREAD_BLOCKED, THREAD_RUNNABLE);
                if (old == THREAD_BLOCKED) sched_add(task);
                woken++;
                if ((uint32_t)woken >= max_wake) break;
            }
            if (next == start) break;
            cur = next;
        }
    }

    spin_unlock_irq(&b->wq.lock, flags);
    if (futex_trace) {
        serial_puts("FK="); serial_hex64(woken); serial_putchar('\n');
    }
    return woken;
}

/* ── FUTEX_LOCK_PI ──────────────────────────────── */

/* PI-Lock Key-Strategie identisch zu futex_wait/wake: shared → PA/pid=0,
 * private → (vaddr, pid). Sonst kollidiert pshared-ROBUST-Mutex mit
 * Cleanup-Wakes, die shared laufen. */
static long futex_lock_pi(uint32_t *uaddr, int shared) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    /* Fast path: uncontended — CAS 0 → our TID */
    uint32_t old = __sync_val_compare_and_swap(uaddr, 0, tid);
    if (old == 0)
        return 0; /* acquired */

    /* Deadlock: we already own it (PTHREAD_MUTEX_ERRORCHECK) */
    if ((old & FUTEX_TID_MASK) == tid)
        return -EDEADLK;

    process_t *p = self->proc;
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr = (uint64_t)(uintptr_t)uaddr;
    if (shared) {
        uint64_t pa = futex_va_to_pa(addr);
        if (pa) { addr = pa; pid = 0; }
    }
    int bucket = hash_uaddr(addr, pid);
    futex_bucket_t *b = &futex_hash[bucket];

    futex_waiter_t w = {
        .entry = { .task = self, .flags = WQ_FLAG_EXCLUSIVE,
                   .next = 0, .prev = 0 },
        .uaddr = addr,
        .pid   = pid,
        .woken = 0,
    };

    uint64_t flags;
    spin_lock_irq(&b->wq.lock, &flags);

    /* Set FUTEX_WAITERS UNDER the bucket lock together with the waiter
     * insertion. Otherwise there's a window between fetch_and_or and
     * queue insert where unlock_pi fires futex_wake on an empty queue
     * and we miss the wake. */
    __sync_fetch_and_or(uaddr, FUTEX_WAITERS);

    /* Boost owner under lock so it can't be freed between find and boost */
    uint32_t owner_tid = old & FUTEX_TID_MASK;
    thread_t *owner = thread_find_by_tid((int)owner_tid);
    if (owner)
        pi_boost(owner, self->priority);

    /* Re-check before blocking — lock may have been released between
     * the fast-path CAS and now. */
    old = __sync_val_compare_and_swap(uaddr, 0, tid);
    if (old == 0) {
        spin_unlock_irq(&b->wq.lock, flags);
        return 0;
    }
    old = __sync_val_compare_and_swap(uaddr, FUTEX_WAITERS, tid | FUTEX_WAITERS);
    if (old == FUTEX_WAITERS) {
        spin_unlock_irq(&b->wq.lock, flags);
        return 0;
    }

    /* Insert waiter + publish state under bucket lock. */
    if (!b->wq.head) {
        b->wq.head = &w.entry;
        w.entry.next = &w.entry;
        w.entry.prev = &w.entry;
    } else {
        wait_queue_entry_t *tail = b->wq.head->prev;
        w.entry.prev = tail;
        w.entry.next = b->wq.head;
        tail->next = &w.entry;
        b->wq.head->prev = &w.entry;
    }
    self->wait_entry = &w.entry;
    self->wait_head  = &b->wq;
    __atomic_store_n(&self->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
    spin_unlock_irq(&b->wq.lock, flags);

    /* Block until woken or signaled. Same re-arm pattern as futex_wait. */
    extern void schedule(void);
    long rc = 0;
    for (;;) {
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) { rc = 0; break; }
        if (signal_deliverable()) { rc = -EINTR; break; }

        spin_lock_irq(&b->wq.lock, &flags);
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) {
            spin_unlock_irq(&b->wq.lock, flags);
            rc = 0; break;
        }
        __atomic_store_n(&self->state, THREAD_BLOCKED, __ATOMIC_RELEASE);
        spin_unlock_irq(&b->wq.lock, flags);

        schedule();
    }

    spin_lock_irq(&b->wq.lock, &flags);
    if (w.entry.next != 0) {
        if (w.entry.next == &w.entry) {
            b->wq.head = 0;
        } else {
            w.entry.prev->next = w.entry.next;
            w.entry.next->prev = w.entry.prev;
            if (b->wq.head == &w.entry) b->wq.head = w.entry.next;
        }
        w.entry.next = w.entry.prev = 0;
    }
    self->wait_entry = 0;
    self->wait_head  = 0;
    __atomic_store_n(&self->state, THREAD_RUNNING, __ATOMIC_RELEASE);
    spin_unlock_irq(&b->wq.lock, flags);

    if (rc == -EINTR) return -EINTR;
    return 0; /* caller restarts; new fast-path CAS tries again */
}

/* ── FUTEX_UNLOCK_PI ────────────────────────────── */

static long futex_unlock_pi(uint32_t *uaddr, int shared) {
    thread_t *self = thread_current();
    if (!self) return -EFAULT;
    uint32_t tid = (uint32_t)self->tid;

    /* Verify we own this futex */
    uint32_t cur = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
    if ((cur & FUTEX_TID_MASK) != tid)
        return -EPERM;

    /* Restore our priority */
    pi_unboost(self);

    /* Release: clear our TID */
    if (cur & FUTEX_WAITERS) {
        /* Waiters present — clear TID, keep WAITERS bit, wake one */
        __sync_val_compare_and_swap(uaddr, cur, FUTEX_WAITERS);
        futex_wake(uaddr, 1, shared);
    } else {
        /* No waiters — just release */
        __sync_val_compare_and_swap(uaddr, cur, 0);
    }

    return 0;
}

/* ── Timespec → milliseconds ────────────────────── */

/* Convert userspace struct timespec to milliseconds.
 * Returns timeout_ms for event_wait: -1 if ts is NULL (infinite),
 * 0 if timespec is zero, >0 otherwise. Capped at INT32_MAX. */
static int timespec_to_ms(const void *ts) {
    if (!ts) return -1;

    struct { long tv_sec; long tv_nsec; } kts;
    if (copy_from_user(&kts, ts, sizeof(kts)) != 0)
        return -1; /* bad pointer → treat as infinite (caller will fail on uaddr access) */

    if (kts.tv_sec < 0 || kts.tv_nsec < 0) return 0;

    long ms = kts.tv_sec * 1000 + kts.tv_nsec / 1000000;
    if (ms <= 0 && (kts.tv_sec > 0 || kts.tv_nsec > 0))
        ms = 1; /* sub-millisecond → round up to 1ms */
    if (ms > 0x7FFFFFFFL) ms = 0x7FFFFFFFL; /* cap */
    return (int)ms;
}

/* ── FUTEX_REQUEUE / FUTEX_CMP_REQUEUE ─────────── */

/* Requeue: wake up to wake_max waiters on uaddr1, then move up to
 * requeue_max remaining waiters from uaddr1's queue to uaddr2's queue
 * WITHOUT waking them. They'll wake when someone does FUTEX_WAKE on uaddr2.
 *
 * CMP variant: atomically checks *uaddr1 == val3 first (prevents ABA race
 * between userspace check and kernel requeue). Returns -EAGAIN on mismatch. */
static long futex_requeue(uint32_t *uaddr1, uint32_t wake_max,
                          uint32_t requeue_max, uint32_t *uaddr2,
                          int cmp, uint32_t cmpval) {
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;
    uint64_t addr1 = (uint64_t)(uintptr_t)uaddr1;
    uint64_t addr2 = (uint64_t)(uintptr_t)uaddr2;

    int b1 = hash_uaddr(addr1, pid);
    int b2 = hash_uaddr(addr2, pid);
    futex_bucket_t *bk1 = &futex_hash[b1];
    futex_bucket_t *bk2 = &futex_hash[b2];

    /* Lock both buckets — ordered by index to prevent deadlock */
    uint64_t flags;
    if (b1 == b2) {
        spin_lock_irq(&bk1->wq.lock, &flags);
    } else if (b1 < b2) {
        spin_lock_irq(&bk1->wq.lock, &flags);
        spin_lock(&bk2->wq.lock);
    } else {
        spin_lock_irq(&bk2->wq.lock, &flags);
        spin_lock(&bk1->wq.lock);
    }

    /* CMP_REQUEUE: check *uaddr1 == cmpval under lock */
    if (cmp) {
        if (__atomic_load_n(uaddr1, __ATOMIC_ACQUIRE) != cmpval) {
            if (b1 == b2) {
                spin_unlock_irq(&bk1->wq.lock, flags);
            } else if (b1 < b2) {
                spin_unlock(&bk2->wq.lock);
                spin_unlock_irq(&bk1->wq.lock, flags);
            } else {
                spin_unlock(&bk1->wq.lock);
                spin_unlock_irq(&bk2->wq.lock, flags);
            }
            return -EAGAIN;
        }
    }

    long woken = 0, requeued = 0;

    /* Iterate bucket 1's wq — cached because wake+requeue both mutate it. */
    wait_queue_entry_t *start = bk1->wq.head;
    if (start) {
        wait_queue_entry_t *cur = start;
        int revisit_start = 1;

        /* Phase 1: wake up to wake_max matching waiters (removes them). */
        while (cur && (uint32_t)woken < wake_max) {
            wait_queue_entry_t *next = (cur->next == start) ? 0 : cur->next;
            futex_waiter_t *w = (futex_waiter_t *)cur;
            if (w->uaddr == addr1 && w->pid == pid) {
                __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
                thread_t *task = cur->task;
                int old = __sync_val_compare_and_swap(&task->state,
                        THREAD_BLOCKED, THREAD_RUNNABLE);
                if (old == THREAD_BLOCKED) sched_add(task);
                woken++;
                /* waiter stays queued until it finishes its wait */
            }
            cur = next;
            revisit_start = 0;
        }
        (void)revisit_start;
    }

    /* Phase 2: requeue up to requeue_max remaining matching waiters
     * from bucket 1 to bucket 2 (rewrite their key only). Because the
     * bucket 2 wq is different from bucket 1, we must transplant the
     * wq entry — unlink from bucket 1's list, link into bucket 2's
     * list, and re-point t->wait_head so sched_wake routes correctly. */
    wait_queue_entry_t *cur = bk1->wq.head;
    while (cur && (uint32_t)requeued < requeue_max) {
        wait_queue_entry_t *next = cur->next;
        if (next == bk1->wq.head) next = 0; /* loop termination */
        futex_waiter_t *w = (futex_waiter_t *)cur;
        /* Skip already-woken (their thread may be about to finish_wait). */
        if (w->uaddr == addr1 && w->pid == pid &&
            !__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE)) {
            /* Unlink from bucket 1 */
            if (cur->next == cur) {
                bk1->wq.head = 0;
            } else {
                cur->prev->next = cur->next;
                cur->next->prev = cur->prev;
                if (bk1->wq.head == cur) bk1->wq.head = cur->next;
            }
            cur->next = cur->prev = 0;
            /* Insert tail of bucket 2 */
            if (b1 != b2) {
                if (!bk2->wq.head) {
                    bk2->wq.head = cur;
                    cur->next = cur;
                    cur->prev = cur;
                } else {
                    wait_queue_entry_t *tail = bk2->wq.head->prev;
                    cur->prev = tail;
                    cur->next = bk2->wq.head;
                    tail->next = cur;
                    bk2->wq.head->prev = cur;
                }
                /* Re-bind thread to new wq so sched_wake routes through bucket 2. */
                if (cur->task) cur->task->wait_head = &bk2->wq;
            } else {
                /* Same bucket: just re-insert at tail (no wq change). */
                if (!bk1->wq.head) {
                    bk1->wq.head = cur;
                    cur->next = cur;
                    cur->prev = cur;
                } else {
                    wait_queue_entry_t *tail = bk1->wq.head->prev;
                    cur->prev = tail;
                    cur->next = bk1->wq.head;
                    tail->next = cur;
                    bk1->wq.head->prev = cur;
                }
            }
            w->uaddr = addr2;
            requeued++;
        }
        cur = next;
    }

    /* Unlock in reverse order */
    if (b1 == b2) {
        spin_unlock_irq(&bk1->wq.lock, flags);
    } else if (b1 < b2) {
        spin_unlock(&bk2->wq.lock);
        spin_unlock_irq(&bk1->wq.lock, flags);
    } else {
        spin_unlock(&bk1->wq.lock);
        spin_unlock_irq(&bk2->wq.lock, flags);
    }

    return woken + requeued;
}

/* ── Dispatcher ─────────────────────────────────── */

long do_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | 0x100); /* strip PRIVATE + CLOCK_REALTIME */
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
        /* Simplified WAKE_OP: wake val waiters on uaddr, then
         * apply operation on *uaddr2 and conditionally wake val3
         * waiters on uaddr2.  We simplify: wake val on uaddr +
         * wake val3 on uaddr2 (skip the atomic op comparison). */
        long r1 = futex_wake(uaddr, val, shared);
        if (uaddr2) {
            long r2 = futex_wake(uaddr2, val3, shared);
            if (r2 > 0 && r1 >= 0) r1 += r2;
        }
        return r1;
    }
    case 9: /* FUTEX_WAIT_BITSET — treat as FUTEX_WAIT (ignore bitmask) */
        return futex_wait(uaddr, val, timespec_to_ms(timeout), shared);
    case 10: /* FUTEX_WAKE_BITSET — treat as FUTEX_WAKE */
        return futex_wake(uaddr, val, shared);
    default:
        serial_puts("futex: unknown op ");
        serial_hex64((uint64_t)(unsigned)cmd);
        serial_putchar('\n');
        return -ENOSYS;
    }
}
