/* LTP clock_adjtime01/02 — ported to ktest */
#include "ktest.h"

/*
 * struct timex layout for x86_64 Linux ABI.
 * sizeof(struct timex) = 208 bytes.
 */
struct k_timex {
    unsigned int modes;         /* 0 */
    long         offset;        /* 8 */
    long         freq;          /* 16 */
    long         maxerror;      /* 24 */
    long         esterror;      /* 32 */
    int          status;        /* 40 */
    long         constant;      /* 48 */
    long         precision;     /* 56 */
    long         tolerance;     /* 64 */
    struct k_timeval time;      /* 72 */
    long         tick;          /* 88 */
    long         ppsfreq;       /* 96 */
    long         jitter;        /* 104 */
    int          shift;         /* 112 */
    long         stabil;        /* 120 */
    long         jitcnt;        /* 128 */
    long         calcnt;        /* 136 */
    long         errcnt;        /* 144 */
    long         stbcnt;        /* 152 */
    int          tai;           /* 160 */
    int          __padding[11]; /* pad to 208 */
};

/* ── clock_adjtime01: basic read (modes=0) succeeds ── */

static void test_clock_adjtime_read(void) {
    puts("\n[ltp/clock_adjtime01-read]\n");

    struct k_timex tx;
    for (int i = 0; i < (int)sizeof(tx); i++)
        ((char *)&tx)[i] = 0;

    long r = sc2(SYS_CLOCK_ADJTIME, CLOCK_REALTIME, (long)&tx);
    check_ge("clock_adjtime read", r, 0);
}

/* ── clock_adjtime02: EINVAL for invalid clock id ── */

static void test_clock_adjtime_einval_clock(void) {
    puts("\n[ltp/clock_adjtime02-einval-clock]\n");

    struct k_timex tx;
    for (int i = 0; i < (int)sizeof(tx); i++)
        ((char *)&tx)[i] = 0;

    long r = sc2(SYS_CLOCK_ADJTIME, 99, (long)&tx);
    check_val("invalid clock EINVAL", r, -EINVAL);
}

/* ── clock_adjtime02: EFAULT for bad address ── */

static void test_clock_adjtime_efault(void) {
    puts("\n[ltp/clock_adjtime02-efault]\n");

    long r = sc2(SYS_CLOCK_ADJTIME, CLOCK_REALTIME, 0);
    check_val("NULL timex EFAULT", r, -EFAULT);
}

TEST("ltp/clock_adjtime01-read",         test_clock_adjtime_read);
TEST("ltp/clock_adjtime02-einval-clock",  test_clock_adjtime_einval_clock);
TEST("ltp/clock_adjtime02-efault",        test_clock_adjtime_efault);
