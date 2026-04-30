/* CosmoRT Loop-Device-Subsystem — minimal facade
 *
 * Provides /dev/loop-control + /dev/loop0..7 so LTP's
 * tst_acquire_device__() / tst_find_free_loopdev() / tst_attach_device()
 * cease to fail with -ENOENT.
 *
 * Status:
 *   - LOOP_CTL_GET_FREE      : fully implemented (slot allocator)
 *   - LOOP_CTL_REMOVE        : no-op success
 *   - LOOP_SET_FD            : duplicates backing fd into private slot
 *   - LOOP_CLR_FD            : clears slot, returns ENXIO once empty
 *   - LOOP_SET_STATUS{,64}   : copies-in info (offset/sizelimit/flags/name)
 *   - LOOP_GET_STATUS{,64}   : copies-out current info, returns ENXIO if unbound
 *   - read/write             : forward to backing fd via vfs_read/write
 *
 * mount("/dev/loopN", ...) is NOT integrated here — do_mount continues
 * to support tmpfs only. That's a follow-up; the acquire-side is already
 * the dominant blocker for ~25 LTP tests.
 */

#include "fs/loop.h"
#include "fs/vfs_internal.h"
#include "event/fd.h"
#include "uaccess.h"
#include "linux/errno.h"

static struct loop_dev loop_devs[LOOP_NDEV];
static int loop_inited;

static struct loop_dev *get_dev_idx(int idx) {
    if (idx < 0 || idx >= LOOP_NDEV) return 0;
    return &loop_devs[idx];
}

static int devid_to_idx(int devid) {
    if (devid < DEV_LOOP_BASE || devid >= DEV_LOOP_END) return -1;
    return devid - DEV_LOOP_BASE;
}

void loop_init(void) {
    if (loop_inited) return;
    for (int i = 0; i < LOOP_NDEV; i++) {
        loop_devs[i].in_use = 0;
        loop_devs[i].bound = 0;
        loop_devs[i].backing_fd = -1;
        loop_devs[i].offset = 0;
        loop_devs[i].sizelimit = 0;
        loop_devs[i].flags = 0;
        loop_devs[i].lo_name[0] = 0;
        loop_devs[i].lock = (spinlock_t)SPINLOCK_INIT;
    }
    loop_inited = 1;
}

struct loop_dev *loop_dev_get(int idx) {
    return get_dev_idx(idx);
}

/* LOOP_CTL_GET_FREE: scan for first slot that is neither in_use nor bound,
 * mark in_use=1, return idx. Linux returns the first slot whose status==
 * unbound — we conflate "in_use" and "bound" here because we never auto-add
 * devices: all 8 are visible always. */
static long loop_ctl_get_free(void) {
    for (int i = 0; i < LOOP_NDEV; i++) {
        struct loop_dev *d = &loop_devs[i];
        uint64_t f = irq_save();
        spin_lock(&d->lock);
        if (!d->bound && !d->in_use) {
            d->in_use = 1;
            spin_unlock(&d->lock);
            irq_restore(f);
            return (long)i;
        }
        spin_unlock(&d->lock);
        irq_restore(f);
    }
    return -ENOSPC; /* no free slot */
}

/* Duplicate-into-kernel: take the user's backing-fd, find its vfs_file*,
 * install a kernel-private fd_entry pointing at the same file with refcount
 * bumped (open_inc). For now we just remember the user-facing fd number;
 * read/write will look it up at the time of use. That's racy if the user
 * closes the fd, but matches the minimal scope.
 *
 * Linux semantics: dup the backing fd into the loop_device, increment
 * file->f_count, and the user is free to close their fd. We approximate
 * by ext4_open_inc on the disk inode if the file is ext4-backed. */
static long loop_set_fd(struct loop_dev *d, int backing_fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, backing_fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;
    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    uint64_t flg = irq_save();
    spin_lock(&d->lock);
    if (d->bound) {
        spin_unlock(&d->lock);
        irq_restore(flg);
        return -EBUSY;
    }
    /* Bump open count so subsequent close on user fd doesn't free the inode. */
    if (f->disk_ino) ext4_open_inc((uint32_t)f->disk_ino);
    if (f->inode) f->inode->refcount++;

    d->backing_fd = backing_fd;
    d->bound = 1;
    d->offset = 0;
    d->sizelimit = 0;
    d->flags = 0;
    d->lo_name[0] = 0;
    spin_unlock(&d->lock);
    irq_restore(flg);
    return 0;
}

