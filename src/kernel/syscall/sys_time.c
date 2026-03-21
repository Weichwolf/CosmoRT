/* CosmoRT Syscall Layer — time and clock syscalls */

#include "internal.h"

/* ── SYS_clock_gettime (228) / SYS_clock_getres (229) ── */

/* struct k_timespec defined in epoll.h, k_timeval in internal.h */

long do_clock_gettime(int clk_id, struct k_timespec *tp) {
    (void)clk_id; /* CLOCK_REALTIME and CLOCK_MONOTONIC both use uptime */
    if (!tp || !user_ok((uint64_t)tp, 16)) return -EFAULT;
    struct k_timespec kts;
    uint64_t ms = timer_ms();
    kts.tv_sec = (long)(ms / 1000);
    kts.tv_nsec = (long)((ms % 1000) * 1000000);
    kmemcpy(tp, &kts, sizeof(kts));
    return 0;
}

long do_clock_getres(int clk_id, struct k_timespec *tp) {
    (void)clk_id;
    if (tp && !user_ok((uint64_t)tp, sizeof(struct k_timespec))) return -EFAULT;
    if (tp) {
        struct k_timespec kts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
        kmemcpy(tp, &kts, sizeof(kts));
    }
    return 0;
}

long do_nanosleep(const struct k_timespec *req, struct k_timespec *rem) {
    if (!req || !user_ok((uint64_t)req, 16)) return -EFAULT;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;
    struct k_timespec kreq;
    kmemcpy(&kreq, req, sizeof(kreq));
    uint64_t ms = (uint64_t)kreq.tv_sec * 1000 + (uint64_t)(kreq.tv_nsec / 1000000);
    if (ms == 0) ms = 1;
    timer_sleep_ms((uint32_t)ms);
    if (rem) {
        struct k_timespec krem = { .tv_sec = 0, .tv_nsec = 0 };
        kmemcpy(rem, &krem, sizeof(krem));
    }
    return 0;
}

long do_clock_nanosleep(int clk_id, int flags,
                               const struct k_timespec *req, struct k_timespec *rem) {
    (void)clk_id;
    if (req && !user_ok((uint64_t)req, 16)) return -EFAULT;
    if (rem && !user_ok((uint64_t)rem, 16)) return -EFAULT;
    if (flags & TIMER_ABSTIME) {
        if (!req) return -EFAULT;
        struct k_timespec kreq;
        kmemcpy(&kreq, req, sizeof(kreq));
        uint64_t target_ms = (uint64_t)kreq.tv_sec * 1000
                           + (uint64_t)(kreq.tv_nsec / 1000000);
        uint64_t now = timer_ms();
        if (target_ms > now) timer_sleep_ms((uint32_t)(target_ms - now));
        return 0;
    }
    return do_nanosleep(req, rem);
}

long do_gettimeofday(struct k_timeval *tv, void *tz) {
    (void)tz;
    if (tv && !user_ok((uint64_t)tv, 16)) return -EFAULT;
    if (tv) {
        struct k_timeval ktv;
        uint64_t ms = timer_ms();
        ktv.tv_sec = (long)(ms / 1000);
        ktv.tv_usec = (long)((ms % 1000) * 1000);
        kmemcpy(tv, &ktv, sizeof(ktv));
    }
    return 0;
}
