/* CosmoRT Scheduler — unified context_switch, per-core idle threads */

#include "proc/thread.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "hw/serial.h"
#include "spinlock.h"
#include "config.h"
#include "core/smp.h"
#include "core/rt.h"
#include "core/timer.h"
#include "core/event_queue.h"
#include "core/rcu.h"
#include "core/nohz.h"
#include "arch/arch.h"
#include <stddef.h>

_Static_assert(offsetof(thread_t, kstack_rsp) == 168,
               "THREAD_KSTACK_RSP_OFF mismatch");

extern void context_switch(thread_t *prev, thread_t *next);
extern void context_resume(thread_t *t) __attribute__((noreturn));
extern void proc_enter_ring3(thread_t *t) __attribute__((noreturn));
extern uint64_t pml4[];
extern void tss_set_rsp0(uint64_t rsp0);

#define IDLE_TID_BASE       (-1)
#define IDLE_STACK_SIZE     16384
static uint8_t core_isolated[SMP_MAX_CORES];

static struct {
    struct {
        thread_t *head;
        thread_t *tail;
    } rq[PRIO_LEVELS];
    spinlock_t lock;
    uint32_t   bitmap;
} core_rq[SMP_MAX_CORES];

static thread_t idle_threads[SMP_MAX_CORES];
static uint8_t idle_stacks[SMP_MAX_CORES][IDLE_STACK_SIZE] __attribute__((aligned(16)));
static thread_t *prev_thread[SMP_MAX_CORES];

void userspace_entry_trampoline(void);
void kthread_entry_trampoline(void);
void switch_to_idle(thread_t *cur);
void schedule(void);

void thread_init_kstack(thread_t *t, void (*entry)(void)) {
    uint64_t *sp = (uint64_t *)t->kstack_top;
    *--sp = (uint64_t)entry;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    t->kstack_rsp = (uint64_t)sp;
}

void userspace_entry_trampoline(void) {
    thread_t *t = thread_current();
    arch_sti();
    arch_set_fs_base(t->fs_base);
    arch_fxrstor(t->fxsave_area);
    arch_set_cr3(virt_to_phys(t->proc->pml4));
    proc_enter_ring3(t);
}

void kthread_entry_trampoline(void) {
    thread_t *t = thread_current();
    arch_sti();
    t->kthread_fn(t->kthread_arg);
    t->state = THREAD_DEAD;
    switch_to_idle(t);
    __builtin_unreachable();
}

thread_t *sched_get_idle(int core) {
    return &idle_threads[core];
}

void switch_to_idle(thread_t *cur) {
    percpu_t *cpu = percpu_self();
    int core = cpu->core_id;

    prev_thread[core] = cur;

    thread_t *idle = &idle_threads[core];
    cpu->current_thread = idle;
    idle->state = THREAD_RUNNING;
    context_switch(cur, idle);
}

static int count_isolated(void) {
    int n = 0, ncores = smp_num_cores();
    for (int c = 1; c < ncores; c++)
        if (core_isolated[c]) n++;
    return n;
}

static int count_rt_threads(void) {
    int count = 0, ncores = smp_num_cores();
    for (int c = 0; c < ncores; c++) {
        thread_t *t = percpu_data[c].current_thread;
        if (t && (t->sched_policy == SCHED_FIFO || t->sched_policy == SCHED_RR))
            count++;
        uint32_t rt_bits = core_rq[c].bitmap >> PRIO_RT_MIN;
        {
            uint32_t v = rt_bits;
            v = v - ((v >> 1) & 0x55555555U);
            v = (v & 0x33333333U) + ((v >> 2) & 0x33333333U);
            v = (v + (v >> 4)) & 0x0F0F0F0FU;
            count += (int)((v * 0x01010101U) >> 24);
        }
    }
    return count;
}