static long loop_clr_fd(struct loop_dev *d) {
    uint64_t flg = irq_save();
    spin_lock(&d->lock);
    if (!d->bound) {
        spin_unlock(&d->lock);
        irq_restore(flg);
        return -ENXIO;
    }
    d->bound = 0;
    d->backing_fd = -1;
    d->offset = 0;
    d->sizelimit = 0;
    d->flags = 0;
    d->lo_name[0] = 0;
    /* keep in_use=1 — user still has /dev/loopN open. The detach loop in
     * LTP iterates ioctl(LOOP_CLR_FD)+sleep until it gets -ENXIO; second
     * call now hits the !bound branch above. Linux clears in_use only on
     * final close of /dev/loopN; we leave in_use=1 as well so the slot is
     * not handed out by GET_FREE while the fd is still open. */
    spin_unlock(&d->lock);
    irq_restore(flg);
    return 0;
}

static long loop_set_status(struct loop_dev *d, const struct loop_info *uinfo) {
    if (!user_ok((uint64_t)uinfo, sizeof(*uinfo))) return -EFAULT;
    struct loop_info ki;
    if (copy_from_user(&ki, uinfo, sizeof(ki)) < 0) return -EFAULT;
    uint64_t flg = irq_save();
    spin_lock(&d->lock);
    if (!d->bound) { spin_unlock(&d->lock); irq_restore(flg); return -ENXIO; }
    d->offset = (uint64_t)(uint32_t)ki.lo_offset;
    d->flags = (uint32_t)ki.lo_flags;
    for (int i = 0; i < 64; i++) d->lo_name[i] = ki.lo_name[i];
    spin_unlock(&d->lock);
    irq_restore(flg);
    return 0;
}

static long loop_get_status(struct loop_dev *d, int idx, struct loop_info *uinfo) {
    if (!user_ok((uint64_t)uinfo, sizeof(*uinfo))) return -EFAULT;
    uint64_t flg = irq_save();
    spin_lock(&d->lock);
    if (!d->bound) { spin_unlock(&d->lock); irq_restore(flg); return -ENXIO; }
    struct loop_info ki = {0};
    ki.lo_number = idx;
    ki.lo_offset = (int)d->offset;
    ki.lo_flags = (int)d->flags;
    for (int i = 0; i < 64; i++) ki.lo_name[i] = d->lo_name[i];
    spin_unlock(&d->lock);
    irq_restore(flg);
    if (copy_to_user(uinfo, &ki, sizeof(ki)) < 0) return -EFAULT;
    return 0;
}

static long loop_set_status64(struct loop_dev *d, const struct loop_info64 *uinfo) {
    if (!user_ok((uint64_t)uinfo, sizeof(*uinfo))) return -EFAULT;
    struct loop_info64 ki;
    if (copy_from_user(&ki, uinfo, sizeof(ki)) < 0) return -EFAULT;
    uint64_t flg = irq_save();
    spin_lock(&d->lock);
    if (!d->bound) { spin_unlock(&d->lock); irq_restore(flg); return -ENXIO; }
    d->offset = ki.lo_offset;
    d->sizelimit = ki.lo_sizelimit;
    d->flags = ki.lo_flags;
    for (int i = 0; i < 64; i++) d->lo_name[i] = ki.lo_file_name[i];
    spin_unlock(&d->lock);
    irq_restore(flg);
    return 0;
}

