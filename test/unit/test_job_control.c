#include "ktest.h"

#define TIOCSPGRP  0x5410
#define TIOCGPGRP  0x540F

#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WSTOPSIG(s)     (((s) >> 8) & 0xFF)
#define WIFCONTINUED(s) ((s) == 0xFFFF)
#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

struct ksigaction {
    void    *handler;
    uint64_t flags;
    void    *restorer;
    uint64_t mask;
};

static void test_job_control(void) {
    puts("\n[Job Control]\n");

    /* Test 1: TIOCSPGRP / TIOCGPGRP roundtrip on fd 0 (PTY slave) */
    {
        long mypid = sc0(SYS_GETPID);
        int32_t set_pgid = (int32_t)mypid;
        long r = sc3(SYS_IOCTL, 0, TIOCSPGRP, (long)&set_pgid);
        check_val("TIOCSPGRP returns 0", r, 0);

        int32_t got_pgid = 0;
        r = sc3(SYS_IOCTL, 0, TIOCGPGRP, (long)&got_pgid);
        check_val("TIOCGPGRP returns 0", r, 0);
        check_val("TIOCGPGRP roundtrip", (long)got_pgid, (long)set_pgid);
    }

    /* Test 2: SIGTSTP stops child, WUNTRACED reports it */
    {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            /* Child: put self in own process group */
            long cpid = sc0(SYS_GETPID);
            sc2(SYS_SETPGID, 0, 0);

            /* Busy-wait; parent will send SIGTSTP.
             * After being stopped + continued, exit cleanly. */
            for (volatile int i = 0; i < 5000000; i++) {}
            sc1(SYS_EXIT_GROUP, 0);
            __builtin_unreachable();
        }
        check("fork ok (SIGTSTP)", pid > 0);

        /* Give child a chance to start */
        struct { long sec; long nsec; } ts = {0, 10000000}; /* 10ms */
        sc2(SYS_NANOSLEEP, (long)&ts, 0);

        /* Send SIGTSTP to child */
        long r = sc2(SYS_KILL, pid, SIGTSTP);
        check_val("kill(child, SIGTSTP)", r, 0);

        /* Wait with WUNTRACED — should report stopped */
        int wstatus = 0;
        r = sc4(SYS_WAIT4, pid, (long)&wstatus, WUNTRACED, 0);
        check_val("wait4 WUNTRACED returns child", r, pid);
        check("WIFSTOPPED", WIFSTOPPED(wstatus));
        check_val("WSTOPSIG == SIGTSTP", (long)WSTOPSIG(wstatus), SIGTSTP);

        /* Send SIGCONT to resume */
        r = sc2(SYS_KILL, pid, SIGCONT);
        check_val("kill(child, SIGCONT)", r, 0);

        /* Wait with WCONTINUED — should report continued */
        wstatus = 0;
        r = sc4(SYS_WAIT4, pid, (long)&wstatus, WCONTINUED, 0);
        check_val("wait4 WCONTINUED returns child", r, pid);
        check("WIFCONTINUED", WIFCONTINUED(wstatus));

        /* Now wait for final exit */
        wstatus = 0;
        r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
        check_val("wait4 final exit", r, pid);
        check("WIFEXITED", WIFEXITED(wstatus));
    }

    /* Test 3: SIGCONT resumes stopped child */
    {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            /* Child: send SIGSTOP to self, then exit */
            sc2(SYS_KILL, sc0(SYS_GETPID), 19 /* SIGSTOP */);
            /* If we get here, we were resumed */
            sc1(SYS_EXIT_GROUP, 42);
            __builtin_unreachable();
        }
        check("fork ok (SIGCONT)", pid > 0);

        /* Wait for child to stop (blocking) */
        int wstatus = 0;
        long r = sc4(SYS_WAIT4, pid, (long)&wstatus, WUNTRACED, 0);
        check_val("SIGSTOP child reported", r, pid);
        check("WIFSTOPPED(SIGSTOP)", WIFSTOPPED(wstatus));

        /* Resume with SIGCONT */
        r = sc2(SYS_KILL, pid, SIGCONT);
        check_val("SIGCONT sent", r, 0);

        /* Wait for exit */
        wstatus = 0;
        r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
        check_val("child exited after SIGCONT", r, pid);
        check("exited normally", WIFEXITED(wstatus));
        check_val("exit code 42", (long)WEXITSTATUS(wstatus), 42);
    }

    /* Test 4: SIGINT to foreground process group via kill(-pgid) */
    {
        long pid = sc0(SYS_FORK);
        if (pid == 0) {
            /* Child: spin */
            for (;;) __asm__ volatile("pause");
            __builtin_unreachable();
        }
        check("fork ok (SIGINT fg)", pid > 0);

        /* Parent sets child into its own process group */
        long r = sc2(SYS_SETPGID, pid, pid);
        check_val("setpgid(child, child)", r, 0);

        struct { long sec; long nsec; } ts = {0, 10000000};
        sc2(SYS_NANOSLEEP, (long)&ts, 0);

        /* Send SIGINT to child's process group */
        r = sc2(SYS_KILL, -pid, SIGINT);
        check_val("kill(-pgid, SIGINT)", r, 0);

        int wstatus = 0;
        r = sc4(SYS_WAIT4, pid, (long)&wstatus, 0, 0);
        check_val("child reaped", r, pid);
        check_val("killed by SIGINT", (long)(wstatus & 0x7F), SIGINT);
    }
}

TEST("job_control", test_job_control);
