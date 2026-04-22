/* execve: DAC (MAY_EXEC) nach euid/euid/groups vs. nur Mode-X-Bit */
#include "ktest.h"

#define WIFEXITED(s)    (((s) & 0x7F) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xFF)

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Minimal static ELF. Wir koennen kein echtes Binary schreiben — daher nutzt
 * der Test einen existierenden ELF-kandidaten aus /bin. Wichtig ist nur:
 * execve darf vor dem Load an DAC scheitern mit EACCES. */

/* 0700 binary, owned by root: non-root (seteuid(nobody)) execve -> EACCES */
static void test_dac_0700_nonroot(void) {
    puts("\n[execve/dac: 0700 root-owned, non-root exec]\n");

    const char script[] = "#!/bin/true\n";
    long fd = sc3(SYS_OPEN, (long)"/tmp/_execdac", O_CREAT | O_WRONLY | O_TRUNC, 0700);
    if (fd < 0) { check("create 0700 file", 0); return; }
    sc3(SYS_WRITE, fd, (long)script, slen(script));
    sc1(SYS_CLOSE, fd);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        /* Drop zu nobody */
        sc3(SYS_SETRESUID, -1L, 65534, -1L);
        char *argv[] = { "/tmp/_execdac", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check_val("EACCES", WEXITSTATUS(status), EACCES);

    sc2(SYS_UNLINK, (long)"/tmp/_execdac", 0);
}
TEST("execve/dac_0700_nonroot", test_dac_0700_nonroot);

/* 0755 file: non-root darf exec */
static void test_dac_0755_nonroot(void) {
    puts("\n[execve/dac: 0755 world-exec, non-root exec ok]\n");

    const char script[] = "#!/nonexistent\n"; /* shebang fails -> interp ENOENT */
    long fd = sc3(SYS_OPEN, (long)"/tmp/_execdac2", O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd < 0) { check("create 0755 file", 0); return; }
    sc3(SYS_WRITE, fd, (long)script, slen(script));
    sc1(SYS_CLOSE, fd);

    long pid = sc0(SYS_FORK);
    if (pid == 0) {
        sc3(SYS_SETRESUID, -1L, 65534, -1L);
        char *argv[] = { "/tmp/_execdac2", (char *)0 };
        char *envp[] = { (char *)0 };
        long rc = sc3(SYS_EXECVE, (long)argv[0], (long)argv, (long)envp);
        /* DAC sollte passen; Fehler ist ENOENT (interp) — nicht EACCES */
        sc1(SYS_EXIT_GROUP, (long)(-rc));
        __builtin_unreachable();
    }

    int status = 0;
    sc4(SYS_WAIT4, pid, (long)&status, 0, 0);
    check("child exited", WIFEXITED(status));
    check("not EACCES", WEXITSTATUS(status) != EACCES);

    sc2(SYS_UNLINK, (long)"/tmp/_execdac2", 0);
}
TEST("execve/dac_0755_nonroot", test_dac_0755_nonroot);
