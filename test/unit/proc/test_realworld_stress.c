/* Class C: Real-World-Stress-Pattern. */
#include "ktest.h"

#define FORK_EXEC_ITERS   50
#define SIG_STORM_COUNT   100
#define PIPE_CONCURRENT   16
#define MANY_MMAP_SIZE    4096
#define MANY_MMAP_COUNT   500
#define FD_REC_LIMIT      64

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

__attribute__((naked)) static void sig_restorer(void) {
    __asm__ volatile("mov $15, %rax\n\tsyscall");
}

static void test_fork_exec_storm(void) {
    puts("\n[FORK_EXEC_STORM]\n");

    int success = 0;
    for (int i = 0; i < FORK_EXEC_ITERS; i++) {
        long pid = sc0(SYS_FORK);
        if (pid < 0) break;
        if (pid == 0) {
            char *argv[2]; argv[0] = (char *)"/nonexistent_storm"; argv[1] = 0;
            long r = sc3(SYS_EXECVE, (long)argv[0], (long)argv, 0);
            sc1(SYS_EXIT_GROUP, (r == -ENOENT) ? 7 : 99);
            __builtin_unreachable();
        }
        int status = 0;
        long w = sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
        if (w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7)
            success++;
    }
    check_val("fork+execve(ENOENT) cycles all reap", success, FORK_EXEC_ITERS);
}

static void test_many_mmap_no_leak(void) {
    puts("\n[MANY_MMAP_NO_LEAK]\n");

    static long a[MANY_MMAP_COUNT];
    int alloced = 0;
    for (int i = 0; i < MANY_MMAP_COUNT; i++) {
        long r = sc6(SYS_MMAP, 0, MANY_MMAP_SIZE, PROT_RW,
                     MAP_PRIV_ANON, -1, 0);
        if (r <= 0) break;
        *(volatile uint32_t *)r = (uint32_t)i;
        a[i] = r;
        alloced++;
    }
    check_ge("500 4K mmaps", (long)alloced, 500);

    int ok = 0;
    for (int i = 0; i < alloced; i++)
        if (*(volatile uint32_t *)a[i] == (uint32_t)i) ok++;
    check_val("data intact across many mmaps", ok, alloced);

    int freed = 0;
    for (int i = 0; i < alloced; i++)
        if (sc2(SYS_MUNMAP, a[i], MANY_MMAP_SIZE) == 0) freed++;
    check_val("all munmaps succeed", freed, alloced);

    long probe = sc6(SYS_MMAP, 0, MANY_MMAP_SIZE, PROT_RW,
                     MAP_PRIV_ANON, -1, 0);
    check("fresh mmap after storm", probe > 0);
    if (probe > 0) sc2(SYS_MUNMAP, probe, MANY_MMAP_SIZE);
}

static void test_pipe_eof_propagation(void) {
    puts("\n[PIPE_EOF_PROPAGATION]\n");

    int fds[2];
    long r = sc2(SYS_PIPE2, (long)fds, 0);
    check_val("pipe2", r, 0);
    if (r != 0) return;

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) { sc1(SYS_CLOSE, fds[0]); sc1(SYS_CLOSE, fds[1]); return; }

    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        sc3(SYS_WRITE, fds[1], (long)"HELLO", 5);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    sc1(SYS_CLOSE, fds[1]);
    char buf[32];
    long n1 = sc3(SYS_READ, fds[0], (long)buf, 32);
    check_val("read data=5", n1, 5);
    long n2 = sc3(SYS_READ, fds[0], (long)buf, 32);
    check_val("EOF after writer close", n2, 0);

    sc1(SYS_CLOSE, fds[0]);
    sc4(SYS_WAIT4, pid, 0, 0, 0);
}

static volatile int sig_count;
static void sig_counter(int sig) { (void)sig; __sync_fetch_and_add(&sig_count, 1); }

static void test_signal_storm_no_loss(void) {
    puts("\n[SIGNAL_STORM]\n");

    sig_count = 0;

    struct k_sigaction sa = { 0 };
    sa.sa_handler = (void *)sig_counter;
    sa.sa_flags = SA_RESTORER | SA_RESTART;
    sa.sa_restorer = (void *)sig_restorer;
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    long self = sc0(SYS_GETPID);
    for (int i = 0; i < SIG_STORM_COUNT; i++)
        sc2(SYS_KILL, self, SIGUSR1);

    for (volatile int w = 0; w < 10000000; w++)
        __asm__ volatile("pause");

    check_ge("at least 1 SIGUSR1 delivered", (long)sig_count, 1);
    check("no more than sent", sig_count <= SIG_STORM_COUNT);
    puts("  delivered="); put_int(sig_count);
    puts(" sent="); put_int(SIG_STORM_COUNT); puts("\n");
}

