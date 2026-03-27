#include "ktest.h"

/* ── prctl tests ─────────────────────────────── */

static void test_prctl(void) {
    puts("\n[prctl]\n");

    /* PR_SET_NAME / PR_GET_NAME roundtrip */
    const char name[] = "test_thread";
    long r = sc5(SYS_PRCTL, PR_SET_NAME, (long)name, 0, 0, 0);
    check_val("prctl PR_SET_NAME", r, 0);

    char got[16] = {0};
    r = sc5(SYS_PRCTL, PR_GET_NAME, (long)got, 0, 0, 0);
    check_val("prctl PR_GET_NAME", r, 0);
    /* Compare first 11 chars */
    int match = 1;
    for (int i = 0; i < 11; i++)
        if (got[i] != name[i]) { match = 0; break; }
    check("PR_GET_NAME matches", match);

    /* PR_SET_PDEATHSIG — no-op, should return 0 */
    r = sc5(SYS_PRCTL, PR_SET_PDEATHSIG, 0, 0, 0, 0);
    check_val("prctl PR_SET_PDEATHSIG", r, 0);

    /* Invalid option → -EINVAL */
    r = sc5(SYS_PRCTL, 9999, 0, 0, 0, 0);
    check_val("prctl invalid → EINVAL", r, -EINVAL);
}

TEST("prctl", test_prctl);
