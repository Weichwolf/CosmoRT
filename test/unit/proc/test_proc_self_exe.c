#include "ktest.h"

/* ── /proc/self/exe test ─────────────────────── */

#define SYS_READLINK 89

static void test_proc_self_exe(void) {
    puts("\n[/proc/self/exe]\n");

    char buf[256] = {0};
    long r = sc3(SYS_READLINK, (long)"/proc/self/exe", (long)buf, 255);
    check("readlink /proc/self/exe > 0", r > 0);
    /* The initial ktest binary was loaded via proc_create_elf, exe_path="/init" */
    check("exe path starts with /", buf[0] == '/');
    puts("  exe_path: ");
    buf[r] = 0;
    puts(buf);
    puts("\n");
}

TEST("/proc/self/exe", test_proc_self_exe);
