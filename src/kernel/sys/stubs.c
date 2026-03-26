/* CosmoRT — Stub syscalls (no-op or fixed return values) */

#include "internal.h"

long do_set_robust_list(void) { return 0; }
long do_mount(void)           { return 0; }
long do_sethostname(void)     { return 0; }
long do_rseq(void)            { return -ENOSYS; }
long do_capget(void)          { return -EPERM; }
long do_capset(void)          { return -EPERM; }

/* advisory locks: noop, single-user */
long do_flock(int fd, int operation) {
    (void)fd; (void)operation;
    return 0;
}

/* msync: validate parameters, actual write-back deferred to SH-C3 */
long do_msync(unsigned long addr, size_t length, int flags) {
    if (addr & 0xFFF) return -EINVAL;
    if (length == 0) return -EINVAL;
    int valid = MS_ASYNC | MS_SYNC | MS_INVALIDATE;
    if (flags & ~valid) return -EINVAL;
    /* MS_ASYNC and MS_SYNC are mutually exclusive */
    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) return -EINVAL;
    /* Must specify one of MS_ASYNC or MS_SYNC */
    if (!(flags & (MS_ASYNC | MS_SYNC))) return -EINVAL;
    return 0; /* actual write-back: SH-C3 */
}
/* TODO: implement if needed */
long do_sendfile(void) { return -ENOSYS; }
/* single-user: noop */
long do_lchown(void)   { return 0; }

long do_sched_get_priority_max(int policy) { (void)policy; return 31; }
long do_sched_get_priority_min(int policy) { (void)policy; return 0; }

/* noop: no enforcement */
long do_setrlimit(void) { return 0; }
/* no-op: no page cache hints */
long do_fadvise64(void) { return 0; }

/* single-user: return default, ignore new */
long do_umask(int mask) { (void)mask; return 0022; }

long do_getgroups(void) { return 0; }
long do_setgroups(void) { return 0; }
