/* popen() pattern reproducer in test-hw.
 *
 * popen("cmd", "r") = pipe2() + fork() + child{dup2(pipe_w,1), exec sh -c}
 *                   + parent{close(pipe_w), fdopen(pipe_r) + fgets + pclose}
 *
 * Ohne /bin/sh in test-hw: kein execve. Diese Tests pruefen den Subset
 *   pipe2() + fork() + dup2() + child-write-stdout + parent-read + wait4
 * der bei popen ohne den exec-Anteil.
 *
 * Wenn diese Tests PASS und popen.exe im Vollauf TIMEOUT, ist die Wurzel
 * im execve+pipe-Zusammenspiel (musl posix_spawn error-pipe oder
 * exec-O_CLOEXEC-Cleanup). */

#include "ktest.h"

#define SYS_PIPE2_NR  293
#define SYS_DUP2_NR   33

/* ── 01: child writes to inherited pipe (no dup2) ─ */
static void test_popen_pipe_inherit(void) {
    puts("\n[popen pattern: pipe inherit]\n");
    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2_NR, (long)fds, 0);
    check_val("pipe2 ok", r, 0);
    if (r < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[0]);
        sc3(SYS_WRITE, fds[1], (long)"hello\n", 6);
        sc1(SYS_CLOSE, fds[1]);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);

    sc1(SYS_CLOSE, fds[1]);
    char buf[16] = {0};
    long rd = sc3(SYS_READ, fds[0], (long)buf, 15);
    check_val("parent read 6 bytes", rd, 6);
    check("hello content", buf[0]=='h' && buf[4]=='o' && buf[5]=='\n');

    /* read after EOF returns 0 */
    long rd2 = sc3(SYS_READ, fds[0], (long)buf, 15);
    check_val("read after EOF returns 0", rd2, 0);

    int ws = 0;
    long w = sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("wait4 returns child pid", w, pid);
    check_val("child exit 0", (ws >> 8) & 0xFF, 0);

    sc1(SYS_CLOSE, fds[0]);
}
TEST("popen/pipe_inherit", test_popen_pipe_inherit);

/* ── 02: child dup2 stdout to pipe, write via fd 1 ── */
static void test_popen_dup2_stdout(void) {
    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2_NR, (long)fds, 0);
    check_val("pipe2 ok", r, 0);
    if (r < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc2(SYS_DUP2_NR, fds[1], 1);
        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_CLOSE, fds[1]);
        sc3(SYS_WRITE, 1, (long)"world\n", 6);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }
    check("fork ok", pid > 0);

    sc1(SYS_CLOSE, fds[1]);
    char buf[16] = {0};
    long rd = sc3(SYS_READ, fds[0], (long)buf, 15);
    check_val("parent read 6 bytes via dup2", rd, 6);
    check("world content", buf[0]=='w' && buf[4]=='d' && buf[5]=='\n');

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check_val("dup2 child exit 0", (ws >> 8) & 0xFF, 0);

    sc1(SYS_CLOSE, fds[0]);
}
TEST("popen/dup2_stdout", test_popen_dup2_stdout);

/* ── 03: parent writes, child reads (popen "w" direction) ── */
static void test_popen_write_direction(void) {
    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2_NR, (long)fds, 0);
    check_val("pipe2 ok", r, 0);
    if (r < 0) return;

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc2(SYS_DUP2_NR, fds[0], 0);
        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_CLOSE, fds[1]);
        char b[16] = {0};
        long rd = sc3(SYS_READ, 0, (long)b, 15);
        int code = 0;
        if (rd == 5) code |= 1;
        if (b[0]=='h' && b[4]=='o') code |= 2;
        sc1(SYS_EXIT, code);
        __builtin_unreachable();
    }

    sc1(SYS_CLOSE, fds[0]);
    long w = sc3(SYS_WRITE, fds[1], (long)"hello", 5);
    check_val("parent write 5 bytes", w, 5);
    sc1(SYS_CLOSE, fds[1]);  /* signals EOF to child */

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    int code = (ws >> 8) & 0xFF;
    check("write_dir: child got 5 bytes", (code & 1) == 1);
    check("write_dir: child got hello",   (code & 2) == 2);
}
TEST("popen/write_direction", test_popen_write_direction);

/* ── 04: SIGUSR1 from child to parent (popen phase 2) ── */
struct ksigaction_pop {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

__attribute__((naked)) static void pop_sig_restorer(void) {
    __asm__ volatile("mov $15, %%rax\nsyscall\n" ::: "memory");
}

static volatile int got_usr1 = 0;
static void pop_usr1_handler(int sig) { (void)sig; got_usr1 = 1; }

static void test_popen_sigusr1_back(void) {
    struct ksigaction_pop sa = {
        .handler = (void *)pop_usr1_handler,
        .flags = SA_RESTORER,
        .restorer = (void *)pop_sig_restorer,
        .mask = 0,
    };
    sc4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8);

    int fds[2] = {-1, -1};
    long r = sc2(SYS_PIPE2_NR, (long)fds, 0);
    check_val("pipe2 ok", r, 0);
    if (r < 0) return;

    long parent_pid = sc0(SYS_GETPID);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_CLOSE, fds[1]);
        char b[8] = {0};
        sc3(SYS_READ, fds[0], (long)b, 7);
        sc1(SYS_CLOSE, fds[0]);
        if (b[0]=='h') sc2(SYS_KILL, parent_pid, SIGUSR1);
        sc1(SYS_EXIT, 0);
        __builtin_unreachable();
    }

    sc1(SYS_CLOSE, fds[0]);
    sc3(SYS_WRITE, fds[1], (long)"hello", 5);
    sc1(SYS_CLOSE, fds[1]);

    int ws = 0;
    sc4(SYS_WAIT4, pid, (long)&ws, 0, 0);
    check("usr1: child exit 0", ((ws >> 8) & 0xFF) == 0);
    check("usr1: parent got SIGUSR1", got_usr1 == 1);
}
TEST("popen/sigusr1_back", test_popen_sigusr1_back);
