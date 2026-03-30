#include "ktest.h"

#define PAGE 4096

static void test_tlb_shootdown(void) {
    puts("\n[TLB Shootdown]\n");

    /* ── 1. mmap + munmap: page no longer accessible ── */
    long pg = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap ok", pg > 0);
    *(volatile char *)pg = 'X';
    long r = sc2(SYS_MUNMAP, pg, PAGE);
    check_val("munmap ok", r, 0);

    /* ── 2. mprotect PROT_NONE then back to PROT_RW ── */
    pg = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap for mprotect", pg > 0);
    *(volatile char *)pg = 'A';
    r = sc3(SYS_MPROTECT, pg, PAGE, PROT_NONE);
    check_val("mprotect NONE ok", r, 0);
    r = sc3(SYS_MPROTECT, pg, PAGE, PROT_READ);
    check_val("mprotect READ ok", r, 0);
    check_val("data preserved", *(volatile char *)pg, 'A');
    sc2(SYS_MUNMAP, pg, PAGE);

    /* ── 3. COW after fork: parent write doesn't affect child ── */
    pg = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    *(volatile int *)pg = 100;
    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Child reads COW page */
        int val = *(volatile int *)pg;
        sc2(SYS_MUNMAP, pg, PAGE);
        sc1(SYS_EXIT, (val == 100) ? 1 : 0);
        __builtin_unreachable();
    }
    /* Parent writes (triggers COW) */
    *(volatile int *)pg = 200;
    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("cow child saw original", (ws >> 8) & 0xFF, 1);
    check_val("cow parent has new value", *(volatile int *)pg, 200);
    sc2(SYS_MUNMAP, pg, PAGE);

    /* ── 4. Shared page visible across fork ── */
    long sh = sc6(SYS_MMAP, 0, PAGE, PROT_RW,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *(volatile int *)sh = 0;
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        *(volatile int *)sh = 77;
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("shared page write visible", *(volatile int *)sh, 77);
    sc2(SYS_MUNMAP, sh, PAGE);

    /* ── 5. Multi-page mprotect ── */
    long mp = sc6(SYS_MMAP, 0, 4 * PAGE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("multi-page mmap", mp > 0);
    for (int i = 0; i < 4; i++)
        *(volatile char *)(mp + i * PAGE) = (char)('0' + i);
    r = sc3(SYS_MPROTECT, mp, 4 * PAGE, PROT_READ);
    check_val("multi-page mprotect ok", r, 0);
    int data_ok = 1;
    for (int i = 0; i < 4; i++)
        if (*(volatile char *)(mp + i * PAGE) != (char)('0' + i)) data_ok = 0;
    check("multi-page data preserved", data_ok);
    sc2(SYS_MUNMAP, mp, 4 * PAGE);

    /* ── 6. munmap partial range ── */
    long big = sc6(SYS_MMAP, 0, 3 * PAGE, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("3-page mmap", big > 0);
    *(volatile char *)(big) = 'H';
    *(volatile char *)(big + 2 * PAGE) = 'T';
    r = sc2(SYS_MUNMAP, big + PAGE, PAGE);  /* unmap middle */
    check_val("partial munmap ok", r, 0);
    check_val("first page intact", *(volatile char *)big, 'H');
    check_val("last page intact", *(volatile char *)(big + 2 * PAGE), 'T');
    sc2(SYS_MUNMAP, big, PAGE);
    sc2(SYS_MUNMAP, big + 2 * PAGE, PAGE);

    /* ── 7. Rapid mmap/munmap cycle (TLB stress) ── */
    for (int i = 0; i < 32; i++) {
        long tmp = sc6(SYS_MMAP, 0, PAGE, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (tmp > 0) {
            *(volatile char *)tmp = (char)i;
            sc2(SYS_MUNMAP, tmp, PAGE);
        }
    }
    check("32x mmap/munmap cycle ok", 1);
}

TEST("tlb_shootdown", test_tlb_shootdown);
