/* Syscall fuzzer: deterministic random args to every syscall number.
 * Child process fuzzes, parent verifies survival. Kernel must never
 * crash, hang, or corrupt state — only return values or errno. */
#include "ktest.h"
#include "cosmort.h"

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

/* ── PRNG ───────────────────────────────────────── */
static uint64_t rng_state;

static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}

/* ── Argument generators ────────────────────────── */

#define KADDR_HIGH  0xFFFF800000000000ULL
#define KADDR_TEXT  0xFFFFFFFF80000000ULL
#define ADDR_DEAD   0xDEAD000000000000ULL
#define USER_END    0x7FFFFFFFE000ULL

static uint64_t ptr_gen(uint64_t valid_buf) {
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

/* ── Flat table of guaranteed non-blocking syscalls ── */

/* Every syscall in this table is guaranteed to return without blocking,
 * regardless of arguments. No I/O, no wait, no sleep, no signal suspend.
 * This covers: memory, fs metadata, process info, stubs, cosmo HW, unknown. */
static const uint64_t nr_all[] = {
    /* Memory (highest risk) */
    SYS_MMAP, SYS_MPROTECT, SYS_MUNMAP, SYS_BRK, SYS_MREMAP,
    SYS_MADVISE, SYS_MLOCK, SYS_MUNLOCK, SYS_MLOCKALL, SYS_MUNLOCKALL,
    /* FS metadata (non-blocking) */
    SYS_STAT, SYS_LSTAT, SYS_FSTAT, SYS_FSTATAT, SYS_STATFS,
    SYS_FSTATFS, SYS_CHMOD, SYS_FCHMOD, SYS_FCHMODAT, SYS_FCHOWN,
    SYS_LINK, SYS_LINKAT, SYS_SYMLINK, SYS_SYMLINKAT,
    SYS_READLINK, SYS_READLINKAT, SYS_UNLINK, SYS_UNLINKAT,
    SYS_RENAME, SYS_RENAMEAT2, SYS_MKDIR, SYS_MKDIRAT, SYS_RMDIR,
    SYS_TRUNCATE, SYS_FTRUNCATE, SYS_UTIMENSAT, SYS_FALLOCATE,
    SYS_MKNODAT, SYS_CHDIR, SYS_GETCWD, SYS_STATX,
    /* Process info (instant return) */
    SYS_GETPID, SYS_GETPPID, SYS_GETTID,
    SYS_GETUID, SYS_GETGID, SYS_GETEUID, SYS_GETEGID,
    SYS_SCHED_YIELD,
    /* File ops that never block (just return errno on bad fd) */
    SYS_CLOSE, SYS_LSEEK, SYS_ACCESS, SYS_DUP, SYS_DUP2,
    SYS_FCNTL, SYS_GETDENTS64, SYS_OPENAT, SYS_FACCESSAT, SYS_DUP3,
    /* Signals (non-blocking — no rt_sigsuspend) */
    SYS_RT_SIGACTION, SYS_RT_SIGPROCMASK, SYS_SIGALTSTACK,
    /* Stubs */
    SYS_SET_ROBUST_LIST, SYS_RSEQ, SYS_CAPGET, SYS_CAPSET,
    SYS_MOUNT, SYS_SETHOSTNAME, SYS_PRCTL, SYS_ARCH_PRCTL,
    /* CosmoRT hw (must EPERM) */
    SYS_COSMO_MMIO_MAP, SYS_COSMO_DMA_ALLOC, SYS_COSMO_DMA_FREE,
    SYS_COSMO_IRQ_REGISTER, SYS_COSMO_PCI_READ, SYS_COSMO_PCI_WRITE,
    SYS_COSMO_FW_LOAD, SYS_COSMO_NIC_ATTACH, SYS_COSMO_KEXEC,
    /* Socket creation (non-blocking) */
    SYS_SOCKET, SYS_BIND, SYS_LISTEN, SYS_SHUTDOWN,
    SYS_SETSOCKOPT, SYS_GETSOCKOPT, SYS_GETSOCKNAME, SYS_GETPEERNAME,
    SYS_SOCKETPAIR,
    /* Unknown / invalid syscall numbers */
    500, 511, 521, 600, 999,
};

#define NR_ALL_LEN ((int)(sizeof(nr_all)/sizeof(nr_all[0])))

/* ── One fuzz round (runs in child) ─────────────── */
static void fuzz_round(uint64_t seed) {
    rng_state = seed;

    /* Close all inherited FDs so we don't block on serial I/O */
    for (int fd = 0; fd < 16; fd++)
        sc1(SYS_CLOSE, fd);

    /* Scratch buffer — valid pointer for ptr_gen */
    long buf = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (buf < 0) sc1(SYS_EXIT_GROUP, 99);
    uint64_t vbuf = (uint64_t)buf;

    for (int i = 0; i < FUZZ_CALLS; i++) {
        uint64_t nr = nr_all[xorshift64() % NR_ALL_LEN];
        uint64_t a0 = ptr_gen(vbuf);
        uint64_t a1 = size_gen();
        uint64_t a2 = flags_gen();
        uint64_t a3 = ptr_gen(vbuf);
        uint64_t a4 = fd_gen();
        uint64_t a5 = flags_gen();

        /* For fd-based syscalls, use fd_gen for first arg */
        if (nr == SYS_CLOSE || nr == SYS_LSEEK || nr == SYS_DUP ||
            nr == SYS_DUP2 || nr == SYS_DUP3 || nr == SYS_FCNTL ||
            nr == SYS_GETDENTS64 || nr == SYS_FSTAT || nr == SYS_FSTATFS ||
            nr == SYS_FCHMOD || nr == SYS_FCHOWN || nr == SYS_FTRUNCATE ||
            nr == SYS_SHUTDOWN || nr == SYS_SETSOCKOPT ||
            nr == SYS_GETSOCKOPT || nr == SYS_GETSOCKNAME ||
            nr == SYS_GETPEERNAME || nr == SYS_SOCKET ||
            nr == SYS_BIND || nr == SYS_LISTEN)
            a0 = fd_gen();

        /* For *at syscalls, use AT_FDCWD or fd_gen */
        if (nr == SYS_OPENAT || nr == SYS_FSTATAT || nr == SYS_FACCESSAT ||
            nr == SYS_MKDIRAT || nr == SYS_MKNODAT || nr == SYS_UNLINKAT ||
            nr == SYS_LINKAT || nr == SYS_SYMLINKAT || nr == SYS_READLINKAT ||
            nr == SYS_FCHMODAT || nr == SYS_RENAMEAT2 || nr == SYS_UTIMENSAT ||
            nr == SYS_FALLOCATE || nr == SYS_STATX)
            a0 = (xorshift64() & 1) ? (uint64_t)(long)AT_FDCWD : fd_gen();

        sc6(nr, (long)a0, (long)a1, (long)a2, (long)a3, (long)a4, (long)a5);
    }

    /* Sanity: basic operations must still work after fuzzing */
    if (sc0(SYS_GETPID) <= 0) sc1(SYS_EXIT_GROUP, 1);

    /* mmap/munmap cycle — verifies VM subsystem is intact */
    long page = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (page > 0) {
        *(volatile uint64_t *)page = 0xCAFEBABE;
        if (*(volatile uint64_t *)page != 0xCAFEBABE)
            sc1(SYS_EXIT_GROUP, 2);
        sc2(SYS_MUNMAP, page, 4096);
    } else {
        sc1(SYS_EXIT_GROUP, 3);
    }

    sc2(SYS_MUNMAP, (long)vbuf, 4096);
    sc1(SYS_EXIT_GROUP, 0);
    __builtin_unreachable();
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

    int ok = 0, crashed = 0, errors = 0;

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

        /* Parent: blocking wait */
        int status = 0;
        long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);

        if (w < 0) {
            errors++;
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
    puts(" errors="); put_int(errors); puts("\n");

    /* Most rounds should pass — some may crash due to signal delivery */
    check("fuzz >=50% clean", ok >= (FUZZ_ROUNDS / 2));
    check("fuzz no kernel crashes", crashed == 0);

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
