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

/* CosmoRT hardware syscalls */
#define SYS_COSMO_PCI_READ  516

/* Constants */
#define PROT_RW     0x3
#define MAP_PRIV_ANON 0x22
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

/* ── Main ────────────────────────────────────── */

void _start(void) {
    puts("\n=== CosmoRT Hardware Test ===\n");

    test_identity();
    test_memory();
    test_tls();
    test_time();
    test_random();
    test_vfs();
    test_threads();
    test_pci();
    test_security();
    test_yield();

    puts("\n=== ");
    put_int((long)passes); puts(" passed, ");
    put_int((long)failures); puts(" failed ===\n");

    if (failures == 0)
        puts("ALL PASSED\n");

    sc1(SYS_exit_group, (long)failures);
    __builtin_unreachable();
}
