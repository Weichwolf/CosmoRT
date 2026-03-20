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
            __asm__ volatile("sti; hlt");
        }
    }
}
