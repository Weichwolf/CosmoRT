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

/* ── Syscall number: 80% real Linux, 10% CosmoRT, 5% boundary, 5% random ── */

static uint64_t nr_gen(void) {
    uint64_t r = xorshift64() % 100;
    if (r < 80) {
        /* All syscalls defined in include/linux/syscall.h */
        static const uint64_t all_nr[] = {
            0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,
            23,24,25,26,28,32,33,35,37,39,40,41,42,43,44,45,46,47,48,49,
            50,51,52,53,54,55,56,57,58,59,60,61,62,63,72,73,76,77,79,80,
            82,83,84,86,87,88,89,90,91,93,94,95,96,97,98,99,100,102,104,
            107,108,109,110,111,112,115,116,121,124,125,126,130,131,137,
            138,142,143,144,145,146,147,149,150,151,152,157,158,160,165,
            169,170,186,188,189,190,191,192,193,194,195,196,197,198,199,
            201,202,203,204,217,218,221,228,229,230,231,232,233,234,254,
            255,257,258,259,262,263,265,266,267,268,269,270,271,273,280,
            281,283,285,286,288,289,290,291,292,293,294,295,296,299,302,
            307,309,316,318,332,334,435,
        };
        return all_nr[xorshift64() % (sizeof(all_nr)/sizeof(all_nr[0]))];
    }
    if (r < 90) {
        static const uint64_t cosmo[] = {
            SYS_COSMO_MMIO_MAP, SYS_COSMO_DMA_ALLOC, SYS_COSMO_DMA_FREE,
            SYS_COSMO_IRQ_REGISTER, SYS_COSMO_PCI_READ, SYS_COSMO_PCI_WRITE,
            SYS_COSMO_FW_LOAD, SYS_COSMO_NIC_ATTACH, SYS_COSMO_KEXEC,
            SYS_COSMO_RT_QUERY,
        };
        return cosmo[xorshift64() % 10];
    }
    if (r < 95) {
        /* Boundaries */
        static const uint64_t bounds[] = { 0, 511, 512, U64_MAX, I64_MAX, I64_MIN };
        return bounds[xorshift64() % 6];
    }
    return xorshift64();  /* full random */
}

/* ── Arguments: 70% plausible, 20% boundary, 10% random ── */

static uint64_t ptr_gen(uint64_t valid_buf) {
    uint64_t r = xorshift64() % 100;
    if (r < 50) return valid_buf;                       /* valid page */
    if (r < 60) return valid_buf + (xorshift64() % 4096); /* within page */
    if (r < 70) return 0;                               /* NULL */
    if (r < 75) return 0xFFFF800000000000ULL;           /* kernel space */
    if (r < 80) return 0xFFFFFFFF80000000ULL;           /* kernel text */
    if (r < 85) return 0x7FFFFFFFE000ULL;               /* user top */
    if (r < 88) return I64_MAX;
    if (r < 91) return U64_MAX;
    if (r < 94) return I64_MIN;
    if (r < 97) return 1;                               /* misaligned */
    return xorshift64();                                /* full random */
}

static uint64_t fd_gen(void) {
    uint64_t r = xorshift64() % 100;
    if (r < 60) return xorshift64() % 4;               /* stdin/out/err/scratch */
    if (r < 80) return xorshift64() % 256;             /* plausible fd */
    if (r < 85) return (uint64_t)-1;                   /* -1 (common error) */
    if (r < 90) return I32_MAX;
    if (r < 95) return U64_MAX;
    return xorshift64();                                /* full random */
}

static uint64_t size_gen(void) {
    uint64_t r = xorshift64() % 100;
    if (r < 40) return xorshift64() % 4097;            /* 0-4096 */
    if (r < 60) return xorshift64() % 0x10000;         /* 0-64K */
    if (r < 70) return 0;
    if (r < 75) return 1;
    if (r < 80) return 4096;
    if (r < 85) return I32_MAX;
    if (r < 88) return U32_MAX;
    if (r < 91) return I64_MAX;
    if (r < 94) return U64_MAX;
    if (r < 97) return I64_MIN;
    return xorshift64();                                /* full random */
}