static int find_least_loaded_core(void) {
    int best = -1, best_load = 0x7FFFFFFF;
    int ncores = smp_num_cores();
    for (int c = 1; c < ncores; c++) {
        if (core_isolated[c]) continue;
        int load = (core_rq[c].bitmap & 1) ? 1 : 0;
        thread_t *t = percpu_data[c].current_thread;
        if (t && t->sched_policy == SCHED_OTHER) load++;
        if (load < best_load) { best_load = load; best = c; }
    }
    return best;
}

static int find_isolated_core(void) {
    int ncores = smp_num_cores();
    for (int c = 1; c < ncores; c++)
        if (core_isolated[c]) return c;
    return -1;
}

__attribute__((cold))
void sched_isolate_core(int core_id) {
    if (core_id >= 0 && core_id < SMP_MAX_CORES)
        core_isolated[core_id] = 1;
}

__attribute__((cold))
void sched_unisolate_core(int core_id) {
    if (core_id >= 0 && core_id < SMP_MAX_CORES)
        core_isolated[core_id] = 0;
}

__attribute__((hot))
void sched_add(thread_t *t) {
    int prio = t->priority;
    if (prio < 0) prio = 0;
    if (prio >= PRIO_LEVELS) prio = PRIO_LEVELS - 1;

    int cpu = t->cpu_affinity;
    int ncores = smp_num_cores();

    if (cpu < 0 || cpu >= SMP_MAX_CORES) {
        if (ncores <= 1) {
            cpu = 0;
        } else if (t->sched_policy == SCHED_FIFO || t->sched_policy == SCHED_RR) {
            static volatile int next_iso = 0;
            int best = -1;
            for (int i = 0; i < ncores; i++) {
                int c = 1 + ((__sync_fetch_and_add(&next_iso, 0) + i) % (ncores - 1));
                if (core_isolated[c]) { best = c; __sync_fetch_and_add(&next_iso, 1); break; }
            }
            cpu = (best >= 0) ? best : 0;
        } else {
            static volatile int next_cpu = 0;
            cpu = 1 + (__sync_fetch_and_add(&next_cpu, 1) % (ncores - 1));
        }
    }

    if (core_isolated[cpu] && t->sched_policy == SCHED_OTHER) {
        int found = -1;
        for (int c = 1; c < ncores; c++) {
            if (!core_isolated[c]) { found = c; break; }
        }
        cpu = (found >= 0) ? found : 1;
    }

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

    int self = percpu_self()->core_id;
    if (cpu != self && cpu < smp_num_cores())
        rt_wake(cpu);
}

void sched_wake(thread_t *t) {
    if (!t) return;
    int old = __sync_val_compare_and_swap(&t->state, THREAD_BLOCKED, THREAD_RUNNABLE);
    if (old != THREAD_BLOCKED) return;
    sched_add(t);
}

void sched_block(void) {
    thread_t *cur = thread_current();
    cur->blocking_info.type = BLOCK_MUTEX;
    schedule();
}

__attribute__((hot))
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

static void migrate_rt_to_isolated(void) {
    int ncores = smp_num_cores();

    int iso[SMP_MAX_CORES], niso = 0;
    for (int c = 1; c < ncores; c++)
        if (core_isolated[c]) iso[niso++] = c;
    if (!niso) return;

    int rr = 0;

    for (int c = 0; c < ncores; c++) {
        if (core_isolated[c]) continue;

        thread_t *migrate_list = 0;

        uint64_t flags;
        spin_lock_irq(&core_rq[c].lock, &flags);

        for (int p = PRIO_RT_MIN; p <= PRIO_RT_MAX; p++) {
            thread_t **prev = &core_rq[c].rq[p].head;
            thread_t *t = *prev;
            while (t) {
                if ((t->sched_policy == SCHED_FIFO || t->sched_policy == SCHED_RR)
                    && t->cpu_affinity < 0)
                {
                    *prev = t->rq_next;
                    if (!*prev) core_rq[c].rq[p].tail = 0;
                    if (!core_rq[c].rq[p].head) {
                        core_rq[c].rq[p].tail = 0;
                        core_rq[c].bitmap &= ~(1u << p);
                    }
                    thread_t *migrating = t;
                    t = t->rq_next;

                    int target = iso[rr % niso];
                    rr++;
                    migrating->cpu_affinity = target;
                    migrating->rq_next = migrate_list;
                    migrate_list = migrating;
                    continue;
                }
                prev = &t->rq_next;
                t = t->rq_next;
            }
        }

        spin_unlock_irq(&core_rq[c].lock, flags);

        while (migrate_list) {
            thread_t *next = migrate_list->rq_next;
            migrate_list->rq_next = 0;
            sched_add(migrate_list);
            migrate_list = next;
        }
    }
}

