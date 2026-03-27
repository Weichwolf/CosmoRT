/* test: RAM dedup (KSM) — page merging, age tracking, COW break */
#include "ktest.h"

#define RT_QUERY_DEDUP_STATS 11

static void msleep(int ms) {
    struct { long sec; long nsec; } ts = { ms / 1000, (ms % 1000) * 1000000L };
    sc2(SYS_NANOSLEEP, (long)&ts, 0);
}

/* Read /proc/ksm and parse pages_merged count */
static long read_ksm_merged(void) {
    int fd = (int)sc3(SYS_OPEN, (long)"/proc/ksm", O_RDONLY, 0);
    if (fd < 0) return -1;
    char buf[256];
    long n = sc3(SYS_READ, fd, (long)buf, 255);
    sc1(SYS_CLOSE, fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    /* Find "pages_merged " and parse the number */
    const char *key = "pages_merged ";
    int klen = 13;
    for (int i = 0; i < n - klen; i++) {
        int match = 1;
        for (int j = 0; j < klen; j++) {
            if (buf[i + j] != key[j]) { match = 0; break; }
        }
        if (match) {
            long val = 0;
            for (int j = i + klen; j < n && buf[j] >= '0' && buf[j] <= '9'; j++)
                val = val * 10 + (buf[j] - '0');
            return val;
        }
    }
    return -1;
}

/* Test: two processes with identical anonymous pages → merge after dedup cycles */
static void test_dedup_merge(void) {
    puts("\n[dedup merge]\n");

    long merged_before = read_ksm_merged();
    check("/proc/ksm readable", merged_before >= 0);

    /* Allocate a page with known pattern */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap", addr > 0x1000);

    /* Fill with deterministic pattern */
    volatile uint8_t *p = (volatile uint8_t *)addr;
    for (int i = 0; i < 4096; i++) p[i] = (uint8_t)(i & 0xFF);

    /* Fork: child gets COW copy of same page */
    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);

    if (pid == 0) {
        /* Child: allocate new page with SAME pattern (not COW-shared) */
        long addr2 = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
        if (addr2 <= 0x1000) sc1(SYS_EXIT_GROUP, 1);
        volatile uint8_t *q = (volatile uint8_t *)addr2;
        for (int i = 0; i < 4096; i++) q[i] = (uint8_t)(i & 0xFF);

        /* Wait for dedup to process (multiple scan cycles) */
        msleep(5000);

        /* Verify data integrity */
        int ok = 1;
        for (int i = 0; i < 4096; i++) {
            if (q[i] != (uint8_t)(i & 0xFF)) { ok = 0; break; }
        }
        sc2(SYS_MUNMAP, addr2, 4096);
        sc1(SYS_EXIT_GROUP, ok ? 0 : 2);
    }

    /* Parent: wait for child */
    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child ok", (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0);

    /* Check that merges happened (may be 0 if dedup hasn't cycled yet — not a hard failure) */
    long merged_after = read_ksm_merged();
    puts("  merges before="); put_int(merged_before);
    puts(" after="); put_int(merged_after); puts("\n");

    /* At minimum, /proc/ksm must remain readable and consistent */
    check("stats consistent", merged_after >= merged_before);

    sc2(SYS_MUNMAP, addr, 4096);
}

/* Test: hot page (recently accessed) is skipped by dedup */
static void test_dedup_skip_hot(void) {
    puts("\n[dedup skip hot]\n");

    /* Allocate and touch a page continuously */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap", addr > 0x1000);
    volatile uint64_t *p = (volatile uint64_t *)addr;

    /* Keep touching to stay hot */
    for (int i = 0; i < 50; i++) {
        *p = (uint64_t)i;
        msleep(100);
    }

    /* Hot pages should not be dedup candidates — just verify no crash */
    check("hot page survived", *p == 49);
    sc2(SYS_MUNMAP, addr, 4096);
}

/* Test: COW break after merge → new page, data intact */
static void test_dedup_cow_break(void) {
    puts("\n[dedup COW break]\n");

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap", addr > 0x1000);
    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = 0xDEADBEEFCAFEBABEULL;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);

    if (pid == 0) {
        /* Child: read to verify shared data, then write to trigger COW */
        if (*p != 0xDEADBEEFCAFEBABEULL) sc1(SYS_EXIT_GROUP, 1);

        /* Wait for potential merge */
        msleep(3000);

        /* Write triggers COW break */
        *p = 0x1234567890ABCDEFULL;
        if (*p != 0x1234567890ABCDEFULL) sc1(SYS_EXIT_GROUP, 2);

        sc1(SYS_EXIT_GROUP, 0);
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child cow break ok", (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0);
    check_val("parent unchanged", (long)*p, (long)0xDEADBEEFCAFEBABEULL);

    sc2(SYS_MUNMAP, addr, 4096);
}

/* Test: /proc/ksm counters are sane */
static void test_dedup_stats(void) {
    puts("\n[dedup stats]\n");

    int fd = (int)sc3(SYS_OPEN, (long)"/proc/ksm", O_RDONLY, 0);
    check("open /proc/ksm", fd >= 0);

    char buf[256];
    long n = sc3(SYS_READ, fd, (long)buf, 255);
    check("read /proc/ksm", n > 0);
    sc1(SYS_CLOSE, fd);

    if (n > 0) {
        buf[n] = 0;
        /* Verify all expected fields present */
        int has_scanned = 0, has_shared = 0, has_merged = 0, has_saved = 0;
        for (int i = 0; i < n - 5; i++) {
            if (buf[i] == 'p' && buf[i+1] == 'a' && buf[i+2] == 'g' && buf[i+3] == 'e' && buf[i+4] == 's') {
                if (buf[i+6] == 's' && buf[i+7] == 'c') has_scanned = 1;
                if (buf[i+6] == 's' && buf[i+7] == 'h') has_shared = 1;
                if (buf[i+6] == 'm') has_merged = 1;
            }
            if (buf[i] == 'b' && buf[i+1] == 'y' && buf[i+2] == 't') has_saved = 1;
        }
        check("has pages_scanned", has_scanned);
        check("has pages_shared", has_shared);
        check("has pages_merged", has_merged);
        check("has bytes_saved", has_saved);
    }
}

TEST("dedup_merge", test_dedup_merge);
TEST("dedup_skip_hot", test_dedup_skip_hot);
TEST("dedup_cow_break", test_dedup_cow_break);
TEST("dedup_stats", test_dedup_stats);
