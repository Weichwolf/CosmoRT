/* CosmoRT — Identity syscalls
 * Single-user system: uid/gid = 1000 (normal user with full capabilities).
 * No permission enforcement, but correct metadata for POSIX conformance. */

#include "internal.h"

#define DEFAULT_UID 1000
#define DEFAULT_GID 1000

long do_getuid(void)  { return DEFAULT_UID; }
long do_getgid(void)  { return DEFAULT_GID; }
long do_geteuid(void) { return DEFAULT_UID; }
long do_getegid(void) { return DEFAULT_GID; }

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
long do_setfsuid(long uid)  { (void)uid; return DEFAULT_UID; }
long do_setfsgid(long gid)  { (void)gid; return DEFAULT_GID; }

long do_getresuid(long *ruid, long *euid, long *suid) {
    long v = DEFAULT_UID;
    if (ruid && user_ok((uint64_t)ruid, 4)) copy_to_user(ruid, &v, 4);
    if (euid && user_ok((uint64_t)euid, 4)) copy_to_user(euid, &v, 4);
    if (suid && user_ok((uint64_t)suid, 4)) copy_to_user(suid, &v, 4);
    return 0;
}

long do_getresgid(long *rgid, long *egid, long *sgid) {
    long v = DEFAULT_GID;
    if (rgid && user_ok((uint64_t)rgid, 4)) copy_to_user(rgid, &v, 4);
    if (egid && user_ok((uint64_t)egid, 4)) copy_to_user(egid, &v, 4);
    if (sgid && user_ok((uint64_t)sgid, 4)) copy_to_user(sgid, &v, 4);
    return 0;
}
