#include "ktest.h"

void test_identity(void) {
    puts("\n[Identity]\n");
    long pid = sc0(SYS_getpid);
    check_ge("getpid > 0", pid, 1);

    long tid = sc0(SYS_gettid);
    check_ge("gettid > 0", tid, 1);

    struct { char s[65]; char n[65]; char r[65]; char v[65]; char m[65]; char d[65]; } uname;
    long ret = sc1(SYS_uname, (long)&uname);
    check_val("uname returns 0", ret, 0);
    check("uname.sysname = CosmoRT",
          uname.s[0]=='C' && uname.s[1]=='o' && uname.s[2]=='s' && uname.s[3]=='m' && uname.s[4]=='o');
    check("uname.machine = x86_64",
          uname.m[0]=='x' && uname.m[1]=='8' && uname.m[2]=='6');
}
