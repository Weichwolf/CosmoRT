#include "ktest.h"
#include "cosmo_rt.h"

static void test_rt_core(void) {
    puts("\n[RT Core]\n");

    /* ktest runs on a Compute-Core (not RT-Core 0) */
    long is_rt = sc2(SYS_COSMO_RT_QUERY, 0, 0);
    check_val("compute core is not RT", is_rt, 0);

    /* RT-Core 0 has physical core ID 0 */
    long core0 = sc2(SYS_COSMO_RT_QUERY, 1, 0);
    check_val("rt_core_id(0) == 0", core0, 0);

    /* Invalid index returns -1 */
    long bad = sc2(SYS_COSMO_RT_QUERY, 1, 1);
    check_val("rt_core_id(1) == -1", bad, -1);

    /* Invalid subcommand */
    long inval = sc2(SYS_COSMO_RT_QUERY, 99, 0);
    check_val("invalid query returns -EINVAL", inval, -22);
}

TEST("rt_core", test_rt_core);