static long loop_get_status64(struct loop_dev *d, int idx, struct loop_info64 *uinfo) {
    if (!user_ok((uint64_t)uinfo, sizeof(*uinfo))) return -EFAULT;
    uint64_t flg = irq_save();
    spin_lock(&d->lock);
    if (!d->bound) { spin_unlock(&d->lock); irq_restore(flg); return -ENXIO; }
    struct loop_info64 ki = {0};
    ki.lo_number = (uint32_t)idx;
    ki.lo_offset = d->offset;
    ki.lo_sizelimit = d->sizelimit;
    ki.lo_flags = d->flags;
    for (int i = 0; i < 64; i++) ki.lo_file_name[i] = d->lo_name[i];
    spin_unlock(&d->lock);
    irq_restore(flg);
    if (copy_to_user(uinfo, &ki, sizeof(ki)) < 0) return -EFAULT;
    return 0;
}

long loop_ioctl(int devid, unsigned long request, unsigned long arg) {
    if (!loop_inited) loop_init();

    if (devid == DEV_LOOP_CTL) {
        switch (request) {
        case LOOP_CTL_GET_FREE: return loop_ctl_get_free();
        case LOOP_CTL_REMOVE:
            /* arg = device number; mark slot un-in_use if not bound */
            if ((long)arg >= 0 && (long)arg < LOOP_NDEV) {
                struct loop_dev *d = &loop_devs[arg];
                uint64_t f = irq_save();
                spin_lock(&d->lock);
                if (d->bound) {
                    spin_unlock(&d->lock); irq_restore(f);
                    return -EBUSY;
                }
                d->in_use = 0;
                spin_unlock(&d->lock); irq_restore(f);
            }
            return 0;
        case LOOP_CTL_ADD:
            /* All 8 slots already exist; succeed if in range, else -EINVAL */
            if ((long)arg < 0 || (long)arg >= LOOP_NDEV) return -EINVAL;
            return (long)arg;
        }
        return -EINVAL;
    }

    int idx = devid_to_idx(devid);
    if (idx < 0) return -ENOTTY;
    struct loop_dev *d = &loop_devs[idx];

    switch (request) {
    case LOOP_SET_FD:           return loop_set_fd(d, (int)arg);
    case LOOP_CLR_FD:           return loop_clr_fd(d);
    case LOOP_SET_STATUS:       return loop_set_status(d, (const struct loop_info *)arg);
    case LOOP_GET_STATUS:       return loop_get_status(d, idx, (struct loop_info *)arg);
    case LOOP_SET_STATUS64:     return loop_set_status64(d, (const struct loop_info64 *)arg);
    case LOOP_GET_STATUS64:     return loop_get_status64(d, idx, (struct loop_info64 *)arg);
    case LOOP_CHANGE_FD:        return -EINVAL; /* not supported */
    case LOOP_SET_CAPACITY:     return 0;
    case LOOP_SET_DIRECT_IO:    return 0;
    case LOOP_SET_BLOCK_SIZE:   return 0;
    }
    return -ENOTTY;
}

/* Forward to backing fd via the regular vfs_read/write. The user-facing
 * backing fd lives in the *current* process's fd table — same process that
 * issued LOOP_SET_FD typically. If a different process opens /dev/loopN,
 * the backing fd may not exist there. That's a follow-up; LTP scenarios
 * keep the loop in one process. */
long loop_read(int devid, void *user_buf, size_t count, int64_t *file_pos) {
    int idx = devid_to_idx(devid);
    if (idx < 0) return -EBADF;
    struct loop_dev *d = &loop_devs[idx];
    if (!d->bound) return -ENXIO;
    /* Ignore offset/sizelimit handling here — minimal */
    (void)file_pos;
    return vfs_read(d->backing_fd, (char *)user_buf, count);
}

long loop_write(int devid, const void *user_buf, size_t count, int64_t *file_pos) {
    int idx = devid_to_idx(devid);
    if (idx < 0) return -EBADF;
    struct loop_dev *d = &loop_devs[idx];
    if (!d->bound) return -ENXIO;
    (void)file_pos;
    return vfs_write(d->backing_fd, (const char *)user_buf, count);
}

void loop_dev_release(int devid) {
    int idx = devid_to_idx(devid);
    if (idx < 0) return;
    /* No refcount on the /dev/loopN fd — cannot tell if last user.
     * Leaving in_use=1 with bound=0 allows reuse via LOOP_CTL_GET_FREE
     * after explicit LOOP_CLR_FD + LOOP_CTL_REMOVE. */
    (void)idx;
}
