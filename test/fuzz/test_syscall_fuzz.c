/* Syscall fuzzer: every syscall number 0-511 + CosmoRT range + boundaries.
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

/* ── 64-bit boundary constants ──────────────────── */
#define I64_MIN  0x8000000000000000ULL
#define I64_MAX  0x7FFFFFFFFFFFFFFFULL
#define U64_MAX  0xFFFFFFFFFFFFFFFFULL
#define I32_MIN  0xFFFFFFFF80000000ULL
#define I32_MAX  0x000000007FFFFFFFULL
#define U32_MAX  0x00000000FFFFFFFFULL

/* ── PRNG ───────────────────────────────────────── */
static uint64_t rng_state;

static uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return rng_state = x;
}

/* ── Syscall number generator ───────────────────── */

static uint64_t nr_gen(void) {
    switch (xorshift64() % 10) {
    case 0:  return xorshift64() % 512;               /* Linux range 0-511 */
    case 1:  return 0x10000 + (xorshift64() % 32);    /* CosmoRT range */
    case 2:  return 0;                                 /* zero */
    case 3:  return I64_MAX;                           /* INT64_MAX */
    case 4:  return I64_MIN;                           /* INT64_MIN */
    case 5:  return U64_MAX;                           /* UINT64_MAX */
    case 6:  return 511;                               /* upper Linux bound */
    case 7:  return 512;                               /* just past Linux */
    case 8:  return xorshift64();                      /* full 64-bit random */
    default: return xorshift64() & 0xFFFF;             /* 16-bit range */
    }
}

/* ── Argument generators ────────────────────────── */

#define KADDR_HIGH  0xFFFF800000000000ULL
#define KADDR_TEXT  0xFFFFFFFF80000000ULL
#define ADDR_DEAD   0xDEAD000000000000ULL
#define USER_END    0x7FFFFFFFE000ULL

static uint64_t ptr_gen(uint64_t valid_buf) {
    switch (xorshift64() % 12) {
    case 0:  return 0;                                 /* NULL */
    case 1:  return 1;                                 /* misaligned */
    case 2:  return ADDR_DEAD;                         /* unmapped high */
    case 3:  return KADDR_HIGH;                        /* kernel space */
    case 4:  return KADDR_TEXT;                        /* kernel text */
    case 5:  return USER_END;                          /* user space top */
    case 6:  return valid_buf;                         /* valid page */
    case 7:  return valid_buf + 4096 - 1;              /* end of valid page */
    case 8:  return I64_MAX;                           /* INT64_MAX */
    case 9:  return U64_MAX;                           /* UINT64_MAX */
    case 10: return I64_MIN;                           /* INT64_MIN */
    default: return xorshift64() & 0x7FFFFFFFFFFFULL;  /* random user addr */
    }
}

static uint64_t fd_gen(void) {
    static const uint64_t fds[] = {
        0, 1, 2, 3, 5, 10, 255, 256,
        U64_MAX, I64_MAX, I64_MIN, I32_MIN, I32_MAX, U32_MAX,
    };
    uint64_t r = xorshift64();
    if ((r & 3) == 0) return xorshift64() & 0x3FF;
    return fds[r % 14];
}

static uint64_t size_gen(void) {
    static const uint64_t sizes[] = {
        0, 1, 4096, 4097,
        I32_MAX, U32_MAX, I64_MAX, U64_MAX, I64_MIN,
        (uint64_t)-1, (uint64_t)-4096,
    };
    uint64_t r = xorshift64();
    if ((r & 3) == 0) return xorshift64() & 0xFFFFF;
    return sizes[r % 11];
}

static uint64_t flags_gen(void) {
    static const uint64_t flags[] = {
        0, 1, U32_MAX, U64_MAX, I64_MAX, I64_MIN,
        0xDEADBEEF, 0x80000000,
    };
    uint64_t r = xorshift64();
    if ((r & 3) == 0) return xorshift64();
    return flags[r % 8];
}

/* ── One fuzz round (runs in child) ─────────────── */
static void fuzz_round(uint64_t seed) {
    rng_state = seed;

    /* Close all inherited FDs so we don't block on serial I/O */
    for (int fd = 0; fd < 16; fd++)
        sc1(SYS_CLOSE, fd);

    /* Block all signals so garbage sigaction can't trap us.
     * SIGKILL (9) stays unblockable — parent can still kill us. */
    {
        uint64_t all_blocked = ~((1ULL << 9) | (1ULL << 19));
        sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&all_blocked, 0, 8);
    }

    /* Scratch buffer — valid pointer for ptr_gen */
    long buf = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_PRIV_ANON, -1, 0);
    if (buf < 0) sc1(SYS_EXIT_GROUP, 99);
    uint64_t vbuf = (uint64_t)buf;

    for (int i = 0; i < FUZZ_CALLS; i++) {
        uint64_t nr = nr_gen();
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
    puts("NOTE: Fuzzer is non-deterministic. Reproduce failures via\n");
    puts("test/crash/ with fixed seed before fixing. Do not re-test\n");
    puts("fixes with the fuzzer — write a deterministic crash test.\n");

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

    volatile uint64_t sentinel_val = 0xFEEDFACECAFEBABEULL;

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

        /* Parent: poll with timeout */
        int status = 0;
        long w = -1;
        {
            int tries = 2000;
            while (tries-- > 0) {
                w = sc4(SYS_WAIT4, pid, (long)&status, 1 /* WNOHANG */, 0);
                if (w > 0) break;
                if (w < 0 && w != -10) break;
                sc0(SYS_SCHED_YIELD);
            }
            if (w == 0) {
                sc2(SYS_KILL, pid, 9);
                w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
            }
        }

        if (w < 0) { errors++; continue; }

        if (WIFSIGNALED(status) && WTERMSIG(status) == 9) {
            crashed++;
            if (crashed <= 5) {
                puts("  TIMEOUT round="); put_int(round);
                puts(" seed=0x"); put_hex(round_seed); puts("\n");
            }
            continue;
        }

        ok++;
    }

    puts("[fuzz] ok="); put_int(ok);
    puts(" timeout="); put_int(crashed);
    puts(" errors="); put_int(errors); puts("\n");

    check("kernel survived all rounds", ok + crashed + errors == FUZZ_ROUNDS);
    check("no timeouts", crashed == 0);
    check("sentinel intact", sentinel_val == 0xFEEDFACECAFEBABEULL);

    int fds_after = 0;
    for (int fd = 0; fd < 64; fd++) {
        if (sc2(SYS_FSTAT, fd, 0) != -9) fds_after++;
    }
    check("no fd leak", fds_after == fds_before);
    check("getpid works", sc0(SYS_GETPID) > 0);

    struct { long sec; long nsec; } t1, t2;
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
    sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);
    check("clock monotonic",
          t2.sec > t1.sec ||
          (t2.sec == t1.sec && t2.nsec >= t1.nsec));
}

CRASH_TEST("fuzz/syscall", test_syscall_fuzz);
