#include "ktest.h"

/* ── sigaltstack ─────────────────────────────── */

#define SS_DISABLE 2

struct test_stack_t {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  _pad;
    uint64_t ss_size;
};

static void test_sigaltstack(void) {
    puts("\n[sigaltstack]\n");

    /* Get old stack */
    struct test_stack_t oss;
    long r = sc2(SYS_SIGALTSTACK, 0, (long)&oss);
    check_val("sigaltstack get returns 0", r, 0);

    /* Set alternate stack */
    long alt = sc6(SYS_MMAP, 0, 16384, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap altstack", alt > 0);
    if (alt <= 0) return;

    struct test_stack_t ss;
    ss.ss_sp = (uint64_t)alt;
    ss.ss_flags = 0;
    ss._pad = 0;
    ss.ss_size = 16384;
    r = sc2(SYS_SIGALTSTACK, (long)&ss, (long)&oss);
    check_val("sigaltstack set returns 0", r, 0);

    /* Verify */
    struct test_stack_t oss2;
    r = sc2(SYS_SIGALTSTACK, 0, (long)&oss2);
    check_val("sigaltstack verify returns 0", r, 0);
    check("ss_sp matches", oss2.ss_sp == (uint64_t)alt);
    check("ss_size matches", oss2.ss_size == 16384);

    /* Disable */
    struct test_stack_t dis;
    dis.ss_sp = 0;
    dis.ss_flags = SS_DISABLE;
    dis._pad = 0;
    dis.ss_size = 0;
    r = sc2(SYS_SIGALTSTACK, (long)&dis, 0);
    check_val("sigaltstack disable", r, 0);

    /* Verify disabled */
    r = sc2(SYS_SIGALTSTACK, 0, (long)&oss2);
    check("disabled ss_flags", oss2.ss_flags == SS_DISABLE);

    sc2(SYS_MUNMAP, alt, 16384);
}

TEST("sigaltstack", test_sigaltstack);
