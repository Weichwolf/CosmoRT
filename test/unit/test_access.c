#include "ktest.h"

static void test_access(void) {
    puts("\n[Access]\n");

    /* /proc/dmesg exists (procfs) */
    long r = sc2(SYS_ACCESS, (long)"/proc/dmesg", 0);
    check_val("access /proc/dmesg", r, 0);

    /* /nonexistent → -ENOENT (2) */
    r = sc2(SYS_ACCESS, (long)"/nonexistent", 0);
    check_val("access /nonexistent → -ENOENT", r, -2);

    /* /dev/null exists */
    r = sc2(SYS_ACCESS, (long)"/dev/null", 0);
    check_val("access /dev/null", r, 0);

    /* /dev exists */
    r = sc2(SYS_ACCESS, (long)"/dev", 0);
    check_val("access /dev", r, 0);
}

TEST("access", test_access);
