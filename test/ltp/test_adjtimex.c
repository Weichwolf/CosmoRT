/* LTP adjtimex01-03 — ported to ktest */
#include "ktest.h"

/*
 * struct timex layout for x86_64 Linux ABI.
 * Must match kernel's struct __kernel_timex exactly.
 */
struct k_timex {
    unsigned int modes;
    long         offset;
    long         freq;
    long         maxerror;
    long         esterror;
    int          status;
    long         constant;
    long         precision;
    long         tolerance;
    struct k_timeval time;
    long         tick;
    long         ppsfreq;
    long         jitter;
    int          shift;
    long         stabil;
    long         jitcnt;
    long         calcnt;
    long         errcnt;
    long         stbcnt;
    int          tai;
    int          __padding[11];
};

#define ADJ_OFFSET          0x0001
#define ADJ_FREQUENCY       0x0002
#define ADJ_MAXERROR        0x0004
#define ADJ_ESTERROR        0x0008
#define ADJ_STATUS          0x0010
#define ADJ_TIMECONST       0x0020
#define ADJ_TICK            0x4000
#define ADJ_OFFSET_SINGLESHOT 0x8001
#define ADJ_ADJTIME         0x8000

#define SET_MODE (ADJ_OFFSET | ADJ_FREQUENCY | ADJ_MAXERROR | ADJ_ESTERROR | \
                  ADJ_STATUS | ADJ_TIMECONST | ADJ_TICK)

static void memzero(void *p, int n) {
    char *c = (char *)p;
    for (int i = 0; i < n; i++) c[i] = 0;
}

/* ── adjtimex01: basic SET_MODE + ADJ_OFFSET_SINGLESHOT succeed ── */

static void test_adjtimex01(void) {
    puts("\n[ltp/adjtimex01]\n");

    struct k_timex saved;
    memzero(&saved, sizeof(saved));
    saved.modes = 0;

    long r = sc1(SYS_ADJTIMEX, (long)&saved);
    check_ge("adjtimex read", r, 0);

    /* SET_MODE with saved values */
    struct k_timex buf = saved;
    buf.modes = SET_MODE;
    r = sc1(SYS_ADJTIMEX, (long)&buf);
    check_ge("adjtimex SET_MODE", r, 0);

    /* ADJ_OFFSET_SINGLESHOT */
    buf = saved;
    buf.modes = ADJ_OFFSET_SINGLESHOT;
    r = sc1(SYS_ADJTIMEX, (long)&buf);
    check_ge("adjtimex SINGLESHOT", r, 0);
}

/* ── adjtimex02: EINVAL for bad tick values ── */

static void test_adjtimex02_tick_low(void) {
    puts("\n[ltp/adjtimex02-tick-low]\n");

    struct k_timex buf;
    memzero(&buf, sizeof(buf));
    buf.modes = 0;
    long r = sc1(SYS_ADJTIMEX, (long)&buf);
    if (r < 0) { fail("adjtimex read", "cannot read"); return; }

    buf.modes = ADJ_TICK;
    buf.tick = 0; /* way below valid range (900000/HZ .. 1100000/HZ) */
    r = sc1(SYS_ADJTIMEX, (long)&buf);
    check_val("tick too low EINVAL", r, -EINVAL);
}

static void test_adjtimex02_tick_high(void) {
    puts("\n[ltp/adjtimex02-tick-high]\n");

    struct k_timex buf;
    memzero(&buf, sizeof(buf));
    buf.modes = 0;
    long r = sc1(SYS_ADJTIMEX, (long)&buf);
    if (r < 0) { fail("adjtimex read", "cannot read"); return; }

    buf.modes = ADJ_TICK;
    buf.tick = 99999999; /* way above valid range */
    r = sc1(SYS_ADJTIMEX, (long)&buf);
    check_val("tick too high EINVAL", r, -EINVAL);
}

/* ── adjtimex02: EFAULT for bad pointer ── */

static void test_adjtimex02_efault(void) {
    puts("\n[ltp/adjtimex02-efault]\n");

    long r = sc1(SYS_ADJTIMEX, 0);
    check_val("NULL timex EFAULT", r, -EFAULT);
}

/* ── adjtimex03: invalid mode returns EINVAL, tai stays zero (CVE-2018-11508) ── */

static void test_adjtimex03(void) {
    puts("\n[ltp/adjtimex03]\n");

    struct k_timex buf;
    memzero(&buf, sizeof(buf));
    buf.modes = ADJ_ADJTIME; /* invalid mode 0x8000 */

    long r = sc1(SYS_ADJTIMEX, (long)&buf);
    check_val("invalid mode EINVAL", r, -EINVAL);

    /* after EINVAL, tai should still be 0 (no data leak) */
    check_val("tai is zero (no leak)", (long)buf.tai, 0);
}

TEST("ltp/adjtimex01",           test_adjtimex01);
TEST("ltp/adjtimex02-tick-low",  test_adjtimex02_tick_low);
TEST("ltp/adjtimex02-tick-high", test_adjtimex02_tick_high);
TEST("ltp/adjtimex02-efault",    test_adjtimex02_efault);
TEST("ltp/adjtimex03",           test_adjtimex03);
