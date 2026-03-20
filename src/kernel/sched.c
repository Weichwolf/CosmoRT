/* CosmoRT RT Scheduler — Per-core priority queues
 *
 * 32 priority levels (0 = SCHED_OTHER, 1-31 = RT).
 * Per-core, per-priority FIFO queues. Highest non-empty queue runs first.
 * SCHED_FIFO: no timeslice, runs until yield/block.
 * SCHED_RR: timeslice, rotates within same priority.
 * SCHED_OTHER: round-robin at priority 0.
 *
 * Each core has its own run queue + spinlock. No global lock.
 */

#include "thread.h"
#include "process.h"
#include "percpu.h"
#include "serial.h"
#include "spinlock.h"
#include "config.h"
#include "smp.h"

/* Forward declarations */
void sched_add(thread_t *t);

/* Core isolation: 1 = RT-only, 0 = normal */
static uint8_t core_isolated[SMP_MAX_CORES];

/* Per-core run queue */
static struct {
    struct {
        thread_t *head;
        thread_t *tail;
    } rq[PRIO_LEVELS];
    spinlock_t lock;
    uint32_t   bitmap;  /* bit N set = rq[N] non-empty */
} core_rq[SMP_MAX_CORES];

/* Mark a core as isolated (RT-only). SCHED_OTHER threads are redirected to BSP. */
void sched_isolate_core(int core_id) {
    if (core_id >= 0 && core_id < SMP_MAX_CORES)
        core_isolated[core_id] = 1;
}

/* Un-isolate a core — allow SCHED_OTHER threads again. */
void sched_unisolate_core(int core_id) {
    if (core_id >= 0 && core_id < SMP_MAX_CORES)
        core_isolated[core_id] = 0;
}

/* Count isolated cores (excluding core 0 which is never isolated). */
static int count_isolated(void) {
    int n = 0, ncores = smp_num_cores();
    for (int c = 1; c < ncores; c++)
        if (core_isolated[c]) n++;
    return n;
}

/* Approximate RT thread count from per-core bitmaps + running threads.
 * Lock-free: reads are atomic word-sized. Fine for a heuristic. */
static int count_rt_threads(void) {
    int count = 0, ncores = smp_num_cores();
    for (int c = 0; c < ncores; c++) {
        thread_t *t = percpu_data[c].current_thread;
        if (t && (t->sched_policy == SCHED_FIFO || t->sched_policy == SCHED_RR))
            count++;
        uint32_t rt_bits = core_rq[c].bitmap >> PRIO_RT_MIN;
        count += __builtin_popcount(rt_bits);
    }
    return count;
}

/* Find non-isolated core (>0) with fewest SCHED_OTHER threads queued. */
static int find_least_loaded_core(void) {
    int best = -1, best_load = 0x7FFFFFFF;
    int ncores = smp_num_cores();
    for (int c = 1; c < ncores; c++) {
        if (core_isolated[c]) continue;
        /* Approximate: count bits at priority 0 (SCHED_OTHER level) */
        int load = (core_rq[c].bitmap & 1) ? 1 : 0;
        /* Also count current thread if SCHED_OTHER */
        thread_t *t = percpu_data[c].current_thread;
        if (t && t->sched_policy == SCHED_OTHER) load++;
        if (load < best_load) { best_load = load; best = c; }
    }
    return best;
}

/* Find an isolated core to un-isolate. */
static int find_isolated_core(void) {
    int ncores = smp_num_cores();
    for (int c = 1; c < ncores; c++)
        if (core_isolated[c]) return c;
    return -1;
}

/* Migrate RT threads with cpu_affinity == -1 to isolated cores (round-robin).
 * Only migrates threads found in run queues (not currently running). */
