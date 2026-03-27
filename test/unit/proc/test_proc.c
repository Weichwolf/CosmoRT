/* /proc entry tests — one TEST per entry */
#include "ktest.h"

static int contains(const char *hay, int haylen, const char *needle) {
    int nlen = 0;
    while (needle[nlen]) nlen++;
    for (int i = 0; i <= haylen - nlen; i++) {
        int j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static long parse_long(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* ── /proc/version ─────────────────────────────── */

static void test_proc_version(void) {
    puts("\n[/proc/version]\n");
    char buf[256];
    long fd = sc3(SYS_OPEN, (long)"/proc/version", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("contains CosmoRT", contains(buf, (int)n, "CosmoRT")); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_version", test_proc_version);

/* ── /proc/uptime ──────────────────────────────── */

static void test_proc_uptime(void) {
    puts("\n[/proc/uptime]\n");
    char buf[256];
    long fd = sc3(SYS_OPEN, (long)"/proc/uptime", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("uptime > 0", parse_long(buf) > 0); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_uptime", test_proc_uptime);

/* ── /proc/loadavg ─────────────────────────────── */

static void test_proc_loadavg(void) {
    puts("\n[/proc/loadavg]\n");
    char buf[256];
    long fd = sc3(SYS_OPEN, (long)"/proc/loadavg", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("contains /", contains(buf, (int)n, "/")); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_loadavg", test_proc_loadavg);

/* ── /proc/filesystems ─────────────────────────── */

static void test_proc_filesystems(void) {
    puts("\n[/proc/filesystems]\n");
    char buf[256];
    long fd = sc3(SYS_OPEN, (long)"/proc/filesystems", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("has cosmofs", contains(buf, (int)n, "cosmofs")); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_filesystems", test_proc_filesystems);

/* ── /proc/meminfo ─────────────────────────────── */

static void test_proc_meminfo(void) {
    puts("\n[/proc/meminfo]\n");
    char buf[512];
    long fd = sc3(SYS_OPEN, (long)"/proc/meminfo", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("has MemTotal", contains(buf, (int)n, "MemTotal")); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_meminfo", test_proc_meminfo);

/* ── /proc/self/status ─────────────────────────── */

static void test_proc_self_status(void) {
    puts("\n[/proc/self/status]\n");
    char buf[512];
    long fd = sc3(SYS_OPEN, (long)"/proc/self/status", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) {
        buf[n] = 0;
        check("has Pid:", contains(buf, (int)n, "Pid:"));
        check("has Name:", contains(buf, (int)n, "Name:"));
        check("has State:", contains(buf, (int)n, "State:"));
        check("has VmSize:", contains(buf, (int)n, "VmSize:"));
    }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_self_status", test_proc_self_status);

/* ── /proc/self/cmdline ────────────────────────── */

static void test_proc_self_cmdline(void) {
    puts("\n[/proc/self/cmdline]\n");
    char buf[256];
    long fd = sc3(SYS_OPEN, (long)"/proc/self/cmdline", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("non-empty", n > 0);
    sc1(SYS_CLOSE, fd);
}
TEST("proc_self_cmdline", test_proc_self_cmdline);

/* ── /proc/self/cwd ────────────────────────────── */

static void test_proc_self_cwd(void) {
    puts("\n[/proc/self/cwd]\n");
    char buf[256];
    long fd = sc3(SYS_OPEN, (long)"/proc/self/cwd", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("starts with /", buf[0] == '/'); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_self_cwd", test_proc_self_cwd);

/* ── /proc/self/maps ───────────────────────────── */

static void test_proc_self_maps(void) {
    puts("\n[/proc/self/maps]\n");
    char buf[512];
    long fd = sc3(SYS_OPEN, (long)"/proc/self/maps", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("has address range", contains(buf, (int)n, "-")); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_self_maps", test_proc_self_maps);

/* ── /proc/bus/pci/devices ─────────────────────── */

static void test_proc_pci(void) {
    puts("\n[/proc/bus/pci/devices]\n");
    char buf[1024];
    long fd = sc3(SYS_OPEN, (long)"/proc/bus/pci/devices", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("has devices", n > 0);
    sc1(SYS_CLOSE, fd);
}
TEST("proc_pci", test_proc_pci);

/* ── /proc/sys/kernel/pid_max ──────────────────── */

static void test_proc_pid_max(void) {
    puts("\n[/proc/sys/kernel/pid_max]\n");
    char buf[64];
    long fd = sc3(SYS_OPEN, (long)"/proc/sys/kernel/pid_max", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("read", n > 0);
    if (n > 0) { buf[n] = 0; check("value > 0", parse_long(buf) > 0); }
    sc1(SYS_CLOSE, fd);
}
TEST("proc_pid_max", test_proc_pid_max);

/* ── /proc/sys/kernel/hostname ─────────────────── */

static void test_proc_hostname(void) {
    puts("\n[/proc/sys/kernel/hostname]\n");
    char buf[128];
    long fd = sc3(SYS_OPEN, (long)"/proc/sys/kernel/hostname", O_RDONLY, 0);
    check("open", fd >= 0);
    if (fd < 0) return;
    long n = sc3(SYS_READ, fd, (long)buf, sizeof(buf) - 1);
    check("non-empty", n > 0);
    sc1(SYS_CLOSE, fd);
}
TEST("proc_hostname", test_proc_hostname);
