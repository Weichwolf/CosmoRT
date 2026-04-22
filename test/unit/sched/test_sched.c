#include "ktest.h"

static void test_yield(void) {
    puts("\n[Scheduler]\n");
    long r = sc0(SYS_SCHED_YIELD);
    check_val("sched_yield returns 0", r, 0);
}

/* Linux-Semantik: sched_getaffinity liefert Maske der verfuegbaren CPUs.
 * musl sysconf(_SC_NPROCESSORS_CONF/_ONLN) popcount'et die Maske — ~0ULL
 * wuerde 64 Phantom-CPUs melden und threaded LTP-Tests (fcntl34 etc.)
 * 32-Thread-Stress starten. */
static void test_sched_getaffinity_mask(void) {
    puts("\n[SCHED_GETAFFINITY_MASK]\n");

    uint64_t mask = 0;
    long r = sc3(SYS_SCHED_GETAFFINITY, 0, sizeof(mask), (long)&mask);
    check("sched_getaffinity >= 0", r > 0);

    int nbits = 0;
    for (uint64_t m = mask; m; m &= m - 1) nbits++;
    check("mask has at least 1 CPU", nbits >= 1);
    check("mask reflects actual core count, not ~0ULL", nbits < 64);
}

TEST("sched", test_yield);
TEST("sched_getaffinity_mask", test_sched_getaffinity_mask);
