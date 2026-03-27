#include "ktest.h"

/* Linux wait status macros */
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)

static void test_wait_status(void) {
    puts("\n[Wait Status]\n");
    int wstatus;
    long pid, r;

    /* Test 1: fork + exit(42) → WIFEXITED, WEXITSTATUS == 42 */
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 42);
        __builtin_unreachable();
    }
    check("fork1 ok", pid > 0);
    r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 returns child pid", r, pid);
    check("exit(42) WIFEXITED", WIFEXITED(wstatus));
    check_val("exit(42) WEXITSTATUS", WEXITSTATUS(wstatus), 42);

    /* Test 2: fork + exit(0) → WIFEXITED, WEXITSTATUS == 0 */
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc1(SYS_EXIT_GROUP, 0);
        __builtin_unreachable();
    }
    check("fork2 ok", pid > 0);
    r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 returns child pid (0)", r, pid);
    check("exit(0) WIFEXITED", WIFEXITED(wstatus));
    check_val("exit(0) WEXITSTATUS", WEXITSTATUS(wstatus), 0);

    /* Test 3: fork + raise(SIGSEGV) → WIFSIGNALED, WTERMSIG == 11 */
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* SIGSEGV = 11, default action = kill */
        sc2(SYS_KILL, sc0(SYS_GETPID), 11);
        /* Should not reach here */
        sc1(SYS_EXIT_GROUP, 99);
        __builtin_unreachable();
    }
    check("fork3 ok", pid > 0);
    r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 returns child pid (sig)", r, pid);
    check("SIGSEGV WIFSIGNALED", WIFSIGNALED(wstatus));
    check_val("SIGSEGV WTERMSIG", WTERMSIG(wstatus), 11);

    /* Test 4: fork + raise(SIGKILL) → WIFSIGNALED, WTERMSIG == 9 */
    pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc2(SYS_KILL, sc0(SYS_GETPID), 9);
        sc1(SYS_EXIT_GROUP, 99);
        __builtin_unreachable();
    }
    check("fork4 ok", pid > 0);
    r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
    check_val("wait4 returns child pid (kill)", r, pid);
    check("SIGKILL WIFSIGNALED", WIFSIGNALED(wstatus));
    check_val("SIGKILL WTERMSIG", WTERMSIG(wstatus), 9);
}

TEST("wait_status", test_wait_status);