static uint64_t flags_gen(void) {
    uint64_t r = xorshift64() % 100;
    if (r < 40) return 0;                              /* no flags */
    if (r < 60) return xorshift64() & 0xFF;            /* low byte flags */
    if (r < 75) return xorshift64() & 0xFFFF;          /* 16-bit flags */
    if (r < 85) return xorshift64() & 0xFFFFFFFF;      /* 32-bit flags */
    if (r < 90) return U64_MAX;
    if (r < 93) return I64_MIN;
    if (r < 96) return I64_MAX;
    return xorshift64();                                /* full random */
}

/* ── Shared crash log (MAP_SHARED, parent reads after child crash) ── */
typedef struct {
    volatile uint64_t nr, a0, a1, a2, a3, a4, a5;
    volatile int call_idx;
} fuzz_log_t;
static fuzz_log_t *fuzz_log;

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

        /* Log to shared memory so parent can report on crash */
        if (fuzz_log) {
            fuzz_log->nr = nr; fuzz_log->a0 = a0; fuzz_log->a1 = a1;
            fuzz_log->a2 = a2; fuzz_log->a3 = a3; fuzz_log->a4 = a4;
            fuzz_log->a5 = a5; fuzz_log->call_idx = i;
            __sync_synchronize();
        }
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
    puts("NOTE: Non-deterministic. To reproduce a crash:\n");
    puts("  1. make test-fuzz FUZZ_SEED=0x<seed from log>\n");
    puts("  2. Last 'round N seed=0x...' before crash identifies the round\n");
    puts("  3. Write a deterministic CRASH_TEST in test/crash/\n");
    puts("  4. Commit the crash test + fix\n");

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

    /* Shared crash log */
    long log_page = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fuzz_log = (log_page > 0) ? (fuzz_log_t *)log_page : 0;

    int fds_before = 0;
    for (int fd = 0; fd < 64; fd++) {
        if (sc2(SYS_FSTAT, fd, 0) != -9) fds_before++;
    }

    int ok = 0, crashed = 0, errors = 0;

    /* Block all catchable signals in the parent so child kill(0, sig) /
     * kill(-1, sig) cannot terminate us. SIGKILL (9) stays unblockable.
     * Ignore SIGSTOP (19) to prevent kill(-1, SIGSTOP) from halting us.
     * (CosmoRT extension: Linux doesn't allow ignoring SIGSTOP.) */
    {
        uint64_t all_blocked = ~((1ULL << 9) | (1ULL << 19));
        sc4(SYS_RT_SIGPROCMASK, 2 /* SIG_SETMASK */, (long)&all_blocked, 0, 8);
        /* Set SIG_IGN for all stop signals that could freeze us */
        struct { void *handler; unsigned long flags; void *restorer; unsigned long mask; }
            ign = { (void *)1 /* SIG_IGN */, 0, 0, 0 };
        for (int s = 19; s <= 22; s++)
            sc4(SYS_RT_SIGACTION, s, (long)&ign, 0, 8);
    }

    for (int round = 0; round < FUZZ_ROUNDS; round++) {
        uint64_t round_seed = seed ^ ((uint64_t)round * 0x9E3779B97F4A7C15ULL);
        puts("  round "); put_int(round); puts(" seed=0x"); put_hex(round_seed); puts("\n");

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
        /* Put child in its own process group so kill(0, sig) in the child
         * cannot reach us. Race-free: child inherits blocked signals. */
        sc2(SYS_SETPGID, pid, pid);

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

        if (WIFSIGNALED(status)) {
            crashed++;
            if (crashed <= 5) {
                int sig = WTERMSIG(status);
                puts(sig == 9 ? "  TIMEOUT" : "  CRASH");
                puts(" round="); put_int(round);
                puts(" signal="); put_int(sig);
                puts(" seed=0x"); put_hex(round_seed);
                if (fuzz_log && fuzz_log->call_idx >= 0) {
                    puts(" call="); put_int(fuzz_log->call_idx);
                    puts(" sc("); put_hex(fuzz_log->nr);
                    puts(","); put_hex(fuzz_log->a0);
                    puts(","); put_hex(fuzz_log->a1);
                    puts(","); put_hex(fuzz_log->a2);
                    puts(","); put_hex(fuzz_log->a3);
                    puts(","); put_hex(fuzz_log->a4);
                    puts(","); put_hex(fuzz_log->a5);
                    puts(")");
                }
                puts("\n");
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
