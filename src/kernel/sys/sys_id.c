/* CosmoRT — Identity syscalls (single-user: uid/gid always 0) */

#include "internal.h"

long do_getuid(void)  { return 0; }
long do_getgid(void)  { return 0; }
long do_geteuid(void) { return 0; }
long do_getegid(void) { return 0; }