static void migrate_rt_to_isolated(void) {
    int ncores = smp_num_cores();

    /* Collect isolated cores */
    int iso[SMP_MAX_CORES], niso = 0;
    for (int c = 1; c < ncores; c++)
        if (core_isolated[c]) iso[niso++] = c;
    if (!niso) return;

    int rr = 0; /* round-robin index */

    for (int c = 0; c < ncores; c++) {
        if (core_isolated[c]) continue; /* skip isolated — RT already there */

        uint64_t flags;
        spin_lock_irq(&core_rq[c].lock, &flags);

        for (int p = PRIO_RT_MIN; p <= PRIO_RT_MAX; p++) {
            thread_t **prev = &core_rq[c].rq[p].head;
            thread_t *t = *prev;
            while (t) {
                if ((t->sched_policy == SCHED_FIFO || t->sched_policy == SCHED_RR)
                    && t->cpu_affinity < 0)
                {
                    /* Remove from this queue */
                    *prev = t->rq_next;
                    if (!*prev) core_rq[c].rq[p].tail = 0;
                    if (!core_rq[c].rq[p].head) {
                        core_rq[c].rq[p].tail = 0;
                        core_rq[c].bitmap &= ~(1u << p);
                    }
                    thread_t *migrating = t;
                    t = t->rq_next;
                    migrating->rq_next = 0;

                    /* Pin to isolated core and enqueue */
                    int target = iso[rr % niso];
                    rr++;
                    migrating->cpu_affinity = target;

                    spin_unlock_irq(&core_rq[c].lock, flags);

                    /* Enqueue on target (sched_add handles locking) */
                    sched_add(migrating);

                    spin_lock_irq(&core_rq[c].lock, &flags);
                    /* Restart scan from head — queue may have changed */
                    prev = &core_rq[c].rq[p].head;
                    t = *prev;
                    continue;
                }
                prev = &t->rq_next;
                t = t->rq_next;
            }
        }

        spin_unlock_irq(&core_rq[c].lock, flags);
    }
}

/* Dynamic RT core isolation. Called every ~1s from timer IRQ context. */
static void sched_rebalance(void) {
    int ncores = smp_num_cores();
    if (ncores < 4) return; /* 2-core: no isolation, RT preempts POSIX */

    int rt_count = count_rt_threads();
    int target = rt_count;
    int max_iso = ncores / 4;
    if (target > max_iso) target = max_iso;
    if (rt_count == 0) target = 0;

    int current_iso = count_isolated();
    if (target == current_iso) {
        /* Even if count unchanged, migrate unpinned RT threads */
        if (current_iso > 0)
            migrate_rt_to_isolated();
        return;
    }

    if (target > current_iso) {
        /* Isolate more cores */
        int need = target - current_iso;
        for (int i = 0; i < need; i++) {
            int c = find_least_loaded_core();
            if (c < 0) break;
            sched_isolate_core(c);
            serial_puts("sched: isolate core ");
            serial_putchar('0' + (c % 10));
            serial_putchar('\n');
        }
    } else {
        /* Un-isolate cores */
        int excess = current_iso - target;
        for (int i = 0; i < excess; i++) {
            int c = find_isolated_core();
            if (c < 0) break;
            /* Unpin RT threads on this core before un-isolating */
            uint64_t flags;
            spin_lock_irq(&core_rq[c].lock, &flags);
            for (int p = PRIO_RT_MIN; p <= PRIO_RT_MAX; p++) {
                thread_t *t = core_rq[c].rq[p].head;
                while (t) { t->cpu_affinity = -1; t = t->rq_next; }
            }
            spin_unlock_irq(&core_rq[c].lock, flags);
            /* Also unpin running RT thread */
            thread_t *cur = percpu_data[c].current_thread;
            if (cur && (cur->sched_policy == SCHED_FIFO || cur->sched_policy == SCHED_RR))
                cur->cpu_affinity = -1;
            sched_unisolate_core(c);
            serial_puts("sched: unisolate core ");
            serial_putchar('0' + (c % 10));
            serial_putchar('\n');
        }
    }

    if (target > 0)
        migrate_rt_to_isolated();
}