/* Zwei unabhaengige mmap(MAP_SHARED, same file) in Parent und Child muessen
 * dieselbe physical page sharen — POSIX page-cache-Semantik. */
static void test_shared_mmap_cross_proc(void) {
    puts("\n[SHARED_MMAP_CROSS_PROC]\n");

    long fd = sc3(SYS_OPEN, (long)"/tmp/shared_xproc",
                  O_RDWR | O_CREAT | O_TRUNC, 0644);
    check("open file", fd >= 0);
    if (fd < 0) return;

    sc3(SYS_FTRUNCATE, fd, 4096, 0);

    long a1 = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_SHARED, fd, 0);
    check("mmap shared parent", a1 > 0);
    if (a1 <= 0) { sc1(SYS_CLOSE, fd); sc1(SYS_UNLINK, (long)"/tmp/shared_xproc"); return; }

    long pid = sc0(SYS_FORK);
    check("fork", pid >= 0);
    if (pid < 0) {
        sc2(SYS_MUNMAP, a1, 4096);
        sc1(SYS_CLOSE, fd);
        sc1(SYS_UNLINK, (long)"/tmp/shared_xproc");
        return;
    }
    if (pid == 0) {
        long a2 = sc6(SYS_MMAP, 0, 4096, PROT_RW, MAP_SHARED, fd, 0);
        if (a2 <= 0) sc1(SYS_EXIT_GROUP, 10);
        *(volatile uint64_t *)a2 = 0xCAFEF00DULL;
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check_val("child ok", (long)WEXITSTATUS(status), 0);
    check_val("parent mmap sees child mmap write",
              (long)*(volatile uint64_t *)a1, (long)0xCAFEF00DULL);

    sc2(SYS_MUNMAP, a1, 4096);
    sc1(SYS_CLOSE, fd);
    sc1(SYS_UNLINK, (long)"/tmp/shared_xproc");
}

static void test_fd_exhaustion_recovery(void) {
    puts("\n[FD_EXHAUSTION_RECOVERY]\n");

    struct { unsigned long cur, max; } save, tight = { FD_REC_LIMIT, 65536 };
    sc4(SYS_PRLIMIT64, 0, 7, 0, (long)&save);
    sc4(SYS_PRLIMIT64, 0, 7, (long)&tight, 0);

    int opened[128];
    int n = 0;
    for (int i = 0; i < 128; i++) {
        long fd = sc1(SYS_DUP, 0);
        if (fd < 0) break;
        opened[n++] = (int)fd;
    }
    check_ge("opened near limit", (long)n, 50);

    long fail = sc1(SYS_DUP, 0);
    check_val("dup at limit = EMFILE", fail, -EMFILE);

    for (int i = 0; i < n / 2; i++) sc1(SYS_CLOSE, opened[i]);

    long fd2 = sc1(SYS_DUP, 0);
    check("dup after close succeeds", fd2 >= 0);
    if (fd2 >= 0) sc1(SYS_CLOSE, fd2);

    for (int i = n / 2; i < n; i++) sc1(SYS_CLOSE, opened[i]);

    sc4(SYS_PRLIMIT64, 0, 7, (long)&save, 0);
}

static void test_pipe_concurrent_read_write(void) {
    puts("\n[PIPE_CONCURRENT_RW]\n");

    int fds[PIPE_CONCURRENT][2];
    int created = 0;
    for (int i = 0; i < PIPE_CONCURRENT; i++) {
        if (sc2(SYS_PIPE2, (long)fds[i], 0) < 0) break;
        created++;
    }
    check_ge("many pipes", created, PIPE_CONCURRENT / 2);

    for (int i = 0; i < created; i++) {
        long w = sc3(SYS_WRITE, fds[i][1], (long)"A", 1);
        if (w != 1) continue;
    }
    for (int i = 0; i < created; i++) sc1(SYS_CLOSE, fds[i][1]);
    int eof_count = 0;
    char c;
    for (int i = 0; i < created; i++) {
        sc3(SYS_READ, fds[i][0], (long)&c, 1);
        long n = sc3(SYS_READ, fds[i][0], (long)&c, 1);
        if (n == 0) eof_count++;
    }
    check_val("EOF on all pipes after writer close", eof_count, created);

    for (int i = 0; i < created; i++) sc1(SYS_CLOSE, fds[i][0]);
}

TEST("stress/fork_exec_storm",   test_fork_exec_storm);
TEST("stress/many_mmap",         test_many_mmap_no_leak);
TEST("stress/pipe_eof_prop",     test_pipe_eof_propagation);
TEST("stress/signal_storm",      test_signal_storm_no_loss);
TEST("stress/shared_mmap",       test_shared_mmap_cross_proc);
TEST("stress/fd_recovery",       test_fd_exhaustion_recovery);
TEST("stress/pipe_concurrent",   test_pipe_concurrent_read_write);
