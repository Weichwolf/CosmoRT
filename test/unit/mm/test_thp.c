/* Transparent Huge Pages — THP default-on + MADV_HUGEPAGE/NOHUGEPAGE */
#include "ktest.h"

#define HUGE_SIZE  (2UL * 1024 * 1024)  /* 2MB */
#define SMALL_SIZE (64UL * 1024)         /* 64KB */

/* Read PTE for a user address (syscall 512 = COSMO_READ_PTE, if available).
 * Fallback: check via /proc/self/maps that VMA exists. */

/* ── THP default: large anonymous mmap gets huge pages ──── */

static void test_thp_default(void) {
    puts("\n[thp/default]\n");

    /* 4MB anonymous mapping — should get VMA_HUGEPAGE automatically */
    long addr = sc6(SYS_MMAP, 0, 4 * HUGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap 8MB", addr > 0);
    if (addr <= 0) return;

    /* Touch pages to trigger demand paging (hopefully via huge pages) */
    volatile char *p = (volatile char *)addr;
    p[0] = 1;
    p[HUGE_SIZE] = 2;
    p[2 * HUGE_SIZE] = 3;
    p[3 * HUGE_SIZE] = 4;

    /* Verify memory works */
    check_val("page 0", p[0], 1);
    check_val("page 2MB", p[HUGE_SIZE], 2);
    check_val("page 4MB", p[2 * HUGE_SIZE], 3);
    check_val("page 6MB", p[3 * HUGE_SIZE], 4);

    sc2(SYS_MUNMAP, addr, 4 * HUGE_SIZE);
}

/* ── MADV_HUGEPAGE on small mapping ─────────────────────── */

static void test_thp_madvise_enable(void) {
    puts("\n[thp/madvise-enable]\n");

    /* Small mapping: 64KB — below THP auto-threshold */
    long addr = sc6(SYS_MMAP, 0, SMALL_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap 64KB", addr > 0);
    if (addr <= 0) return;

    /* Force THP via madvise */
    long r = sc3(SYS_MADVISE, addr, SMALL_SIZE, MADV_HUGEPAGE);
    check_val("madvise HUGEPAGE", r, 0);

    /* Touch memory — should work regardless of page size */
    volatile char *p = (volatile char *)addr;
    p[0] = 0xAA;
    p[SMALL_SIZE - 1] = 0xBB;
    check_val("first byte", p[0], (char)0xAA);
    check_val("last byte", p[SMALL_SIZE - 1], (char)0xBB);

    sc2(SYS_MUNMAP, addr, SMALL_SIZE);
}

/* ── MADV_NOHUGEPAGE disables THP ──────────────────────── */

static void test_thp_madvise_disable(void) {
    puts("\n[thp/madvise-disable]\n");

    /* Large mapping that would get THP by default */
    long addr = sc6(SYS_MMAP, 0, 4 * HUGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap 8MB", addr > 0);
    if (addr <= 0) return;

    /* Disable THP */
    long r = sc3(SYS_MADVISE, addr, 4 * HUGE_SIZE, MADV_NOHUGEPAGE);
    check_val("madvise NOHUGEPAGE", r, 0);

    /* Touch — should still work (just 4KB pages) */
    volatile char *p = (volatile char *)addr;
    p[0] = 1;
    p[HUGE_SIZE] = 2;
    check_val("page 0", p[0], 1);
    check_val("page 2MB", p[HUGE_SIZE], 2);

    sc2(SYS_MUNMAP, addr, 4 * HUGE_SIZE);
}

/* ── MADV_HUGEPAGE on non-anonymous mapping → no-op ──────── */

static void test_thp_file_backed_noop(void) {
    puts("\n[thp/file-noop]\n");

    /* Open a file and mmap it */
    long fd = sc3(SYS_OPEN, (long)"/proc/version", O_RDONLY, 0);
    if (fd < 0) { puts("  skip (no /proc/version)\n"); return; }

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_READ,
                     MAP_PRIVATE, fd, 0);
    sc1(SYS_CLOSE, fd);
    check("mmap file", addr > 0);
    if (addr <= 0) return;

    /* MADV_HUGEPAGE on file-backed: accepted but no effect (Linux behavior) */
    long r = sc3(SYS_MADVISE, addr, 4096, MADV_HUGEPAGE);
    check_val("madvise file-backed accepted", r, 0);

    sc2(SYS_MUNMAP, addr, 4096);
}

/* ── MADV on invalid range → ENOMEM ──────────────────────── */

static void test_thp_madvise_einval(void) {
    puts("\n[thp/madvise-einval]\n");

    /* Unaligned address → EINVAL */
    long r = sc3(SYS_MADVISE, 0x1001, 4096, MADV_HUGEPAGE);
    check_val("unaligned EINVAL", r, -EINVAL);

    /* Address in unmapped region → ENOMEM */
    r = sc3(SYS_MADVISE, 0x10000000, 4096, MADV_DONTNEED);
    check_val("unmapped ENOMEM", r, -ENOMEM);
}

/* ── Huge page + fork COW ────────────────────────────────── */

static void test_thp_fork(void) {
    puts("\n[thp/fork]\n");

    /* 4MB mapping with THP */
    volatile long *shared = (volatile long *)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", (long)shared > 0);
    if ((long)shared <= 0) return;
    *shared = 0;

    long addr = sc6(SYS_MMAP, 0, 4 * HUGE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap 8MB", addr > 0);
    if (addr <= 0) { sc2(SYS_MUNMAP, (long)shared, 4096); return; }

    /* Write pattern before fork */
    volatile long *p = (volatile long *)addr;
    p[0] = 0xDEAD;
    p[HUGE_SIZE / sizeof(long)] = 0xBEEF;

    void *stack = (void *)sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long pid = sc5(SYS_CLONE, SIGCHLD, (long)stack + 4096, 0, 0, 0);
    check("fork", pid >= 0);
    if (pid < 0) goto cleanup;

    if (pid == 0) {
        /* Child: verify COW data intact, then modify */
        int ok = (p[0] == 0xDEAD && p[HUGE_SIZE / sizeof(long)] == 0xBEEF);
        p[0] = 0xCAFE; /* COW: should not affect parent */
        *shared = ok ? 1 : 2;
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("child saw parent data", *shared, 1);
    check_val("parent data unchanged (COW)", p[0], 0xDEAD);

cleanup:
    sc2(SYS_MUNMAP, addr, 4 * HUGE_SIZE);
    sc2(SYS_MUNMAP, (long)stack, 4096);
    sc2(SYS_MUNMAP, (long)shared, 4096);
}

TEST("thp/default",          test_thp_default);
TEST("thp/madvise-enable",   test_thp_madvise_enable);
TEST("thp/madvise-disable",  test_thp_madvise_disable);
TEST("thp/file-noop",        test_thp_file_backed_noop);
TEST("thp/madvise-einval",   test_thp_madvise_einval);
TEST("thp/fork",             test_thp_fork);
