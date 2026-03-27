/* CosmoRT Job Control Tests — SIGTSTP/SIGCONT/SIGINT + wait4 */

#include "ktest.h"

/* Linux wait status macros */
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s)  (((s) & 0x7F) > 0 && ((s) & 0x7F) < 0x7F)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)     (((s) >> 8) & 0xFF)
#define WIFCONTINUED(s) ((s) == 0xFFFF)

/* wait4 option flags */
#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x540F

static void test_job_control(void) {
    puts("\n[Job Control]\n");

    /* Test 1: TIOCSPGRP / TIOCGPGRP roundtrip */
    {
        int32_t set_pgid = 99;
        long r = sc3(SYS_IOCTL, 0, TIOCSPGRP, (long)&set_pgid);
        check_val("TIOCSPGRP set 99", r, 0);

        int32_t got = 0;
        r = sc3(SYS_IOCTL, 0, TIOCGPGRP, (long)&got);
        check_val("TIOCGPGRP returns 0", r, 0);
        check_val("TIOCGPGRP roundtrip 99", (long)got, 99);
    }

    /* Test 2: fork → child setpgid → parent wait4 → child exit
     * This was the old hang-bug: wait4 blocked after fork+setpgid. */
    {
        long child = sc0(SYS_FORK);
        if (child == 0) {
            /* Child: create own process group, then exit */
            sc2(SYS_SETPGID, 0, 0);
            sc1(SYS_EXIT_GROUP, 42);
            __builtin_unreachable();
        }
        check("fork returned child pid", child > 0);
        int wstatus = 0;
        long w = sc4(SYS_WAIT4, child, (long)&wstatus, 0, 0);
        check_val("wait4 returned child pid", w, child);
        check("child exited normally", WIFEXITED(wstatus));
        check_val("child exit code 42", (long)WEXITSTATUS(wstatus), 42);
    }

    /* Test 3: SIGTSTP → child stopped → wait4(WUNTRACED) returns WIFSTOPPED */
    {
        long child = sc0(SYS_FORK);
        if (child == 0) {
            /* Child: loop until killed/stopped */
            for (volatile int i = 0; i < 10000; i++)
                sc0(SYS_SCHED_YIELD);
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        check("fork for stop test", child > 0);

        /* Give child a moment to start, then send SIGTSTP */
        sc0(SYS_SCHED_YIELD);
        sc0(SYS_SCHED_YIELD);
        long r = sc2(SYS_KILL, child, SIGTSTP);
        check_val("kill(child, SIGTSTP)", r, 0);

        /* Wait with WUNTRACED — should see stopped child */
        int wstatus = 0;
        long w = sc4(SYS_WAIT4, child, (long)&wstatus, WUNTRACED, 0);
        check_val("wait4 WUNTRACED returned child", w, child);
        check("WIFSTOPPED", WIFSTOPPED(wstatus));
        check_val("WSTOPSIG == SIGTSTP", (long)WSTOPSIG(wstatus), SIGTSTP);

        /* Resume with SIGCONT, then kill */
        sc2(SYS_KILL, child, SIGCONT);

        /* Wait for continued (non-blocking check) */
        int cstatus = 0;
        w = sc4(SYS_WAIT4, child, (long)&cstatus, WNOHANG | WCONTINUED, 0);
        /* WCONTINUED may or may not return immediately depending on timing */

        /* Kill child cleanly */
        sc2(SYS_KILL, child, SIGKILL);
        int kstatus = 0;
        sc4(SYS_WAIT4, child, (long)&kstatus, 0, 0);
        check("child killed after continue", WIFSIGNALED(kstatus));
    }

    /* Test 4: SIGINT to process group via kill(-pgid, SIGINT) */
    {
        long child = sc0(SYS_FORK);
        if (child == 0) {
            /* Child: set own process group, install SIG_DFL (terminate) */
            sc2(SYS_SETPGID, 0, 0);
            /* Yield to let parent send signal */
            for (volatile int i = 0; i < 10000; i++)
                sc0(SYS_SCHED_YIELD);
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        check("fork for pgid kill", child > 0);

        /* Set child's pgid from parent side too (race-free) */
        sc2(SYS_SETPGID, child, child);

        sc0(SYS_SCHED_YIELD);
        sc0(SYS_SCHED_YIELD);

        /* Kill the child's process group */
        long r = sc2(SYS_KILL, -child, SIGINT);
        check_val("kill(-pgid, SIGINT)", r, 0);

        int wstatus = 0;
        long w = sc4(SYS_WAIT4, child, (long)&wstatus, 0, 0);
        check_val("wait4 after pgid kill", w, child);
        check("killed by signal", WIFSIGNALED(wstatus));
        check_val("WTERMSIG == SIGINT", (long)WTERMSIG(wstatus), SIGINT);
    }

    /* Test 5: SIGCONT resumes stopped child → wait4(WCONTINUED) */
    {
        long child = sc0(SYS_FORK);
        if (child == 0) {
            for (volatile int i = 0; i < 10000; i++)
                sc0(SYS_SCHED_YIELD);
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        check("fork for continue test", child > 0);

        sc0(SYS_SCHED_YIELD);
        sc0(SYS_SCHED_YIELD);

        /* Stop child */
        sc2(SYS_KILL, child, SIGSTOP);

        /* Wait for stop */
        int wstatus = 0;
        long w = sc4(SYS_WAIT4, child, (long)&wstatus, WUNTRACED, 0);
        check_val("SIGSTOP: wait4 returned child", w, child);
        check("SIGSTOP: WIFSTOPPED", WIFSTOPPED(wstatus));

        /* Continue */
        sc2(SYS_KILL, child, SIGCONT);

        /* Check continued */
        int cstatus = 0;
        w = sc4(SYS_WAIT4, child, (long)&cstatus, WCONTINUED, 0);
        check_val("SIGCONT: wait4 returned child", w, child);
        check("SIGCONT: WIFCONTINUED", WIFCONTINUED(cstatus));

        /* Clean up */
        sc2(SYS_KILL, child, SIGKILL);
        sc4(SYS_WAIT4, child, 0, 0, 0);
    }

    /* Restore fg_pgid to something sane */
    {
        long mypid = sc0(SYS_GETPID);
        int32_t pgid = (int32_t)mypid;
        sc3(SYS_IOCTL, 0, TIOCSPGRP, (long)&pgid);
    }
}

TEST("job_control", test_job_control);
