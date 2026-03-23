#include "ktest.h"

static void test_procgroup(void) {
    puts("\n[Process Groups]\n");

    long pid = sc0(SYS_getpid);

    /* Initial: getpgrp() == getpid() */
    long pgrp = sc0(SYS_getpgrp);
    check_val("getpgrp == getpid initially", pgrp, pid);

    /* setpgid(0, 0) → set own pgid to own pid */
    long r = sc2(SYS_setpgid, 0, 0);
    check_val("setpgid(0,0)", r, 0);

    /* getpgrp still == pid */
    pgrp = sc0(SYS_getpgrp);
    check_val("getpgrp == pid after setpgid(0,0)", pgrp, pid);

    /* getpgid(0) == own pgid */
    long pgid = sc1(SYS_getpgid, 0);
    check_val("getpgid(0) == pid", pgid, pid);

    /* getpgid(pid) == pid */
    pgid = sc1(SYS_getpgid, pid);
    check_val("getpgid(pid) == pid", pgid, pid);

    /* setsid() → new session, pgid = pid, returns pid */
    long sid = sc0(SYS_setsid);
    check_val("setsid returns pid", sid, pid);

    /* getsid(0) == pid */
    sid = sc1(SYS_getsid, 0);
    check_val("getsid(0) == pid", sid, pid);

    /* getsid(pid) == pid */
    sid = sc1(SYS_getsid, pid);
    check_val("getsid(pid) == pid", sid, pid);

    /* Fork + setpgid: child gets own process group */
    long child = sc0(SYS_fork);
    if (child == 0) {
        /* Child: setpgid(0, 0) to create own process group */
        sc2(SYS_setpgid, 0, 0);
        long cpgrp = sc0(SYS_getpgrp);
        long cpid = sc0(SYS_getpid);
        check_val("child pgrp == child pid", cpgrp, cpid);
        sc1(SYS_exit_group, 0);
    }
    if (child > 0) {
        /* Parent: wait for child */
        int wstatus = 0;
        sc4(SYS_wait4, child, (long)&wstatus, 0, 0);
        check("child exited", (wstatus & 0x7f) == 0);

        /* Parent pgid unchanged */
        pgrp = sc0(SYS_getpgrp);
        check_val("parent pgrp unchanged", pgrp, pid);
    }
}

TEST("procgroup", test_procgroup);
