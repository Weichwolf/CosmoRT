/* Syscall fuzzer: deterministic random args to every syscall number.
 * Child process fuzzes, parent verifies survival. Kernel must never
 * crash, hang, or corrupt state — only return values or errno. */
#include "ktest.h"

/* ── Wait status macros ─────────────────────────── */
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

/* ── Configuration ──────────────────────────────── */
#ifndef FUZZ_ROUNDS
#define FUZZ_ROUNDS     20
#endif
#ifndef FUZZ_CALLS
#define FUZZ_CALLS      50
#endif
#ifndef FUZZ_SEED
#define FUZZ_SEED       0
#endif
#ifndef FUZZ_TIMEOUT_MS
#define FUZZ_TIMEOUT_MS 2000
#endif

/* ── PRNG ───────────────────────────────────────── */
static uint64_t rng_state;

static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}

static uint64_t pick(const uint64_t *tbl, int n) {
    return tbl[xorshift64() % n];
}

/* ── Argument generators ────────────────────────── */

#define KADDR_HIGH  0xFFFF800000000000ULL
#define KADDR_TEXT  0xFFFFFFFF80000000ULL
#define ADDR_DEAD   0xDEAD000000000000ULL
#define USER_END    0x7FFFFFFFE000ULL

static uint64_t ptr_gen(uint64_t valid_buf) {
    static const uint64_t base[] = {
        0, 1, ADDR_DEAD, KADDR_HIGH, KADDR_TEXT, USER_END,
    };
    uint64_t r = xorshift64();
    switch (r % 9) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return ADDR_DEAD;
    case 3:  return KADDR_HIGH;
    case 4:  return KADDR_TEXT;
    case 5:  return USER_END;
    case 6:  return valid_buf;
    case 7:  return valid_buf + 4096 - 1;
    default: return xorshift64() & 0x7FFFFFFFFFFFULL;
    }
    (void)base;
}

static uint64_t fd_gen(void) {
    static const uint64_t fds[] = {
        (uint64_t)-1, (uint64_t)-100, 0, 1, 2, 3, 5, 10,
        255, 256, 0x7FFFFFFF, (uint64_t)-2,
    };
    uint64_t r = xorshift64();
    if ((r & 3) == 0) return xorshift64() & 0x3FF;
    return fds[r % 12];
}

static uint64_t size_gen(void) {
    static const uint64_t sizes[] = {
        0, 1, 4096, 4097, 0x7FFFFFFF,
        0xFFFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
    };
    uint64_t r = xorshift64();
    if ((r & 3) == 0) return xorshift64() & 0xFFFFF;
    return sizes[r % 7];
}

static uint64_t flags_gen(void) {
    uint64_t r = xorshift64();
    switch (r % 5) {
    case 0:  return 0;
    case 1:  return (uint64_t)-1;
    case 2:  return 0xFFFFFFFF;
    case 3:  return xorshift64() & 0xFFFFFFFF;
    default: return xorshift64();
    }
}

static uint64_t sig_gen(void) {
    static const uint64_t sigs[] = {
        0, 1, 2, 9, 10, 11, 14, 15, 19, 31, 32, 33, 64,
        (uint64_t)-1, 65, 999,
    };
    return sigs[xorshift64() % 16];
}

static uint64_t pid_gen(void) {
    long self = sc0(SYS_GETPID);
    static const uint64_t base[] = {
        0, (uint64_t)-1, (uint64_t)-2, 1, 0x7FFFFFFF,
    };
    uint64_t r = xorshift64();
    if ((r & 3) == 0) return (uint64_t)self;
    return base[r % 5];
}

/* ── Syscall number tables per class ────────────── */

static const uint64_t nr_file[] = {
    SYS_READ, SYS_WRITE, SYS_OPEN, SYS_CLOSE, SYS_LSEEK,
    SYS_IOCTL, SYS_READV, SYS_WRITEV, SYS_ACCESS, SYS_DUP,
    SYS_DUP2, SYS_FCNTL, SYS_GETDENTS64, SYS_OPENAT,
    SYS_FACCESSAT, SYS_DUP3, SYS_PREAD64, SYS_PWRITE64,
};

static const uint64_t nr_mem[] = {
    SYS_MMAP, SYS_MPROTECT, SYS_MUNMAP, SYS_BRK, SYS_MREMAP,
    SYS_MADVISE, SYS_MLOCK, SYS_MUNLOCK, SYS_MLOCKALL, SYS_MUNLOCKALL,
};

