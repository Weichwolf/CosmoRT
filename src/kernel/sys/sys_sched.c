/* CosmoRT — Scheduling syscalls */

#include "internal.h"
#include "core/smp.h"

/* ── SYS_sched_yield (24) ────────────────────────── */

long do_sched_yield(void) {
    /* schedule() re-enqueues RUNNING thread at tail, picks next.
     * Returns via normal syscall path → check_signals_syscall_path runs. */
    schedule();
    return 0;
}

/* ── SYS_sched_setaffinity (203) / getaffinity (204) ── */

long do_sched_setaffinity(int pid, size_t cpusetsize, const uint64_t *mask) {
    (void)pid; /* pid=0 → current thread */
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    uint64_t k_mask;
    int r = copy_from_user(&k_mask, mask, 8);
    if (r) return r;
    if (k_mask == 0) return -EINVAL;
    int core = __builtin_ctzll(k_mask);
    if (core >= SMP_MAX_CORES) return -EINVAL;
    t->cpu_affinity = core;
    return 0;
}

long do_sched_getaffinity(int pid, size_t cpusetsize, uint64_t *mask) {
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (cpusetsize < 8 || !mask) return -EINVAL;
    /* Linux: sched_getaffinity returns the set of all CPUs the thread
     * could possibly run on. musl sysconf(_SC_NPROCESSORS_ONLN/_CONF)
     * popcount() on the mask — returning ~0ULL yields 64 spurious CPUs. */
    uint64_t kmask;
    if (t->cpu_affinity >= 0) {
        kmask = 1ULL << t->cpu_affinity;
    } else {
        int n = smp_num_cores();
        if (n <= 0) n = 1;
        if (n >= 64) kmask = ~0ULL;
        else kmask = (1ULL << n) - 1;
    }
    { int r = copy_to_user(mask, &kmask, 8); if (r) return r; }
    return (long)sizeof(uint64_t);
}

/* ── SYS_sched_setscheduler (144) / getscheduler (145) ── */

struct sched_param_k { int sched_priority; };

long do_sched_setscheduler(int pid, int policy, const void *param_) {
    const struct sched_param_k *param = (const struct sched_param_k *)param_;
    (void)pid;
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (policy < 0 || policy > 2) return -EINVAL;
    t->sched_policy = policy;
    if (param) {
        struct sched_param_k kp;
        int r = copy_from_user(&kp, param, sizeof(kp));
        if (r) return r;
        if (kp.sched_priority < 0 || kp.sched_priority >= PRIO_LEVELS) return -EINVAL;
        t->priority = kp.sched_priority;
    }
    return 0;
}

long do_sched_getscheduler(int pid) {
    (void)pid;
    thread_t *t = thread_current();
    return t ? t->sched_policy : 0;
}

long do_sched_setparam(int pid, const void *param_) {
    const struct sched_param_k *param = (const struct sched_param_k *)param_;
    (void)pid;
    thread_t *t = thread_current();
    if (!t || !param) return -EFAULT;
    struct sched_param_k kp;
    int r = copy_from_user(&kp, param, sizeof(kp));
    if (r) return r;
    if (kp.sched_priority < 0 || kp.sched_priority >= PRIO_LEVELS) return -EINVAL;
    t->priority = kp.sched_priority;
    return 0;
}

long do_sched_getparam(int pid, void *param_) {
    struct sched_param_k *param = (struct sched_param_k *)param_;
    (void)pid;
    thread_t *t = thread_current();
    if (!t || !param) return -EFAULT;
    struct sched_param_k kparam;
    kparam.sched_priority = t->priority;
    { int r = copy_to_user(param, &kparam, sizeof(kparam)); if (r) return r; }
    return 0;
}
