/* CosmoRT Hardware Test — probes all subsystems after boot
 *
 * Tests: syscalls, memory, threads, VFS, PCI scan, NIC, timers, signals.
 * Reports PASS/FAIL per test on serial. Returns number of failures.
 */

typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned char uint8_t;
typedef unsigned long size_t;
typedef long ssize_t;

#define NULL ((void *)0)

/* ── Syscall wrappers ─────────────────────────── */

static long sc0(long n) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n):"rcx","r11","memory"); return r;
}
static long sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long sc2(long n, long a, long b) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b):"rcx","r11","memory"); return r;
}
static long sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}
static long sc4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10")=d;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory"); return r;
}
static long sc5(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory"); return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e; register long r9 __asm__("r9")=f;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory"); return r;
}

/* Syscall numbers */
#define SYS_read           0
#define SYS_write          1
#define SYS_open           2
#define SYS_close          3
#define SYS_fstat          5
#define SYS_mmap           9
#define SYS_munmap         11
#define SYS_brk            12
#define SYS_getpid         39
#define SYS_clone          56
#define SYS_exit           60
#define SYS_uname          63
#define SYS_getcwd         79
#define SYS_gettid         186
#define SYS_clock_gettime  228
#define SYS_getrandom      318
#define SYS_exit_group     231
#define SYS_arch_prctl     158
#define SYS_sched_yield    24
#define SYS_mprotect       10
#define SYS_kill           62
#define SYS_getpgrp        111

/* CosmoRT hardware syscalls */
#define SYS_COSMO_PCI_READ  516

/* Constants */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define PROT_RW     0x3
#define MAP_PRIV_ANON 0x22
#define MAP_FIXED_NOREPLACE 0x100032
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define CLOCK_MONOTONIC 1
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define CLONE_VM    0x100
#define CLONE_THREAD 0x10000

/* ── Output ───────────────────────────────────── */

static void puts(const char *s) {
    int n = 0; while (s[n]) n++;
    sc3(SYS_write, 1, (long)s, n);
}