/* No exit/exit_group — would kill the child prematurely */
static const uint64_t nr_proc[] = {
    SYS_WAIT4, SYS_GETPID, SYS_GETPPID, SYS_GETTID,
    SYS_GETUID, SYS_GETGID, SYS_GETEUID, SYS_GETEGID,
    SYS_SCHED_YIELD,
};

static const uint64_t nr_sig[] = {
    SYS_RT_SIGACTION, SYS_RT_SIGPROCMASK, SYS_KILL, SYS_TGKILL,
    SYS_SIGALTSTACK,
};

static const uint64_t nr_net[] = {
    SYS_SOCKET, SYS_CONNECT, SYS_BIND, SYS_LISTEN, SYS_ACCEPT,
    SYS_SENDTO, SYS_RECVFROM, SYS_SHUTDOWN, SYS_SETSOCKOPT,
    SYS_GETSOCKOPT, SYS_GETSOCKNAME, SYS_GETPEERNAME,
    SYS_SOCKETPAIR, SYS_ACCEPT4,
};

static const uint64_t nr_ipc[] = {
    SYS_PIPE, SYS_PIPE2, SYS_FUTEX, SYS_EVENTFD2,
    SYS_EPOLL_CREATE1, SYS_EPOLL_CTL, SYS_EPOLL_WAIT,
    SYS_TIMERFD_CREATE, SYS_TIMERFD_SETTIME, SYS_SIGNALFD4,
    SYS_INOTIFY_INIT1, SYS_INOTIFY_ADD_WATCH, SYS_INOTIFY_RM_WATCH,
};

static const uint64_t nr_fs[] = {
    SYS_STAT, SYS_LSTAT, SYS_FSTAT, SYS_FSTATAT, SYS_STATFS,
    SYS_FSTATFS, SYS_CHMOD, SYS_FCHMOD, SYS_FCHMODAT, SYS_FCHOWN,
    SYS_LINK, SYS_LINKAT, SYS_SYMLINK, SYS_SYMLINKAT,
    SYS_READLINK, SYS_READLINKAT, SYS_UNLINK, SYS_UNLINKAT,
    SYS_RENAME, SYS_RENAMEAT2, SYS_MKDIR, SYS_MKDIRAT, SYS_RMDIR,
    SYS_TRUNCATE, SYS_FTRUNCATE, SYS_UTIMENSAT, SYS_FALLOCATE,
    SYS_MKNODAT, SYS_CHDIR, SYS_GETCWD, SYS_STATX,
};

static const uint64_t nr_cosmo[] = {
    SYS_COSMO_MMIO_MAP, SYS_COSMO_DMA_ALLOC, SYS_COSMO_DMA_FREE,
    SYS_COSMO_IRQ_REGISTER, SYS_COSMO_PCI_READ, SYS_COSMO_PCI_WRITE,
    SYS_COSMO_FW_LOAD, SYS_COSMO_NIC_ATTACH, SYS_COSMO_KEXEC,
};

static const uint64_t nr_stubs[] = {
    SYS_GETUID, SYS_GETGID, SYS_GETEUID, SYS_GETEGID,
    SYS_GETPID, SYS_GETPPID, SYS_GETTID, SYS_SET_ROBUST_LIST,
    SYS_RSEQ, SYS_CAPGET, SYS_CAPSET, SYS_MOUNT, SYS_SETHOSTNAME,
    SYS_PRCTL, SYS_ARCH_PRCTL,
};

/* ── Invalid syscall numbers ────────────────────── */
static uint64_t random_invalid_nr(void) {
    static const uint64_t bad[] = {
        500, 511, 521, 550, 600, 999,
        0x7FFFFFFFFFFFFFFFULL,
    };
    return bad[xorshift64() % 7];
}

#define NUM_CLASSES 10
#define ARRAY_LEN(a) ((int)(sizeof(a)/sizeof(a[0])))