static void sched_rebalance(void) {
    int ncores = smp_num_cores();
    if (ncores < 4) return;

    int rt_count = count_rt_threads();
    int target = rt_count;
    int max_iso = ncores / 4;
    if (target > max_iso) target = max_iso;
    if (rt_count == 0) target = 0;

    int current_iso = count_isolated();
    if (target == current_iso) {
        if (current_iso > 0)
            migrate_rt_to_isolated();
        return;
    }

    if (target > current_iso) {
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
        int excess = current_iso - target;
        for (int i = 0; i < excess; i++) {
            int c = find_isolated_core();
            if (c < 0) break;
            uint64_t flags;
            spin_lock_irq(&core_rq[c].lock, &flags);
            for (int p = PRIO_RT_MIN; p <= PRIO_RT_MAX; p++) {
                thread_t *t = core_rq[c].rq[p].head;
                while (t) { t->cpu_affinity = -1; t = t->rq_next; }
            }
            spin_unlock_irq(&core_rq[c].lock, flags);
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

void sched_preempt(void *frame_ptr) {
    static int rebalance_counter;
    if (percpu_self()->core_id == 0 && ++rebalance_counter >= 1000) {
        rebalance_counter = 0;
        sched_rebalance();
    }

    epoll_check_timeouts();
    rcu_check_quiescent();

    {
        thread_t *alarm_t = percpu_self()->current_thread;
        if (alarm_t && alarm_t->proc) {
            process_t *ap = alarm_t->proc;
            if (ap->alarm_deadline_ms > 0 && timer_ms() >= ap->alarm_deadline_ms) {
                ap->alarm_deadline_ms = 0;
                void *handler = ap->sig_actions[SIGALRM].sa_handler;
                if ((uint64_t)handler > 1)
                    ap->sig_pending |= SIG_BIT(SIGALRM);
                else if (handler == (void *)0) {
                    ap->exit_signal = SIGALRM;
                    ap->sig_pending |= SIG_BIT(SIGALRM);
                }
            }
        }
    }
    if (percpu_self()->core_id == 0) {
        extern void check_alarm_timers(void);
        check_alarm_timers();
    }

    if (percpu_self()->core_id == 0) {
        extern void vt_flush(int vt_id);
        extern int vt_active(void);
        vt_flush(vt_active());
    }

    percpu_t *cpu = percpu_self();
    thread_t *cur = cpu->current_thread;
    if (!cur || cur->state != THREAD_RUNNING) return;
    if (cur->tid < 0) return;

    uint64_t *f = (uint64_t *)frame_ptr;

    if ((f[18] & 3) != 3) return;

    extern void check_pending_signals(void);
    {
        process_t *p = cur->proc;
        uint64_t deliverable = p ? ((p->sig_pending | cur->sig_thread_pending) & ~cur->sig_blocked) : 0;
        int alarm_due = p && p->alarm_deadline_ms > 0 && timer_ms() >= p->alarm_deadline_ms;
        if (deliverable || alarm_due) {
            cpu->in_preempt = 1;
            cur->r15 = f[0]; cur->r14 = f[1]; cur->r13 = f[2]; cur->r12 = f[3];
            cur->r11 = f[4]; cur->r10 = f[5]; cur->r9 = f[6];  cur->r8 = f[7];
            cur->rbp = f[8]; cur->rdi = f[9]; cur->rsi = f[10]; cur->rdx = f[11];
            cur->rcx = f[12]; cur->rbx = f[13]; cur->rax = f[14];
            cur->rip = f[17]; cur->rflags = f[19]; cur->rsp = f[20];
            cur->fs_base = arch_get_fs_base();
            check_pending_signals();
            f[0] = cur->r15; f[1] = cur->r14; f[2] = cur->r13; f[3] = cur->r12;
            f[4] = cur->r11; f[5] = cur->r10; f[6] = cur->r9;  f[7] = cur->r8;
            f[8] = cur->rbp; f[9] = cur->rdi; f[10] = cur->rsi; f[11] = cur->rdx;
            f[12] = cur->rcx; f[13] = cur->rbx; f[14] = cur->rax;
            f[17] = cur->rip; f[19] = cur->rflags; f[20] = cur->rsp;
            cpu->in_preempt = 0;
        }
    }

    if (cur->preempt_count > 0) {
        cur->need_resched = 1;
        return;
    }

    if (cur->sched_policy == SCHED_FIFO || cur->sched_policy == SCHED_RR) {
        int cpu_id = cpu->core_id;
        uint32_t bm = core_rq[cpu_id].bitmap;
        uint32_t higher = bm & ~((1u << (cur->priority + 1)) - 1);
        if (!higher) {
            if (cur->sched_policy == SCHED_RR) {
                if (cur->timeslice > 0) {
                    cur->timeslice--;
                    return;
                }
                if (!(bm & (1u << cur->priority))) return;
                cur->timeslice = RR_TIMESLICE;
            } else {
                return;
            }
        }
    }

    cur->r15 = f[0]; cur->r14 = f[1]; cur->r13 = f[2]; cur->r12 = f[3];
    cur->r11 = f[4]; cur->r10 = f[5]; cur->r9 = f[6];  cur->r8 = f[7];
    cur->rbp = f[8]; cur->rdi = f[9]; cur->rsi = f[10]; cur->rdx = f[11];
    cur->rcx = f[12]; cur->rbx = f[13]; cur->rax = f[14];
    cur->rip = f[17]; cur->rflags = f[19]; cur->rsp = f[20];

    cur->fs_base = arch_get_fs_base();
    arch_fxsave(cur->fxsave_area);

    extern void lapic_eoi(void);
    lapic_eoi();
    arch_set_kernel_gs_base((uint64_t)(uintptr_t)cpu);

    cur->blocking_info.type = BLOCK_PREEMPT;
    schedule();

    f[0] = cur->r15; f[1] = cur->r14; f[2] = cur->r13; f[3] = cur->r12;
    f[4] = cur->r11; f[5] = cur->r10; f[6] = cur->r9;  f[7] = cur->r8;
    f[8] = cur->rbp; f[9] = cur->rdi; f[10] = cur->rsi; f[11] = cur->rdx;
    f[12] = cur->rcx; f[13] = cur->rbx; f[14] = cur->rax;
    f[17] = cur->rip; f[19] = cur->rflags; f[20] = cur->rsp;
    arch_set_fs_base(cur->fs_base);
    arch_fxrstor(cur->fxsave_area);
    if (cur->proc)
        arch_set_cr3(virt_to_phys(cur->proc->pml4));
}

__attribute__((used))
void schedule(void) {
    thread_t *cur = thread_current();
    percpu_t *cpu = percpu_self();
    int core = cpu->core_id;

    prev_thread[core] = cur;

    thread_t *idle = &idle_threads[core];
    cpu->current_thread = idle;
    idle->state = THREAD_RUNNING;

    context_switch(cur, idle);
}

__attribute__((cold))
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

static int sched_thread_valid(thread_t *t) {
    if (t->state == THREAD_DEAD || t->state == THREAD_FREE)
        return 0;
    if (t->kthread_fn)
        return 1;
    if (!t->proc || !t->proc->pml4)
        return 0;
    return 1;
}

void sched_loop_once(void) {
    thread_t *next = sched_pick();
    if (next) {
        if (!sched_thread_valid(next)) {
            if (next->state == THREAD_DEAD && !next->proc)
                thread_free(next);
            return;
        }
        percpu_t *cpu = percpu_self();
        next->state = THREAD_RUNNING;
        cpu->current_thread = next;
        tss_set_rsp0(next->kstack_top);
        cpu->kernel_rsp = next->kstack_top;

        thread_t *idle = &idle_threads[cpu->core_id];
        context_switch(idle, next);

        cpu->current_thread = idle;
        if (next->state == THREAD_RUNNABLE)
            sched_add(next);
    } else {
        arch_halt();
    }
}

void sched_loop(void) {
    int core = percpu_self()->core_id;
    percpu_t *cpu = percpu_self();

    thread_t *idle = &idle_threads[core];
    idle->state = THREAD_RUNNING;
    idle->tid = IDLE_TID_BASE - core;
    idle->kstack_rsp = 0;
    idle->proc = 0;
    idle->kthread_fn = 0;
    idle->priority = -1;
    idle->sched_policy = SCHED_OTHER;
    idle->preempt_count = 0;
    idle->need_resched = 0;
    cpu->current_thread = idle;

    for (;;) {
        cpu->need_resched = 0;

        thread_t *pp = prev_thread[core];
        if (pp) {
            prev_thread[core] = 0;
            switch (pp->blocking_info.type) {
            case BLOCK_YIELD:
                break;
            case BLOCK_PREEMPT:
                if (pp->state == THREAD_RUNNING)
                    pp->state = THREAD_RUNNABLE;
                sched_add(pp);
                break;
            case BLOCK_MUTEX:
                pp->state = THREAD_BLOCKED;
                break;
            case BLOCK_EVENT: {
                pp->state = THREAD_BLOCKED;
                pp->wake_at_tsc = pp->blocking_info.deadline_tsc;
                if (pp->wake_at_tsc)
                    epoll_sleeper_add_ext(pp);
                event_queue_t *eq = (event_queue_t *)pp->blocking_info.context;
                if (eq) {
                    __asm__ volatile("mfence" ::: "memory");
                    if (arch_load_acquire(&eq->head) != eq->tail) {
                        pp->state = THREAD_RUNNABLE;
                        sched_add(pp);
                        break;
                    }
                }
                if (!pp->wake_at_tsc)
                    epoll_sleeper_add_ext(pp);
                break;
            }
            case BLOCK_SLEEP:
                pp->state = THREAD_BLOCKED;
                pp->wake_at_tsc = pp->blocking_info.deadline_tsc;
                epoll_sleeper_add_ext(pp);
                break;
            default:
                if (pp->state == THREAD_RUNNABLE)
                    sched_add(pp);
                break;
            }
            pp->blocking_info.type = BLOCK_NONE;
        }

        thread_t *next = sched_pick();
        if (next) {
            if (!sched_thread_valid(next)) {
                if (next->state == THREAD_DEAD && !next->proc)
                    thread_free(next);
                continue;
            }

            next->state = THREAD_RUNNING;
            cpu->current_thread = next;
            tss_set_rsp0(next->kstack_top);
            cpu->kernel_rsp = next->kstack_top;

            context_switch(idle, next);

            cpu->current_thread = idle;
        } else {
            uint64_t idle_top = (uint64_t)(uintptr_t)
                (idle_stacks[core] + sizeof(idle_stacks[core]));
            tss_set_rsp0(idle_top);
            cpu->kernel_rsp = idle_top;

            rcu_check_quiescent();
            epoll_check_timeouts();

            if (core_isolated[core] && !nohz_is_tickless(core))
                nohz_enter_tickless(core);
            else if (!core_isolated[core] && nohz_is_tickless(core))
                nohz_exit_tickless(core);

            if (nohz_is_tickless(core)) {
                uint64_t dl = epoll_nearest_deadline_tsc(core);
                if (dl)
                    nohz_arm_oneshot(core, dl);
                else
                    nohz_cancel_oneshot(core);
            }

            if (core_rq[core].bitmap || prev_thread[core])
                continue;
            arch_halt();
        }
    }
}
