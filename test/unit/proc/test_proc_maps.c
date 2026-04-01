/* /proc/pid/maps — Linux-kompatible VMA-Ausgabe */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

static int contains(const char *hay, int len, const char *needle) {
    int nlen = 0;
    while (needle[nlen]) nlen++;
    for (int i = 0; i <= len - nlen; i++) {
        int j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

/* Find line containing needle, copy to out (max outlen). Returns line length. */
static int find_line(const char *buf, int buflen, const char *needle, char *out, int outlen) {
    int nlen = 0;
    while (needle[nlen]) nlen++;
    for (int i = 0; i <= buflen - nlen; i++) {
        int j = 0;
        while (j < nlen && buf[i + j] == needle[j]) j++;
        if (j == nlen) {
            /* Found — walk back to line start */
            int ls = i;
            while (ls > 0 && buf[ls - 1] != '\n') ls--;
            /* Walk forward to line end */
            int le = i;
            while (le < buflen && buf[le] != '\n') le++;
            int ll = le - ls;
            if (ll > outlen - 1) ll = outlen - 1;
            for (int k = 0; k < ll; k++) out[k] = buf[ls + k];
            out[ll] = 0;
            return ll;
        }
    }
    return 0;
}

static long read_maps(char *buf, int bufsize) {
    long fd = sc3(SYS_OPEN, (long)"/proc/self/maps", O_RDONLY, 0);
    if (fd < 0) return fd;
    long total = 0;
    while (total < bufsize - 1) {
        long n = sc3(SYS_READ, fd, (long)(buf + total), bufsize - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = 0;
    sc1(SYS_CLOSE, fd);
    return total;
}

/* ── Basic format ────────────────────────────────── */

static void test_maps_format(void) {
    puts("\n[maps/format]\n");

    char buf[4096];
    long n = read_maps(buf, sizeof(buf));
    check("read > 0", n > 0);
    if (n <= 0) return;

    /* Every line must have: <hex>-<hex> <4 perms chars> 00000000 00:00 */
    check("has address range", contains(buf, (int)n, "-"));
    check("has offset field", contains(buf, (int)n, "00000000"));
    check("has dev field", contains(buf, (int)n, "00:00"));

    /* At least one rw- mapping (stack or data) */
    check("has rw- VMA", contains(buf, (int)n, "rw-"));
    /* At least one executable mapping (r-x or rwx) */
    int has_exec = contains(buf, (int)n, "r-x") || contains(buf, (int)n, "rwx");
    check("has executable VMA", has_exec);
}

/* ── [stack] label ───────────────────────────────── */

static void test_maps_stack_label(void) {
    puts("\n[maps/stack]\n");

    char buf[4096];
    long n = read_maps(buf, sizeof(buf));
    check("read > 0", n > 0);
    if (n <= 0) return;

    check("[stack] present", contains(buf, (int)n, "[stack]"));

    /* Stack line must be rw-p (read-write, private) */
    char line[256];
    int ll = find_line(buf, (int)n, "[stack]", line, sizeof(line));
    check("stack line found", ll > 0);
    if (ll > 0)
        check("stack is rw-p", contains(line, ll, "rw-p"));
}

/* ── [heap] label after brk ──────────────────────── */

static void test_maps_heap_label(void) {
    puts("\n[maps/heap]\n");

    /* Grow brk to create a heap VMA */
    long cur = sc1(SYS_BRK, 0);
    check("brk(0)", cur > 0);
    long grown = sc1(SYS_BRK, cur + 4096);
    check("brk grow", grown >= cur + 4096);

    char buf[4096];
    long n = read_maps(buf, sizeof(buf));
    check("read > 0", n > 0);
    if (n <= 0) return;

    check("[heap] present", contains(buf, (int)n, "[heap]"));

    char line[256];
    int ll = find_line(buf, (int)n, "[heap]", line, sizeof(line));
    if (ll > 0)
        check("heap is rw-p", contains(line, ll, "rw-p"));
}

/* ── Private vs shared flag ──────────────────────── */

static void test_maps_shared_flag(void) {
    puts("\n[maps/shared]\n");

    /* MAP_SHARED anonymous mapping */
    long addr = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", addr > 0);
    if (addr <= 0) return;

    char buf[4096];
    long n = read_maps(buf, sizeof(buf));
    check("read > 0", n > 0);

    /* Should contain an 's' (shared) VMA */
    if (n > 0)
        check("has shared 's' VMA", contains(buf, (int)n, "rw-s"));

    sc2(SYS_MUNMAP, addr, 4096);
}

/* ── mmap/munmap reflected ───────────────────────── */

static void test_maps_mmap_munmap(void) {
    puts("\n[maps/mmap]\n");

    /* Count lines before */
    char buf[4096];
    long n1 = read_maps(buf, sizeof(buf));
    int lines1 = 0;
    for (long i = 0; i < n1; i++) if (buf[i] == '\n') lines1++;

    /* Add mapping */
    long addr = sc6(SYS_MMAP, 0, 0x10000, PROT_READ,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap", addr > 0);
    if (addr <= 0) return;

    long n2 = read_maps(buf, sizeof(buf));
    int lines2 = 0;
    for (long i = 0; i < n2; i++) if (buf[i] == '\n') lines2++;
    check("more VMAs after mmap", lines2 > lines1);

    /* Remove mapping */
    sc2(SYS_MUNMAP, addr, 0x10000);

    long n3 = read_maps(buf, sizeof(buf));
    int lines3 = 0;
    for (long i = 0; i < n3; i++) if (buf[i] == '\n') lines3++;
    check("VMAs restored after munmap", lines3 == lines1);
}

/* ── mprotect changes permissions ────────────────── */

static void test_maps_mprotect(void) {
    puts("\n[maps/mprotect]\n");

    long addr = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap rw", addr > 0);
    if (addr <= 0) return;

    /* Make read-only */
    long r = sc3(SYS_MPROTECT, addr, 4096, PROT_READ);
    check("mprotect r", r == 0);

    char buf[4096];
    long n = read_maps(buf, sizeof(buf));

    /* Find the line containing our address */
    char hex[32];
    int hp = 0;
    uint64_t va = (uint64_t)addr;
    if (va == 0) { hex[hp++] = '0'; }
    else {
        char tmp[16]; int tn = 0;
        while (va) { tmp[tn++] = "0123456789abcdef"[va & 0xf]; va >>= 4; }
        for (int i = tn - 1; i >= 0; i--) hex[hp++] = tmp[i];
    }
    hex[hp] = 0;

    char line[256];
    int ll = find_line(buf, (int)n, hex, line, sizeof(line));
    check("VMA found", ll > 0);
    if (ll > 0)
        check("perms r--p after mprotect", contains(line, ll, "r--p"));

    sc2(SYS_MUNMAP, addr, 4096);
}

/* ── /proc/PID/maps for child process ────────────── */

static void test_maps_other_pid(void) {
    puts("\n[maps/pid]\n");

    /* Shared page for child to signal readiness */
    volatile int *flag = (volatile int *)sc6(SYS_MMAP, 0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    check("mmap shared", (long)flag > 0);
    if ((long)flag <= 0) return;
    *flag = 0;

    void *stack = (void *)sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap stack", (long)stack > 0);
    if ((long)stack <= 0) { sc2(SYS_MUNMAP, (long)flag, 4096); return; }

    long pid = sc5(SYS_CLONE, SIGCHLD, (long)stack + 4096, 0, 0, 0);
    check("clone", pid >= 0);
    if (pid < 0) goto cleanup;

    if (pid == 0) {
        *flag = 1;
        /* Spin until parent reads our maps */
        while (*flag != 2) sc0(SYS_SCHED_YIELD);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    /* Wait for child to be ready */
    while (*flag != 1) sc0(SYS_SCHED_YIELD);

    /* Build /proc/<pid>/maps path */
    char path[64] = "/proc/";
    int pp = 6;
    { /* itoa for pid */
        char tmp[16]; int tn = 0;
        long p = pid;
        do { tmp[tn++] = '0' + (p % 10); p /= 10; } while (p > 0);
        for (int i = tn - 1; i >= 0; i--) path[pp++] = tmp[i];
    }
    path[pp++] = '/'; path[pp++] = 'm'; path[pp++] = 'a';
    path[pp++] = 'p'; path[pp++] = 's'; path[pp] = 0;

    long fd = sc3(SYS_OPEN, (long)path, O_RDONLY, 0);
    check("open /proc/pid/maps", fd >= 0);
    if (fd >= 0) {
        char buf[2048] = {0};
        long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
        check("read child maps", n > 0);
        if (n > 0) {
            buf[n] = 0;
            check("child has [stack]", contains(buf, (int)n, "[stack]"));
            int has_x = contains(buf, (int)n, "r-x") || contains(buf, (int)n, "rwx");
            check("child has exec VMA", has_x);
        }
        sc1(SYS_CLOSE, fd);
    }

    /* Let child exit */
    *flag = 2;
    int wstatus = 0;
    sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);

cleanup:
    sc2(SYS_MUNMAP, (long)stack, 4096);
    sc2(SYS_MUNMAP, (long)flag, 4096);
}

/* ── Stack growth reflected ──────────────────────── */

static void test_maps_stack_growth(void) {
    puts("\n[maps/stack-growth]\n");

    /* Read maps, find [stack] line, parse start address */
    char buf[4096];
    long n = read_maps(buf, sizeof(buf));
    check("read > 0", n > 0);
    if (n <= 0) return;

    char line[256];
    int ll = find_line(buf, (int)n, "[stack]", line, sizeof(line));
    check("stack found", ll > 0);
    if (ll <= 0) return;

    /* Parse start address from line */
    uint64_t start1 = 0;
    for (int i = 0; i < ll && line[i] != '-'; i++) {
        char c = line[i];
        if (c >= '0' && c <= '9') start1 = (start1 << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') start1 = (start1 << 4) | (uint64_t)(c - 'a' + 10);
    }
    check("parsed start > 0", start1 > 0);

    /* Touch stack 128KB below current RSP to force VMA_GROWSDOWN expansion.
     * volatile prevents optimization; negative offset from a stack var
     * accesses unmapped stack pages. */
    volatile char anchor = 0;
    volatile char *deep = &anchor - 0x20000;
    *deep = 42;
    (void)anchor;

    /* Re-read maps */
    n = read_maps(buf, sizeof(buf));
    ll = find_line(buf, (int)n, "[stack]", line, sizeof(line));
    check("stack found after growth", ll > 0);
    if (ll <= 0) return;

    uint64_t start2 = 0;
    for (int i = 0; i < ll && line[i] != '-'; i++) {
        char c = line[i];
        if (c >= '0' && c <= '9') start2 = (start2 << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') start2 = (start2 << 4) | (uint64_t)(c - 'a' + 10);
    }

    /* Stack should have grown (start address moved lower) */
    check("stack grew (start lower)", start2 < start1);
}

TEST("maps/format",       test_maps_format);
TEST("maps/stack",        test_maps_stack_label);
TEST("maps/heap",         test_maps_heap_label);
TEST("maps/shared",       test_maps_shared_flag);
TEST("maps/mmap",         test_maps_mmap_munmap);
TEST("maps/mprotect",     test_maps_mprotect);
TEST("maps/pid",          test_maps_other_pid);
TEST("maps/stack-growth", test_maps_stack_growth);