static void put_hex(uint64_t v) {
    char buf[17]; int i = 0;
    if (v == 0) { puts("0"); return; }
    while (v) { buf[i++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    char out[17]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

static void put_int(long v) {
    if (v < 0) { puts("-"); v = -v; }
    char buf[20]; int i = 0;
    do { buf[i++] = '0' + (char)(v % 10); v /= 10; } while (v);
    char out[20]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

static int failures = 0;
static int passes = 0;

static void pass(const char *name) {
    puts("  PASS  "); puts(name); puts("\n");
    passes++;
}

static void fail(const char *name, const char *detail) {
    puts("  FAIL  "); puts(name);
    if (detail) { puts(" ("); puts(detail); puts(")"); }
    puts("\n");
    failures++;
}

static void check(const char *name, int condition) {
    if (condition) pass(name); else fail(name, NULL);
}

static void check_val(const char *name, long got, long expected) {
    if (got == expected) {
        pass(name);
    } else {
        puts("  FAIL  "); puts(name);
        puts(" (got="); put_int(got);
        puts(" expected="); put_int(expected); puts(")\n");
        failures++;
    }
}

static void check_ge(const char *name, long got, long minimum) {
    if (got >= minimum) {
        pass(name);
    } else {
        puts("  FAIL  "); puts(name);
        puts(" (got="); put_int(got);
        puts(" min="); put_int(minimum); puts(")\n");
        failures++;
    }
}

/* ── Tests ────────────────────────────────────── */

static void test_identity(void) {
    puts("\n[Identity]\n");
    long pid = sc0(SYS_getpid);
    check_ge("getpid > 0", pid, 1);

    long tid = sc0(SYS_gettid);
    check_ge("gettid > 0", tid, 1);

    struct { char s[65]; char n[65]; char r[65]; char v[65]; char m[65]; char d[65]; } uname;
    long ret = sc1(SYS_uname, (long)&uname);
    check_val("uname returns 0", ret, 0);
    check("uname.sysname = CosmoRT",
          uname.s[0]=='C' && uname.s[1]=='o' && uname.s[2]=='s' && uname.s[3]=='m' && uname.s[4]=='o');
    check("uname.machine = x86_64",
          uname.m[0]=='x' && uname.m[1]=='8' && uname.m[2]=='6');
}

static void test_memory(void) {
    puts("\n[Memory]\n");

    /* mmap anonymous */
    long addr = sc6(SYS_mmap, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap anon succeeds", addr > 0);
    if (addr > 0) {
        volatile char *p = (volatile char *)addr;
        p[0] = 0x42;
        check_val("mmap page writable", (long)p[0], 0x42);
        /* Page was zeroed */
        check_val("mmap page zeroed", (long)p[1], 0);
        long r = sc2(SYS_munmap, addr, 4096);
        check_val("munmap returns 0", r, 0);
    }

    /* mmap large (demand paging) */
    long big = sc6(SYS_mmap, 0, 1024*1024, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap 1MB succeeds", big > 0);
    if (big > 0) {
        volatile char *p = (volatile char *)big;
        /* Touch first and last page (triggers page faults) */
        p[0] = 1;
        p[1024*1024 - 1] = 2;
        check_val("demand page first", (long)p[0], 1);
        check_val("demand page last", (long)p[1024*1024-1], 2);
        sc2(SYS_munmap, big, 1024*1024);
    }

    /* brk */
    long brk0 = sc1(SYS_brk, 0);
    check("brk(0) returns current", brk0 > 0);
    long brk1 = sc1(SYS_brk, brk0 + 4096);
    check_val("brk grow", brk1, brk0 + 4096);
}

static void test_tls(void) {
    puts("\n[TLS]\n");

    uint64_t test_val = 0xDEADBEEF12345678ULL;
    long r = sc2(SYS_arch_prctl, ARCH_SET_FS, (long)&test_val);
    check_val("arch_prctl SET_FS", r, 0);

    uint64_t readback = 0;
    r = sc2(SYS_arch_prctl, ARCH_GET_FS, (long)&readback);
    check_val("arch_prctl GET_FS", r, 0);
    check("FS base roundtrip", readback == (uint64_t)(long)&test_val);
}

static void test_time(void) {
    puts("\n[Timers]\n");

    struct { long sec; long nsec; } ts;
    long r = sc2(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts);
    check_val("clock_gettime returns 0", r, 0);
    check_ge("time.sec >= 0", ts.sec, 0);
    check("time.nsec in range", ts.nsec >= 0 && ts.nsec < 1000000000);

    puts("  uptime: "); put_int(ts.sec); puts("s ");
    put_int(ts.nsec / 1000000); puts("ms\n");
}

static void test_random(void) {
    puts("\n[Random]\n");
    uint8_t buf[16] = {0};
    long r = sc3(SYS_getrandom, (long)buf, 16, 0);
    check_val("getrandom returns 16", r, 16);
    int nonzero = 0;
    for (int i = 0; i < 16; i++)
        if (buf[i]) nonzero++;
    check("getrandom produces data", nonzero > 0);
}

static void test_vfs(void) {
    puts("\n[VFS]\n");

    /* Create and write a file */
    long fd = sc3(SYS_open, (long)"/test.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("open /test.txt for write", fd >= 0);
    if (fd >= 0) {
        const char *data = "Hello from ktest!";
        long w = sc3(SYS_write, fd, (long)data, 17);
        check_val("write 17 bytes", w, 17);
        sc1(SYS_close, fd);
    }

    /* Read it back */
    fd = sc3(SYS_open, (long)"/test.txt", O_RDONLY, 0);
    check("open /test.txt for read", fd >= 0);
    if (fd >= 0) {
        char rbuf[32] = {0};
        long r = sc3(SYS_read, fd, (long)rbuf, 17);
        check_val("read 17 bytes", r, 17);
        check("read data matches",
              rbuf[0]=='H' && rbuf[1]=='e' && rbuf[2]=='l' && rbuf[3]=='l' && rbuf[4]=='o');
        sc1(SYS_close, fd);
    }

    /* getcwd */
    char cwd[128] = {0};
    long r = sc2(SYS_getcwd, (long)cwd, 128);
    check("getcwd succeeds", r > 0);
    check("cwd is /", cwd[0] == '/' && (cwd[1] == 0 || cwd[1] == '\n'));

    /* fstat on closed fd */
    struct {
        uint64_t dev, ino, nlink;
        uint32_t mode, uid, gid, pad;
        uint64_t rdev;
        int64_t size, blksize, blocks;
        int64_t atime_s, atime_ns, mtime_s, mtime_ns, ctime_s, ctime_ns;
        int64_t unused[3];
    } st;
    r = sc2(SYS_fstat, fd, (long)&st);
    /* fd is closed, should fail */
    check("fstat on closed fd fails", r < 0);
}

static volatile int worker_done = 0;

static void worker_fn(void) {
    __sync_fetch_and_add(&worker_done, 1);
    for (;;) __asm__ volatile("pause");
}

static void test_threads(void) {
    puts("\n[Threads]\n");

    worker_done = 0;

    /* Allocate stack for worker */
    long stack = sc6(SYS_mmap, 0, 65536, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap thread stack", stack > 0);
    if (stack <= 0) return;

    long ret = sc5(SYS_clone, CLONE_VM | CLONE_THREAD, stack + 65536, 0, 0, 0);
    if (ret == 0) {
        /* Child */
        worker_fn();
        __builtin_unreachable();
    }
    check("clone returns tid", ret > 0);

    /* Wait for worker to signal completion */
    for (volatile int i = 0; i < 10000000 && !worker_done; i++)
        __asm__ volatile("pause");

    check("worker thread ran", worker_done > 0);

    puts("  worker tid="); put_int(ret); puts("\n");
}

static void test_pci(void) {
    puts("\n[PCI Scan]\n");

    int found = 0;
    for (int bus = 0; bus < 8; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = 0;
            long r = sc5(SYS_COSMO_PCI_READ, bus, dev, 0, 0, (long)&id);
            if (r < 0 || id == 0 || id == 0xFFFFFFFF) continue;
            uint32_t vendor = id & 0xFFFF;
            uint32_t device = (id >> 16) & 0xFFFF;
            puts("  PCI "); put_int(bus); puts(":"); put_int(dev);
            puts(".0 = "); put_hex(vendor); puts(":"); put_hex(device);

            /* Identify known devices */
            if (vendor == 0x8086 && (device == 0x100E || device == 0x100F))
                puts(" (E1000 NIC)");
            else if (vendor == 0x8086 && device == 0x1237)
                puts(" (440FX Host)");
            else if (vendor == 0x8086 && device == 0x7000)
                puts(" (PIIX3 ISA)");
            else if (vendor == 0x8086 && device == 0x7010)
                puts(" (PIIX3 IDE)");
            else if (vendor == 0x8086 && device == 0x7113)
                puts(" (PIIX4 ACPI)");
            else if (vendor == 0x1234 && device == 0x1111)
                puts(" (QEMU VGA)");
            else if (vendor == 0x1AF4)
                puts(" (virtio)");

            puts("\n");
            found++;
        }
    }
    check_ge("PCI devices found", (long)found, 1);
}

static void test_security(void) {
    puts("\n[Security]\n");

    /* User pointer validation: kernel address should be rejected */
    long r = sc3(SYS_write, 1, 0xFFFF800000000000LL, 10);
    check("write(kernel_addr) → EFAULT", r == -14);

    r = sc3(SYS_read, 0, 0xFFFF800000000000LL, 10);
    check("read(kernel_addr) → EFAULT", r == -14);

    /* Overflow check */
    r = sc6(SYS_mmap, 0, (long)-1, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap(SIZE_MAX) fails", r < 0);
}

static void test_yield(void) {
    puts("\n[Scheduler]\n");
    long r = sc0(SYS_sched_yield);
    check_val("sched_yield returns 0", r, 0);
}

/* ── Signal tests ────────────────────────────── */

#define SYS_rt_sigaction  13
#define SYS_rt_sigprocmask 14
#define SYS_kill          62
#define SYS_fork          57
#define SYS_wait4         61

#define SIGUSR1 10
#define SIGUSR2 12
#define SIGTERM 15

#define SA_RESTORER 0x04000000
#define SA_SIGINFO  0x00000004

struct ksigaction {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

/* Minimal restorer trampoline: mov rax, 15; syscall */
__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile(
        "mov $15, %%rax\n"
        "syscall\n"
        ::: "memory"
    );
}

static volatile int sig_received = 0;
static volatile int sig_value = 0;

__attribute__((used)) static void test_handler(int sig) {
    sig_received = 1;
    sig_value = sig;
}

__attribute__((used)) static void test_handler_siginfo(int sig, void *info, void *uctx) {
    (void)info; (void)uctx;
    sig_received = 1;
    sig_value = sig;
}

static void test_signals(void) {
    puts("\n[Signals]\n");

    /* Test 1: SIG_IGN via rt_sigaction */
    struct ksigaction sa, old;
    sa.handler = (void *)1; /* SIG_IGN */
    sa.flags = 0;
    sa.restorer = 0;
    sa.mask = 0;
    long r = sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, (long)&old, 8);
    check_val("rt_sigaction SIG_IGN", r, 0);

    /* Send SIGUSR1 to self — should not crash (SIG_IGN) */
    r = sc2(SYS_kill, sc0(SYS_getpid), SIGUSR1);
    check_val("kill(self, SIGUSR1) SIG_IGN", r, 0);

    /* Test 2: User signal handler */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    r = sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
    check_val("rt_sigaction user handler", r, 0);

    /* Send SIGUSR1 to self — handler should run */
    r = sc2(SYS_kill, sc0(SYS_getpid), SIGUSR1);
    check_val("kill(self, SIGUSR1) handler", r, 0);
    check_val("handler received sig", sig_received, 1);
    check_val("handler got SIGUSR1", sig_value, SIGUSR1);

    /* Test 3: Second signal (SIGUSR2) with different handler */
    sig_received = 0;
    sig_value = 0;
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_rt_sigaction, SIGUSR2, (long)&sa, 0, 8);
    sc2(SYS_kill, sc0(SYS_getpid), SIGUSR2);
    check_val("SIGUSR2 handler ran", sig_received, 1);
    check_val("SIGUSR2 value", sig_value, SIGUSR2);

    /* Test 4: rt_sigprocmask — block SIGUSR1, send it, unblock, check delivery */
    sig_received = 0;
    sig_value = 0;
    /* Re-register handler (signal was auto-blocked during delivery) */
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);

    uint64_t block_mask = (1ULL << SIGUSR1);
    uint64_t old_mask = 0;
    sc4(SYS_rt_sigprocmask, 0 /* SIG_BLOCK */, (long)&block_mask, (long)&old_mask, 8);

    /* Send while blocked — should be queued */
    sc2(SYS_kill, sc0(SYS_getpid), SIGUSR1);
    check_val("blocked sig not delivered", sig_received, 0);

    /* Unblock — should deliver now */
    sc4(SYS_rt_sigprocmask, 1 /* SIG_UNBLOCK */, (long)&block_mask, 0, 8);
    check_val("unblocked sig delivered", sig_received, 1);
    check_val("unblocked sig value", sig_value, SIGUSR1);

    /* Test 5: oldact returned correctly */
    sa.handler = (void *)test_handler;
    sa.flags = SA_RESTORER;
    sa.restorer = (void *)sig_restorer;
    sa.mask = 0;
    sc4(SYS_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
    struct ksigaction got;
    sc4(SYS_rt_sigaction, SIGUSR1, 0, (long)&got, 8);
    check("oldact handler matches", got.handler == (void *)test_handler);
    check("oldact restorer matches", got.restorer == (void *)sig_restorer);
}

/* ── procfs ──────────────────────────────────── */

static int kstrncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

static void test_procfs(void) {
    puts("\n[Procfs]\n");

    /* /proc/dmesg — should contain boot output */
    long fd = sc3(SYS_open, (long)"/proc/dmesg", O_RDONLY, 0);
    check("open /proc/dmesg", fd >= 0);
    if (fd >= 0) {
        char buf[128] = {0};
        long r = sc3(SYS_read, fd, (long)buf, 127);
        check("read /proc/dmesg > 0", r > 0);
        /* Boot output starts with \r\n\r\nCosmoRT */
        int found = 0;
        for (int i = 0; i < (int)r - 7; i++) {
            if (buf[i]=='C' && buf[i+1]=='o' && buf[i+2]=='s' &&
                buf[i+3]=='m' && buf[i+4]=='o' && buf[i+5]=='R' && buf[i+6]=='T') {
                found = 1; break;
            }
        }
        check("dmesg contains CosmoRT", found);
        sc1(SYS_close, fd);
    }

    /* /proc/meminfo — should contain MemTotal */
    fd = sc3(SYS_open, (long)"/proc/meminfo", O_RDONLY, 0);
    check("open /proc/meminfo", fd >= 0);
    if (fd >= 0) {
        char buf[256] = {0};
        long r = sc3(SYS_read, fd, (long)buf, 255);
        check("read /proc/meminfo > 0", r > 0);
        check("meminfo starts with MemTotal", r >= 9 && kstrncmp(buf, "MemTotal:", 9) == 0);
        sc1(SYS_close, fd);
    }

    /* /proc/cpuinfo — should contain cores */
    fd = sc3(SYS_open, (long)"/proc/cpuinfo", O_RDONLY, 0);
    check("open /proc/cpuinfo", fd >= 0);
    if (fd >= 0) {
        char buf[256] = {0};
        long r = sc3(SYS_read, fd, (long)buf, 255);
        check("read /proc/cpuinfo > 0", r > 0);
        check("cpuinfo starts with processor", r >= 9 && kstrncmp(buf, "processor", 9) == 0);
        sc1(SYS_close, fd);
    }

    /* /proc/nonexistent — should fail */
    fd = sc3(SYS_open, (long)"/proc/nonexistent", O_RDONLY, 0);
    check("open /proc/nonexistent fails", fd < 0);
}

/* ── VM Patterns (JIT engines, large allocations) ── */

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
                   MAP_FIXED_NOREPLACE, -1, 0);
    check("MAP_FIXED_NOREPLACE → EEXIST", dup < 0);
    sc2(SYS_munmap, (long)base, 4096);

    /* 5. SIGABRT terminates (abort() pattern) */
    /* Can't test self-kill here — would terminate ktest.
     * Just verify kill syscall exists. */
    long kr = sc2(SYS_kill, sc0(SYS_getpid), 0); /* sig=0: check only */
    check("kill(self, 0) succeeds", kr == 0);
}

/* ── Batch 9 tests ───────────────────────────── */

#define SYS_getrusage     98
#define SYS_times         100
#define SYS_sigaltstack   131
#define SYS_rt_sigaction  13
#define SYS_poll          7
#define SYS_epoll_create1 291
#define SYS_epoll_ctl     233
#define SYS_epoll_wait    232
#define SYS_pipe2         293
#define SYS_eventfd2      290

#define RUSAGE_SELF       0
#define RUSAGE_CHILDREN   (-1)
#define SS_DISABLE        2
#define EPOLLIN           0x001
#define EPOLLOUT          0x004
#define EPOLLET           (1U << 31)
#define EPOLL_CTL_ADD     1
#define EPOLL_CTL_DEL     2

struct k_rusage {
    struct { long tv_sec; long tv_usec; } ru_utime;
    struct { long tv_sec; long tv_usec; } ru_stime;
    long ru_maxrss;
    long ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt, ru_nswap;
    long ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw, ru_nivcsw;
};

struct k_tms {
    long tms_utime, tms_stime, tms_cutime, tms_cstime;
};

struct k_stack_t {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  _pad;
    uint64_t ss_size;
};

struct k_epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

static void test_getrusage(void) {
    puts("\n[getrusage]\n");

    struct k_rusage ru;
    long r = sc2(SYS_getrusage, RUSAGE_SELF, (long)&ru);
    check_val("getrusage returns 0", r, 0);
    check("ru_maxrss > 0", ru.ru_maxrss > 0);
    check("ru_utime.tv_sec >= 0", ru.ru_utime.tv_sec >= 0);
    /* utime should reflect some uptime */
    check("ru_utime non-zero", ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0);

    /* RUSAGE_CHILDREN: should return 0 values */
    struct k_rusage ruc;
    r = sc2(SYS_getrusage, RUSAGE_CHILDREN, (long)&ruc);
    check_val("getrusage children returns 0", r, 0);
    check_val("children ru_maxrss = 0", ruc.ru_maxrss, 0);
}

static void test_times(void) {
    puts("\n[times]\n");

    struct k_tms tms;
    long r = sc1(SYS_times, (long)&tms);
    check("times returns ticks > 0", r > 0);
    check("tms_utime > 0", tms.tms_utime > 0);
    check_val("tms_stime = 0", tms.tms_stime, 0);
    check_val("tms_cutime = 0", tms.tms_cutime, 0);
    check_val("tms_cstime = 0", tms.tms_cstime, 0);
    /* tms_utime should roughly equal return value */
    long diff = r - tms.tms_utime;
    if (diff < 0) diff = -diff;
    check("tms_utime ≈ return value", diff < 10);
}

static void test_sigaltstack(void) {
    puts("\n[sigaltstack]\n");

    /* Get old stack (should be disabled initially) */
    struct k_stack_t oss;
    long r = sc2(SYS_sigaltstack, 0, (long)&oss);
    check_val("sigaltstack get returns 0", r, 0);

    /* Set alternate stack */
    long altstack = sc6(SYS_mmap, 0, 16384, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("mmap altstack", altstack > 0);
    if (altstack <= 0) return;

    struct k_stack_t ss;
    ss.ss_sp = (uint64_t)altstack;
    ss.ss_flags = 0;
    ss._pad = 0;
    ss.ss_size = 16384;
    r = sc2(SYS_sigaltstack, (long)&ss, (long)&oss);
    check_val("sigaltstack set returns 0", r, 0);

    /* Verify it was stored */
    struct k_stack_t oss2;
    r = sc2(SYS_sigaltstack, 0, (long)&oss2);
    check_val("sigaltstack verify returns 0", r, 0);
    check("ss_sp matches", oss2.ss_sp == (uint64_t)altstack);
    check("ss_size matches", oss2.ss_size == 16384);

    /* Disable */
    struct k_stack_t dis;
    dis.ss_sp = 0;
    dis.ss_flags = SS_DISABLE;
    dis._pad = 0;
    dis.ss_size = 0;
    r = sc2(SYS_sigaltstack, (long)&dis, 0);
    check_val("sigaltstack disable returns 0", r, 0);

    sc2(SYS_munmap, altstack, 16384);
}

static void test_poll_infinite(void) {
    puts("\n[poll timeout=-1]\n");

    /* Create a pipe, write to it, then poll with timeout=-1.
     * Should return immediately since data is ready. */
    int pipefd[2];
    long r = sc2(SYS_pipe2, (long)pipefd, 0);
    check_val("pipe2 returns 0", r, 0);
    if (r < 0) return;

    /* Write data to pipe */
    const char *msg = "hi";
    sc3(SYS_write, pipefd[1], (long)msg, 2);

    /* Poll read end with timeout=-1 (infinite) */
    struct { int fd; short events; short revents; } pfd;
    pfd.fd = pipefd[0];
    pfd.events = 0x0001; /* POLLIN */
    pfd.revents = 0;
    r = sc3(SYS_poll, (long)&pfd, 1, -1);
    check("poll(-1) returns ready", r >= 1);
    check("poll POLLIN set", (pfd.revents & 0x0001) != 0);

    sc1(SYS_close, pipefd[0]);
    sc1(SYS_close, pipefd[1]);
}

static void test_epollet(void) {
    puts("\n[EPOLLET]\n");

    /* Create eventfd + epoll. Add eventfd with EPOLLET|EPOLLIN.
     * Write to eventfd. First epoll_wait should return ready.
     * Second epoll_wait (timeout=0) should return 0 (edge already consumed). */
    long efd = sc2(SYS_eventfd2, 0, 0);
    check("eventfd2", efd >= 0);
    if (efd < 0) return;

    long epfd = sc1(SYS_epoll_create1, 0);
    check("epoll_create1", epfd >= 0);
    if (epfd < 0) { sc1(SYS_close, efd); return; }

    struct k_epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data = 42;
    long r = sc4(SYS_epoll_ctl, epfd, EPOLL_CTL_ADD, efd, (long)&ev);
    check_val("epoll_ctl ADD ET", r, 0);

    /* Write to eventfd */
    uint64_t val = 1;
    sc3(SYS_write, efd, (long)&val, 8);

    /* First epoll_wait: should see event */
    struct k_epoll_event out;
    r = sc4(SYS_epoll_wait, epfd, (long)&out, 1, 0);
    check_val("epoll_wait ET first", r, 1);

    /* Second epoll_wait: edge already consumed, should return 0 */
    r = sc4(SYS_epoll_wait, epfd, (long)&out, 1, 0);
    check_val("epoll_wait ET second = 0", r, 0);

    sc1(SYS_close, epfd);
    sc1(SYS_close, efd);
}

/* ── MAP_SHARED fork test ─────────────────── */

#define SYS_ioctl          16
#define MAP_SHARED_ANON    0x21  /* MAP_SHARED | MAP_ANONYMOUS */
#define TIOCSPGRP          0x5410
#define TIOCGPGRP          0x540F

static void test_map_shared(void) {
    puts("\n[MAP_SHARED]\n");

    /* mmap a shared anonymous page */
    long addr = sc6(SYS_mmap, 0, 4096, PROT_RW, MAP_SHARED_ANON, -1, 0);
    check("mmap MAP_SHARED|ANON", addr > 0);
    if (addr <= 0) return;

    volatile int32_t *shared = (volatile int32_t *)addr;
    *shared = 0;

    long pid = sc0(57 /* SYS_fork */);
    if (pid == 0) {
        /* Child: write sentinel, exit */
        *shared = 0xCAFE;
        sc1(SYS_exit_group, 0);
        __builtin_unreachable();
    }
    check("fork returns child pid", pid > 0);

    /* Wait for child */
    int wstatus = 0;
    sc4(61 /* SYS_wait4 */, (long)pid, (long)&wstatus, 0, 0);

    /* Parent reads child's write through shared page */
    check_val("shared page visible", (long)*shared, 0xCAFE);

    sc2(SYS_munmap, addr, 4096);
}

/* ── ioctl job control test ───────────────── */

static void test_ioctl_jobctl(void) {
    puts("\n[ioctl jobctl]\n");

    /* stdin (fd 0) should be a PTY slave in ktest.
     * Set foreground pgid, read it back. */
    int32_t set_pgid = 42;
    long r = sc3(SYS_ioctl, 0, TIOCSPGRP, (long)&set_pgid);
    check_val("TIOCSPGRP returns 0", r, 0);

    int32_t got_pgid = 0;
    r = sc3(SYS_ioctl, 0, TIOCGPGRP, (long)&got_pgid);
    check_val("TIOCGPGRP returns 0", r, 0);
    check_val("TIOCGPGRP roundtrip", (long)got_pgid, 42);
}

/* ── Main ────────────────────────────────────── */

void _start(void) {
    puts("\n=== CosmoRT Hardware Test ===\n");

    test_identity();
    test_memory();
    test_tls();
    test_time();
    test_random();
    test_vfs();
    test_procfs();
    test_threads();
    test_pci();
    test_security();
    test_yield();
    test_signals();
    test_vm_patterns();
    test_getrusage();
    test_times();
    test_sigaltstack();
    test_poll_infinite();
    test_epollet();
    test_map_shared();
    test_ioctl_jobctl();

    puts("\n=== ");
    put_int((long)passes); puts(" passed, ");
    put_int((long)failures); puts(" failed ===\n");

    if (failures == 0)
        puts("ALL PASSED\n");

    sc1(SYS_exit_group, (long)failures);
    __builtin_unreachable();
}
