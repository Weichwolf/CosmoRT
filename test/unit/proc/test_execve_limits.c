/* Class A: execve argv/envp Limits.
 *
 * EXECVE_MAX_ARGS=4096, EXECVE_BUF_SIZE=128KB (ARG_MAX).  */
#include "ktest.h"

#define LARGE_ARG_COUNT     4100
#define MED_ARG_COUNT       1000

#define ARG_MAX_BYTES       (128 * 1024)

static void test_execve_too_many_args(void) {
    puts("\n[EXECVE_TOO_MANY_ARGS]\n");

    long addrs = sc6(SYS_MMAP, 0,
                     (long)(LARGE_ARG_COUNT + 2) * (long)sizeof(char *),
                     PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("argv region", addrs > 0);
    if (addrs <= 0) return;

    char **argv = (char **)addrs;
    for (int i = 0; i < LARGE_ARG_COUNT; i++) argv[i] = (char *)"x";
    argv[LARGE_ARG_COUNT] = 0;

    long r = sc3(SYS_EXECVE, (long)"/nonexistent_execve_test",
                 (long)argv, 0);
    check_val("execve argc=4100 → -E2BIG", r, -E2BIG);

    sc2(SYS_MUNMAP, addrs,
        (long)(LARGE_ARG_COUNT + 2) * (long)sizeof(char *));
}

static void test_execve_arg_strlen_oversize(void) {
    puts("\n[EXECVE_ARG_STRLEN]\n");

    long buf = sc6(SYS_MMAP, 0, ARG_MAX_BYTES + 8192, PROT_RW,
                   MAP_PRIV_ANON, -1, 0);
    check("big arg buffer", buf > 0);
    if (buf <= 0) return;

    char *big = (char *)buf;
    for (int i = 0; i < ARG_MAX_BYTES + 4096; i++) big[i] = 'A';
    big[ARG_MAX_BYTES + 4096] = 0;

    char *argv[2];
    argv[0] = big;
    argv[1] = 0;

    long r = sc3(SYS_EXECVE, (long)"/nonexistent_execve_test_2",
                 (long)argv, 0);
    check_val("execve with oversize arg → -E2BIG", r, -E2BIG);

    sc2(SYS_MUNMAP, buf, ARG_MAX_BYTES + 8192);
}

static void test_execve_env_too_large(void) {
    puts("\n[EXECVE_ENV_TOO_LARGE]\n");

    long reg = sc6(SYS_MMAP, 0, 256 * 1024, PROT_RW,
                   MAP_PRIV_ANON, -1, 0);
    check("env region", reg > 0);
    if (reg <= 0) return;

    long ptrs = sc6(SYS_MMAP, 0,
                    (long)(MED_ARG_COUNT + 2) * (long)sizeof(char *),
                    PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (ptrs <= 0) { sc2(SYS_MUNMAP, reg, 256 * 1024); check("ptrs", 0); return; }

    char *envp[2];
    char *envbuf = (char *)reg;
    for (int i = 0; i < 256 * 1024 - 1; i++) envbuf[i] = 'B';
    envbuf[256 * 1024 - 1] = 0;

    envp[0] = envbuf;
    envp[1] = 0;

    char *argv[2]; argv[0] = (char *)"/nonexistent_execve_test_3"; argv[1] = 0;
    long r = sc3(SYS_EXECVE, (long)"/nonexistent_execve_test_3",
                 (long)argv, (long)envp);
    check_val("execve envp 256KB → -E2BIG", r, -E2BIG);

    sc2(SYS_MUNMAP, reg, 256 * 1024);
    sc2(SYS_MUNMAP, ptrs,
        (long)(MED_ARG_COUNT + 2) * (long)sizeof(char *));
}

TEST("execve/too_many_args",    test_execve_too_many_args);
TEST("execve/arg_strlen",       test_execve_arg_strlen_oversize);
TEST("execve/env_too_large",    test_execve_env_too_large);