/* Add thread to appropriate core's queue.
 * cpu_affinity >= 0: that core. Otherwise: current core (cache locality).
 * SCHED_OTHER threads are redirected away from isolated cores. */
void sched_add(thread_t *t) {
    int prio = t->priority;
    if (prio < 0) prio = 0;
    if (prio >= PRIO_LEVELS) prio = PRIO_LEVELS - 1;

    int cpu = t->cpu_affinity;
    if (cpu < 0 || cpu >= SMP_MAX_CORES) {
        /* Round-robin across cores */
        static volatile int next_cpu = 0;
        cpu = __sync_fetch_and_add(&next_cpu, 1) % smp_num_cores();
    }

    /* Isolated core: only RT threads (SCHED_FIFO/SCHED_RR) may run */
    if (core_isolated[cpu] && t->sched_policy == SCHED_OTHER)
        cpu = 0; /* redirect to BSP */

    t->state = THREAD_RUNNABLE;
    t->rq_next = 0;

    uint64_t flags;
    spin_lock_irq(&core_rq[cpu].lock, &flags);

    if (core_rq[cpu].rq[prio].tail) {
        core_rq[cpu].rq[prio].tail->rq_next = t;
    } else {
        core_rq[cpu].rq[prio].head = t;
    }
    core_rq[cpu].rq[prio].tail = t;
    core_rq[cpu].bitmap |= (1u << prio);

    spin_unlock_irq(&core_rq[cpu].lock, flags);
}

/* Remove and return highest-priority runnable thread from current core */
thread_t *sched_pick(void) {
    int cpu = percpu_self()->core_id;

    uint64_t flags;
    spin_lock_irq(&core_rq[cpu].lock, &flags);

    thread_t *t = 0;

    if (core_rq[cpu].bitmap) {
        int prio = 31 - __builtin_clz(core_rq[cpu].bitmap);

        t = core_rq[cpu].rq[prio].head;
        if (t) {
            core_rq[cpu].rq[prio].head = t->rq_next;
            if (!core_rq[cpu].rq[prio].head) {
                core_rq[cpu].rq[prio].tail = 0;
                core_rq[cpu].bitmap &= ~(1u << prio);
            }
            t->rq_next = 0;
        }
    }

    spin_unlock_irq(&core_rq[cpu].lock, flags);
    return t;
}

/* Timer preemption: called from IRQ handler.
 * Saves current thread, picks next, restores into frame.
 * frame layout: [r15..rax, vector, error, rip, cs, rflags, rsp, ss] */
