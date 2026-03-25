/* CosmoRT inotify — file system event monitoring (stub)
 *
 * Minimal implementation: returns valid fds but no real FS-event
 * delivery yet. Enough for programs that open inotify but tolerate
 * no events (Node.js fs.watch fallback to polling).
 */

#include "event/epoll.h"
#include "proc/process.h"
#include "mm/slab.h"
#include "spinlock.h"
#include "event/fd.h"

/* ── Inotify pool ───────────────────────────────── */

#define INOTIFY_POOL_MAX 16

typedef struct {
    int        refcount;
    spinlock_t lock;
} inotify_t;

static inotify_t inotify_pool[INOTIFY_POOL_MAX];
static slab_t    inotify_slab;

void inotify_init_slab(void) {
    slab_init(&inotify_slab, inotify_pool, (int)sizeof(inotify_t), INOTIFY_POOL_MAX);
}

/* ── SYS_INOTIFY_INIT1 (294) ───────────────────── */

long do_inotify_init1(int flags) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    inotify_t *ino = (inotify_t *)slab_alloc(&inotify_slab);
    if (!ino) return -ENOMEM;

    ino->refcount = 1;
    ino->lock = (spinlock_t)SPINLOCK_INIT;

    int fd_flags = O_RDWR;
    /* IN_CLOEXEC == O_CLOEXEC, IN_NONBLOCK == O_NONBLOCK on Linux x86_64 */
    if (flags & O_CLOEXEC)  fd_flags |= O_CLOEXEC;
    if (flags & O_NONBLOCK) fd_flags |= O_NONBLOCK;

    int fd = fd_alloc(&p->fds, FD_INOTIFY, ino, fd_flags);
    if (fd < 0) {
        slab_free(&inotify_slab, ino);
        return -EMFILE;
    }
    return fd;
}

long do_inotify_add_watch(int fd, const char *path, uint32_t mask) {
    (void)path; (void)mask;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_INOTIFY) return -EBADF;
    /* Stub: return watch descriptor 1 */
    return 1;
}

long do_inotify_rm_watch(int fd, int wd) {
    (void)wd;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_INOTIFY) return -EBADF;
    return 0;
}

long inotify_read(void *obj, void *buf, long count) {
    (void)obj; (void)buf; (void)count;
    return -EAGAIN; /* no events queued */
}

int inotify_has_events(void *obj) {
    (void)obj;
    return 0;
}

void inotify_event(const char *path, uint32_t mask) {
    (void)path; (void)mask;
    /* Stub: no delivery */
}

void inotify_destroy(void *obj) {
    if (!obj) return;
    inotify_t *ino = (inotify_t *)obj;
    if (__sync_sub_and_fetch(&ino->refcount, 1) <= 0)
        slab_free(&inotify_slab, obj);
}
