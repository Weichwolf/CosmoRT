#include "ktest.h"

static void test_vm_patterns(void) {
    puts("\n[VM Patterns]\n");

    /* 1. Large PROT_NONE reservation + selective mprotect */
    uint64_t big = (uint64_t)sc6(SYS_mmap, 0, 128*1024*1024, PROT_NONE,
                                  MAP_PRIV_ANON, -1, 0);
    check("mmap 128MB PROT_NONE", big > 0x1000);

    /* mprotect first 64KB to RW */
    long mp = sc3(SYS_mprotect, (long)big, 65536, PROT_RW);
    check("mprotect 64KB RW", mp == 0);

    /* Write to activated region */
    *(volatile uint64_t *)big = 0xDEADBEEF;
    check("write to mprotect'd region", *(volatile uint64_t *)big == 0xDEADBEEF);

    /* munmap the whole reservation */
    long mu = sc2(SYS_munmap, (long)big, 128*1024*1024);
    check("munmap 128MB", mu == 0);

    /* 2. Overallocate + trim (alignment pattern) */
    uint64_t over = (uint64_t)sc6(SYS_mmap, 0, 256*1024*1024, PROT_NONE,
                                   MAP_PRIV_ANON, -1, 0);
    check("mmap 256MB for trim", over > 0x1000);

    /* Find 64MB-aligned boundary within allocation */
    uint64_t aligned = (over + 0x3FFFFFFULL) & ~0x3FFFFFFULL;
    /* Trim below */
    if (aligned > over)
        sc2(SYS_munmap, (long)over, aligned - over);
    /* Trim above: keep 64MB from aligned */
    uint64_t keep_end = aligned + 64*1024*1024;
    uint64_t alloc_end = over + 256*1024*1024;
    if (keep_end < alloc_end)
        sc2(SYS_munmap, (long)keep_end, alloc_end - keep_end);

    /* Activate and use the aligned region */
    mp = sc3(SYS_mprotect, (long)aligned, 4096, PROT_RW);
    check("mprotect aligned region", mp == 0);
    *(volatile uint64_t *)aligned = 0xCAFE;
    check("write to aligned region", *(volatile uint64_t *)aligned == 0xCAFE);
    sc2(SYS_munmap, (long)aligned, 64*1024*1024);

    /* 3. mprotect RW → RX + execute (JIT pattern) */
    uint64_t code = (uint64_t)sc6(SYS_mmap, 0, 4096, PROT_RW,
                                   MAP_PRIV_ANON, -1, 0);
    check("mmap code page RW", code > 0x1000);
    /* Write: mov eax, 42; ret */
    uint8_t *p = (uint8_t *)code;
    p[0] = 0xb8; p[1] = 42; p[2] = 0; p[3] = 0; p[4] = 0; /* mov eax, 42 */
    p[5] = 0xc3; /* ret */
    mp = sc3(SYS_mprotect, (long)code, 4096, PROT_READ | PROT_EXEC);
    check("mprotect RW→RX", mp == 0);
    /* Execute JIT code */
    int (*fn)(void) = (int (*)(void))code;
    int result = fn();
    check("JIT exec returns 42", result == 42);
    sc2(SYS_munmap, (long)code, 4096);

    /* 4. MAP_FIXED_NOREPLACE */
    uint64_t base = (uint64_t)sc6(SYS_mmap, 0, 4096, PROT_RW,
                                   MAP_PRIV_ANON, -1, 0);
    check("mmap base for NOREPLACE", base > 0x1000);
    /* Try to map on top — should fail with EEXIST */
    long dup = sc6(SYS_mmap, (long)base, 4096, PROT_RW,
                   MAP_FIXED_NOREPLACE | MAP_PRIV_ANON, -1, 0);
    check("MAP_FIXED_NOREPLACE → EEXIST", dup < 0);
    sc2(SYS_munmap, (long)base, 4096);

    /* 5. SIGABRT terminates (abort() pattern) */
    /* Can't test self-kill here — would terminate ktest.
     * Just verify kill syscall exists. */
    long kr = sc2(SYS_kill, sc0(SYS_getpid), 0); /* sig=0: check only */
    check("kill(self, 0) succeeds", kr == 0);
}

TEST("vm_patterns", test_vm_patterns);
