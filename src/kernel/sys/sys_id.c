/* CosmoRT — Identity syscalls (single-user: uid/gid always 0) */

#include "internal.h"

long do_getuid(void)  { return 0; }
long do_getgid(void)  { return 0; }
long do_geteuid(void) { return 0; }
long do_getegid(void) { return 0; }

/* set*id: single-user, always root — accept and ignore */
long do_setuid(long uid)   { (void)uid; return 0; }
long do_setgid(long gid)   { (void)gid; return 0; }
long do_setreuid(long ruid, long euid) { (void)ruid; (void)euid; return 0; }
long do_setregid(long rgid, long egid) { (void)rgid; (void)egid; return 0; }
long do_setresuid(long ruid, long euid, long suid) {
    (void)ruid; (void)euid; (void)suid; return 0;
}
long do_setresgid(long rgid, long egid, long sgid) {
    (void)rgid; (void)egid; (void)sgid; return 0;
}
long do_setfsuid(long uid)  { (void)uid; return 0; }
long do_setfsgid(long gid)  { (void)gid; return 0; }

/* getres*id: write 0,0,0 to user pointers */
long do_getresuid(long *ruid, long *euid, long *suid) {
    long zero = 0;
    if (ruid && user_ok((uint64_t)ruid, 4)) copy_to_user(ruid, &zero, 4);
    if (euid && user_ok((uint64_t)euid, 4)) copy_to_user(euid, &zero, 4);
    if (suid && user_ok((uint64_t)suid, 4)) copy_to_user(suid, &zero, 4);
    return 0;
}

long do_getresgid(long *rgid, long *egid, long *sgid) {
    long zero = 0;
    if (rgid && user_ok((uint64_t)rgid, 4)) copy_to_user(rgid, &zero, 4);
    if (egid && user_ok((uint64_t)egid, 4)) copy_to_user(egid, &zero, 4);
    if (sgid && user_ok((uint64_t)sgid, 4)) copy_to_user(sgid, &zero, 4);
    return 0;
}