void sched_preempt(void *frame_ptr) {
    /* Periodic rebalance (~1s at 10Hz) — only on BSP to avoid races */
    static int rebalance_counter;
    if (percpu_self()->core_id == 0 && ++rebalance_counter >= 10) {
        rebalance_counter = 0;
        sched_rebalance();
    }

    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || cur->state != THREAD_RUNNING) return;

    uint64_t *f = (uint64_t *)frame_ptr;

    /* Check if from Ring 3 (CS RPL = 3) */
    if ((f[18] & 3) != 3) return; /* kernel mode — don't preempt */

    /* SCHED_FIFO: never preempt (runs until yield/block) */
    if (cur->sched_policy == SCHED_FIFO) return;

    /* SCHED_RR: decrement timeslice */
    if (cur->sched_policy == SCHED_RR) {
        if (cur->timeslice > 0) {
            cur->timeslice--;
            return; /* still has time */
        }
        cur->timeslice = RR_TIMESLICE; /* reset for next run */
    }

    /* Save current thread context from interrupt frame */
    cur->r15 = f[0]; cur->r14 = f[1]; cur->r13 = f[2]; cur->r12 = f[3];
    cur->r11 = f[4]; cur->r10 = f[5]; cur->r9 = f[6];  cur->r8 = f[7];
    cur->rbp = f[8]; cur->rdi = f[9]; cur->rsi = f[10]; cur->rdx = f[11];
    cur->rcx = f[12]; cur->rbx = f[13]; cur->rax = f[14];
    cur->rip = f[17]; cur->rflags = f[19]; cur->rsp = f[20];

    /* Save current FS base (TLS) */
    {
        uint32_t fs_lo, fs_hi;
        __asm__ volatile("rdmsr" : "=a"(fs_lo), "=d"(fs_hi) : "c"(0xC0000100));
        cur->fs_base = ((uint64_t)fs_hi << 32) | fs_lo;
    }

    /* Put current back in run queue */
    sched_add(cur);

    /* Pick next thread */
    thread_t *next = sched_pick();
    if (!next || next == cur) {
        /* No other thread: keep running current */
        if (next) next->state = THREAD_RUNNING;
        return;
    }

    /* Switch to next thread */
    next->state = THREAD_RUNNING;
    cpu->current_thread = next;

    /* Load page tables */
    __asm__ volatile("mov %0, %%cr3" :: "r"(virt_to_phys(next->proc->pml4)) : "memory");

    /* Set kernel stack */
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(next->kstack_top);
    cpu->kernel_rsp = next->kstack_top;

    /* Load next thread's FS base (TLS) */
    {
        uint64_t fs = next->fs_base;
        __asm__ volatile("wrmsr" :: "c"(0xC0000100),
                         "a"((uint32_t)fs), "d"((uint32_t)(fs >> 32)));
    }

    /* Restore next thread context into interrupt frame */
    f[0] = next->r15; f[1] = next->r14; f[2] = next->r13; f[3] = next->r12;
    f[4] = next->r11; f[5] = next->r10; f[6] = next->r9;  f[7] = next->r8;
    f[8] = next->rbp; f[9] = next->rdi; f[10] = next->rsi; f[11] = next->rdx;
    f[12] = next->rcx; f[13] = next->rbx; f[14] = next->rax;
    f[17] = next->rip; f[18] = 0x2B; /* CS user code 64 */
    f[19] = next->rflags; f[20] = next->rsp; f[21] = 0x23; /* SS user data */
}

/* Scheduler init */
void sched_init(void) {
    for (int c = 0; c < SMP_MAX_CORES; c++) {
        for (int i = 0; i < PRIO_LEVELS; i++) {
            core_rq[c].rq[i].head = core_rq[c].rq[i].tail = 0;
        }
        core_rq[c].lock = (spinlock_t)SPINLOCK_INIT;
        core_rq[c].bitmap = 0;
    }
    serial_puts("sched: per-core RT init (");
    char t[4]; int ti = 0, v = PRIO_LEVELS;
    do { t[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(t[ti]);
    serial_puts(" prio x ");
    ti = 0; v = SMP_MAX_CORES;
    do { t[ti++] = '0' + v % 10; v /= 10; } while (v);
    while (ti--) serial_putchar(t[ti]);
    serial_puts(" cores)\n");
}

/* Per-core ISR stack for idle HLT */
static uint8_t idle_stacks[SMP_MAX_CORES][8192] __attribute__((aligned(16)));

/* Scheduler loop — runs on each core (BSP + APs) */
void sched_loop(void) {
    int core = percpu_self()->core_id;
    extern void tss_set_rsp0(uint64_t rsp0);

    for (;;) {
        thread_t *next = sched_pick();
        if (next) {
            thread_run(next);
            if (next->state == THREAD_RUNNABLE)
                sched_add(next);
        } else {
            uint64_t idle_top = (uint64_t)(uintptr_t)
                (idle_stacks[core] + sizeof(idle_stacks[core]));
            tss_set_rsp0(idle_top);
            percpu_self()->kernel_rsp = idle_top;
            /* Poll network while idle (no-op if no NIC) */
            extern void net_poll(void);
            net_poll();
            __asm__ volatile("sti; hlt");
        }
    }
}