/* ── One fuzz round (runs in child) ─────────────── */
static void fuzz_round(uint64_t seed) {
    rng_state = seed;

    /* Close stdin to prevent blocking reads on fd 0 */
    sc1(SYS_CLOSE, 0);

    /* Scratch buffer — valid pointer for ptr_gen */
    long buf = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (buf < 0) sc1(SYS_EXIT_GROUP, 99);
    uint64_t vbuf = (uint64_t)buf;

    for (int i = 0; i < FUZZ_CALLS; i++) {
        uint64_t cls = xorshift64() % NUM_CLASSES;
        uint64_t nr;
        uint64_t a0, a1, a2, a3, a4, a5;

        switch (cls) {
        case 0: /* file I/O */
            nr = pick(nr_file, ARRAY_LEN(nr_file));
            a0 = fd_gen(); a1 = ptr_gen(vbuf); a2 = size_gen();
            a3 = flags_gen(); a4 = fd_gen(); a5 = size_gen();
            break;
        case 1: /* memory — highest risk */
            nr = pick(nr_mem, ARRAY_LEN(nr_mem));
            a0 = ptr_gen(vbuf); a1 = size_gen(); a2 = flags_gen();
            a3 = flags_gen(); a4 = fd_gen(); a5 = size_gen();
            break;
        case 2: /* process (no fork/exec in fuzz — too expensive/dangerous) */
            nr = pick(nr_proc, ARRAY_LEN(nr_proc));
            a0 = pid_gen(); a1 = ptr_gen(vbuf); a2 = flags_gen();
            a3 = ptr_gen(vbuf); a4 = 0; a5 = 0;
            break;
        case 3: /* signals */
            nr = pick(nr_sig, ARRAY_LEN(nr_sig));
            a0 = sig_gen(); a1 = ptr_gen(vbuf); a2 = ptr_gen(vbuf);
            a3 = 8; /* sigsetsize */ a4 = 0; a5 = 0;
            /* Avoid kill(-1, SIGKILL) — would kill parent */
            if (nr == SYS_KILL && a0 == (uint64_t)-1 &&
                (a1 == 9 || a1 == 15))
                a1 = 0; /* signal 0 = probe */
            if (nr == SYS_KILL) { a1 = a0; a0 = pid_gen(); }
            break;
        case 4: /* network */
            nr = pick(nr_net, ARRAY_LEN(nr_net));
            a0 = fd_gen(); a1 = ptr_gen(vbuf); a2 = size_gen();
            a3 = flags_gen(); a4 = ptr_gen(vbuf); a5 = size_gen();
            break;
        case 5: /* IPC */
            nr = pick(nr_ipc, ARRAY_LEN(nr_ipc));
            a0 = fd_gen(); a1 = ptr_gen(vbuf); a2 = size_gen();
            a3 = flags_gen(); a4 = ptr_gen(vbuf); a5 = size_gen();
            break;
        case 6: /* FS metadata */
            nr = pick(nr_fs, ARRAY_LEN(nr_fs));
            a0 = (xorshift64() & 1) ? (uint64_t)(long)AT_FDCWD : fd_gen();
            a1 = ptr_gen(vbuf); a2 = ptr_gen(vbuf);
            a3 = flags_gen(); a4 = size_gen(); a5 = flags_gen();
            break;
        case 7: /* CosmoRT hw primitives — must EPERM */
            nr = pick(nr_cosmo, ARRAY_LEN(nr_cosmo));
            a0 = ptr_gen(vbuf); a1 = size_gen(); a2 = flags_gen();
            a3 = ptr_gen(vbuf); a4 = size_gen(); a5 = flags_gen();
            break;
        case 8: /* stubs */
            nr = pick(nr_stubs, ARRAY_LEN(nr_stubs));
            a0 = ptr_gen(vbuf); a1 = size_gen(); a2 = flags_gen();
            a3 = ptr_gen(vbuf); a4 = size_gen(); a5 = flags_gen();
            break;
        default: /* unknown syscalls */
            nr = random_invalid_nr();
            a0 = ptr_gen(vbuf); a1 = size_gen(); a2 = flags_gen();
            a3 = ptr_gen(vbuf); a4 = size_gen(); a5 = flags_gen();
            break;
        }

        /* Fire the syscall — we only care that the kernel survives */
        sc6(nr, (long)a0, (long)a1, (long)a2, (long)a3, (long)a4, (long)a5);
    }

    /* Sanity: basic operations must still work after fuzzing */
    long pid = sc0(SYS_GETPID);
    if (pid <= 0) sc1(SYS_EXIT_GROUP, 1);

    char msg[] = "ok\n";
    long w = sc3(SYS_WRITE, 1, (long)msg, 3);
    if (w < 0) sc1(SYS_EXIT_GROUP, 2);

    /* mmap/munmap cycle */
    long page = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (page > 0) {
        *(volatile uint64_t *)page = 0xCAFEBABE;
        sc2(SYS_MUNMAP, page, 4096);
    }

    sc2(SYS_MUNMAP, (long)vbuf, 4096);
    sc1(SYS_EXIT_GROUP, 0);
    __builtin_unreachable();
}

