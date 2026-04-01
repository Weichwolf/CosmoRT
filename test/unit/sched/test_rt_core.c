#include "ktest.h"
#include "cosmort.h"

static void test_rt_query(void) {
    puts("\n[RT Query]\n");

    /* RT-Core concept removed — subcases 0 and 1 return -EINVAL */
    long q0 = sc2(SYS_COSMO_RT_QUERY, 0, 0);
    check_val("subcase 0 returns -EINVAL", q0, -22);

    long q1 = sc2(SYS_COSMO_RT_QUERY, 1, 0);
    check_val("subcase 1 returns -EINVAL", q1, -22);

    /* Invalid subcommand still returns -EINVAL */
    long inval = sc2(SYS_COSMO_RT_QUERY, 99, 0);
    check_val("invalid query returns -EINVAL", inval, -22);
}

TEST("rt_query", test_rt_query);
