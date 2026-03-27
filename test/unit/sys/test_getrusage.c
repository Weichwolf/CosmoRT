#include "ktest.h"

/* ── getrusage ───────────────────────────────── */

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

struct test_timeval_ru { long tv_sec; long tv_usec; };

struct test_rusage {
    struct test_timeval_ru ru_utime;
    struct test_timeval_ru ru_stime;
    long ru_maxrss;
    long ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt, ru_nswap;
    long ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw, ru_nivcsw;
};

static void test_getrusage(void) {
    puts("\n[getrusage]\n");

    struct test_rusage ru;
    long r = sc2(SYS_GETRUSAGE, RUSAGE_SELF, (long)&ru);
    check_val("getrusage returns 0", r, 0);
    check("ru_maxrss > 0", ru.ru_maxrss > 0);
    check("ru_utime non-zero", ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0);

    /* RUSAGE_CHILDREN: all zeros */
    struct test_rusage ruc;
    r = sc2(SYS_GETRUSAGE, RUSAGE_CHILDREN, (long)&ruc);
    check_val("getrusage children returns 0", r, 0);
    check_val("children ru_maxrss = 0", ruc.ru_maxrss, 0);

    /* Invalid who */
    r = sc2(SYS_GETRUSAGE, 99, (long)&ru);
    check_val("getrusage invalid who → -EINVAL", r, -EINVAL);
}

TEST("getrusage", test_getrusage);