/* ── Watchdog: kill child after timeout ─────────── */
static int wait_with_timeout(long child_pid, int *status) {
    /* Try non-blocking first */
    long r = sc4(SYS_WAIT4, child_pid, (long)status, 1 /* WNOHANG */, 0);
    if (r > 0) return 0; /* already done */

    /* Sleep in small increments, check periodically */
    struct { long sec; long nsec; } ts = { 0, 10000000 }; /* 10ms */
    int attempts = FUZZ_TIMEOUT_MS / 10;
    for (int i = 0; i < attempts; i++) {
        sc2(SYS_NANOSLEEP, (long)&ts, 0);
        r = sc4(SYS_WAIT4, child_pid, (long)status, 1 /* WNOHANG */, 0);
        if (r > 0) return 0;
    }

    /* Timeout — kill child */
    sc2(SYS_KILL, child_pid, SIGKILL);
    sc4(SYS_WAIT4, child_pid, (long)status, 0, 0);
    return -1; /* timed out */
}

/* ── Main test function ─────────────────────────── */
static void test_syscall_fuzz(void) {
    puts("\n[Syscall Fuzzer]\n");

    /* Seed */
    uint64_t seed = FUZZ_SEED;
    if (seed == 0) {
        struct { long sec; long nsec; } ts;
        sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&ts);
        seed = (uint64_t)ts.nsec;
        if (seed == 0) seed = 0xDEADBEEF;
    }
    puts("[fuzz] seed=0x"); put_hex(seed); puts("\n");
    puts("[fuzz] rounds="); put_int(FUZZ_ROUNDS);
    puts(" calls="); put_int(FUZZ_CALLS); puts("\n");

    /* Sentinel page to detect memory corruption */
    long sentinel = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    check("sentinel mmap", sentinel > 0);
    *(volatile uint64_t *)sentinel = 0xFEEDFACECAFEBABEULL;

    /* Snapshot: fd count before */
    int fds_before = 0;
    for (int fd = 0; fd < 64; fd++) {
        if (sc2(SYS_FSTAT, fd, 0) != -9) fds_before++;
    }

    int ok = 0, timeouts = 0, crashed = 0;

    for (int round = 0; round < FUZZ_ROUNDS; round++) {
        uint64_t round_seed = seed ^ ((uint64_t)round * 0x9E3779B97F4A7C15ULL);

        long pid = sc0(SYS_FORK);
        if (pid < 0) {
            puts("  fork failed round="); put_int(round);
            puts(" err="); put_int(pid); puts("\n");
            break;
        }
        if (pid == 0) {
            fuzz_round(round_seed);
            __builtin_unreachable();
        }

        /* Parent: wait with timeout */
        int status = 0;
        int r = wait_with_timeout(pid, &status);

        if (r < 0) {
            timeouts++;
            if (timeouts <= 3) {
                puts("  TIMEOUT round="); put_int(round);
                puts(" seed=0x"); put_hex(round_seed); puts("\n");
            }
            continue;
        }

        if (WIFSIGNALED(status)) {
            crashed++;
            if (crashed <= 5) {
                puts("  CRASH round="); put_int(round);
                puts(" sig="); put_int(WTERMSIG(status));
                puts(" seed=0x"); put_hex(round_seed); puts("\n");
            }
            continue;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            ok++;
    }

    puts("[fuzz] ok="); put_int(ok);
    puts(" crashed="); put_int(crashed);
    puts(" timeout="); put_int(timeouts); puts("\n");

    /* At least 90% of rounds must pass cleanly */
    check("fuzz >=90% clean", ok >= (FUZZ_ROUNDS * 9 / 10));
    check("fuzz no timeouts", timeouts == 0);

    /* Sentinel intact — kernel didn't corrupt our memory */
    check("sentinel intact",
          *(volatile uint64_t *)sentinel == 0xFEEDFACECAFEBABEULL);

    /* fd count unchanged */
    int fds_after = 0;
    for (int fd = 0; fd < 64; fd++) {
        if (sc2(SYS_FSTAT, fd, 0) != -9) fds_after++;
    }
    check("no fd leak", fds_after == fds_before);

    /* Parent kernel still responding */
    check("getpid works", sc0(SYS_GETPID) > 0);

    /* Timing still sane */
    struct { long sec; long nsec; } t1, t2;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);
    check("clock monotonic",
          t2.sec > t1.sec ||
          (t2.sec == t1.sec && t2.nsec >= t1.nsec));

    sc2(SYS_MUNMAP, sentinel, 4096);
}

CRASH_TEST("fuzz/syscall", test_syscall_fuzz);
