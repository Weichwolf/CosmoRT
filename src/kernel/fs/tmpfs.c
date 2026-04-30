/* CosmoRT tmpfs — in-memory filesystem
 *
 * Mounted at /dev/shm, /tmp, and as fallback root when no ext4.
 * Uses vfs_node/vfs_inode dentry tree from der VFS-Schicht; Dentries werden
 * unter absolutem Pfad angelegt, daher Mount-Pfad + relpath rekonstruieren
 * bevor vfs_lookup aufgerufen wird.
 */

#include "fs/vfs_internal.h"

/* Absoluter Pfad fuer vfs_lookup: bei Root-Mount ist relpath schon absolut,
 * sonst mnt->path + relpath. Rueckgabe ist der zu nutzende Pfadzeiger,
 * entweder relpath direkt oder out. Bei Overflow gibt der Aufrufer out mit
 * truncated content zurueck — vfs_lookup erkennt das selbst via ENAMETOOLONG. */
static const char *tmpfs_abspath(struct mount *mnt, const char *relpath,
                                 char *out, int outsize) {
    if (!mnt || !mnt->path || mnt->pathlen <= 1) return relpath;
    int mi = 0;
    while (mnt->path[mi] && mi < outsize - 1) { out[mi] = mnt->path[mi]; mi++; }
    if (mi > 0 && out[mi - 1] == '/') mi--;
    if (relpath[0] == '/' && relpath[1] == 0) { out[mi] = 0; return out; }
    int ri = 0;
    while (relpath[ri] && mi + ri < outsize - 1) {
        out[mi + ri] = relpath[ri]; ri++;
    }
    out[mi + ri] = 0;
    return out;
}

/* ── inode_ops ──────────────────────────────────── */

static int tmpfs_op_stat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    char _a[512];
    const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(abs, &err);
    if (!node) return err;
    fill_stat(node->inode, buf);
    return 0;
}

static int tmpfs_op_lstat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    char _a[512];
    const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_nofollow(abs, &err);
    if (!node) return err;
    fill_stat(node->inode, buf);
    return 0;
}

static int tmpfs_op_mkdir(struct mount *mnt, const char *relpath, int mode) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    struct vfs_node *existing = vfs_lookup(abs);
    if (existing) return -EEXIST;
    struct vfs_node *n = vfs_create(abs, VFS_DIR);
    if (!n) return -ENOENT;
    process_t *pcur = proc_current();
    uint32_t cmask = pcur ? pcur->umask_val : 0022;
    uint32_t cmode = ((uint32_t)mode & 07777) & ~(cmask & 0777);
    struct vfs_inode *parent_ino = n->parent ? n->parent->inode : 0;
    if (parent_ino && (parent_ino->mode & S_ISGID))
        cmode |= S_ISGID;
    n->inode->mode = cmode;
    if (pcur) {
        n->inode->uid = pcur->fsuid;
        n->inode->gid = (parent_ino && (parent_ino->mode & S_ISGID))
                            ? parent_ino->gid
                            : pcur->fsgid;
    }
    return 0;
}

static int tmpfs_op_rmdir(struct mount *mnt, const char *relpath) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(abs, &err);
    if (!node) return err;
    if (node->inode->type != VFS_DIR) return -ENOTDIR;
    if (node->inode->children) return -ENOTEMPTY;
    if (node == vfs_root_node) return -EINVAL;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    inode_decref(node->inode);
    slab_free(&node_slab, node);
    return 0;
}

static int tmpfs_op_unlink(struct mount *mnt, const char *relpath) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_nofollow(abs, &err);
    if (!node) return err;
    if (node->inode->type == VFS_DIR) return -EISDIR;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    if (node->inode->nlink > 0) node->inode->nlink--;
    inode_decref(node->inode);
    slab_free(&node_slab, node);
    inotify_event(abs, IN_DELETE);
    return 0;
}

static int tmpfs_op_rename(struct mount *mnt, const char *oldrel, const char *newrel) {
    char _o[512], _n[512];
    const char *oabs = tmpfs_abspath(mnt, oldrel, _o, sizeof(_o));
    const char *nabs = tmpfs_abspath(mnt, newrel, _n, sizeof(_n));
    struct vfs_node *node = vfs_lookup(oabs);
    if (!node) return -ENOENT;
    struct vfs_node *dst = vfs_lookup(nabs);
    if (dst && dst != node) {
        if (dst->inode->type == VFS_DIR && dst->inode->children) return -ENOTEMPTY;
        if (dst->parent) unlink_child(dst->parent, dst);
        inode_decref(dst->inode);
        slab_free(&node_slab, dst);
    }
    if (node->parent) unlink_child(node->parent, node);

    const char *basename;
    struct vfs_node *new_parent = lookup_parent(nabs, &basename);
    if (!new_parent) return -ENOENT;
    kstrncpy(node->name, basename, 256);
    node->parent = new_parent;
    node->next = new_parent->inode->children;
    new_parent->inode->children = node;
    inotify_event(oabs, IN_MOVED_FROM);
    inotify_event(nabs, IN_MOVED_TO);
    return 0;
}

static int tmpfs_op_symlink(struct mount *mnt, const char *target, const char *relpath) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    struct vfs_node *existing = vfs_lookup(abs);
    if (existing) return -EEXIST;
    ensure_dirs(abs);
    struct vfs_node *n = vfs_create(abs, VFS_SYMLINK);
    if (!n) return -ENOMEM;
    n->inode->type = VFS_SYMLINK;
    kstrncpy(n->inode->symlink_target, target, 256);
    n->inode->size = (size_t)kstrlen(target);
    return 0;
}

static int tmpfs_op_readlink(struct mount *mnt, const char *relpath,
                             char *buf, size_t bufsiz) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_nofollow(abs, &err);
    if (!node) return err;
    if (node->inode->type != VFS_SYMLINK) return -EINVAL;
    int tlen = kstrlen(node->inode->symlink_target);
    if ((size_t)tlen > bufsiz) tlen = (int)bufsiz;
    kmemcpy(buf, node->inode->symlink_target, (size_t)tlen);
    return tlen;
}

static int tmpfs_op_link(struct mount *mnt, const char *oldrel, const char *newrel) {
    char _o[512], _n[512];
    const char *oabs = tmpfs_abspath(mnt, oldrel, _o, sizeof(_o));
    const char *nabs = tmpfs_abspath(mnt, newrel, _n, sizeof(_n));
    struct vfs_node *old_node = vfs_lookup(oabs);
    if (!old_node) return -ENOENT;
    if (old_node->inode->type == VFS_DIR) return -EPERM;

    struct vfs_node *existing = vfs_lookup(nabs);
    if (existing) return -EEXIST;

    const char *basename;
    struct vfs_node *parent = lookup_parent(nabs, &basename);
    if (!parent || parent->inode->type != VFS_DIR) return -ENOENT;

    struct vfs_node *n = node_alloc(basename, old_node->inode->type);
    if (!n) return -ENOMEM;
    /* Share inode */
    inode_decref(n->inode);
    n->inode = old_node->inode;
    n->inode->refcount++;
    n->inode->nlink++;
    n->parent = parent;
    n->next = parent->inode->children;
    parent->inode->children = n;
    return 0;
}

static void inode_drop_suid_sgid(struct vfs_inode *inode) {
    if (inode->type == VFS_DIR) return;
    if (inode->mode & S_ISUID)
        inode->mode &= ~(uint32_t)S_ISUID;
    if ((inode->mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
        inode->mode &= ~(uint32_t)S_ISGID;
}

static int tmpfs_op_chmod(struct mount *mnt, const char *relpath, uint32_t mode) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(abs, &err);
    if (!node) return err;
    process_t *cur = proc_current();
    if (!cred_can_chmod(cur, node->inode->uid)) return -EPERM;
    uint32_t new_mode = mode & 07777;
    if (cur && cur->euid != 0 && (new_mode & S_ISGID) &&
        !cred_in_group(cur, node->inode->gid))
        new_mode &= ~(uint32_t)S_ISGID;
    extern uint32_t timer_epoch_sec(void);
    node->inode->mode = new_mode;
    node->inode->ctime = timer_epoch_sec();
    inotify_event(abs, IN_ATTRIB);
    return 0;
}

static int tmpfs_chown_common(struct vfs_inode *inode, uint32_t uid, uint32_t gid) {
    process_t *cur = proc_current();
    if (cur && cur->euid != 0) {
        if (uid != (uint32_t)-1 && uid != inode->uid) return -EPERM;
        if (!cred_owns(cur, inode->uid)) return -EPERM;
        if (gid != (uint32_t)-1 && !cred_in_group(cur, gid)) return -EPERM;
    }
    extern uint32_t timer_epoch_sec(void);
    if (uid != (uint32_t)-1) inode->uid = uid;
    if (gid != (uint32_t)-1) inode->gid = gid;
    inode_drop_suid_sgid(inode);
    inode->ctime = timer_epoch_sec();
    return 0;
}

static int tmpfs_op_chown(struct mount *mnt, const char *relpath,
                          uint32_t uid, uint32_t gid) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(abs, &err);
    if (!node) return err;
    int rc = tmpfs_chown_common(node->inode, uid, gid);
    if (rc == 0) inotify_event(abs, IN_ATTRIB);
    return rc;
}

static int tmpfs_op_lchown(struct mount *mnt, const char *relpath,
                           uint32_t uid, uint32_t gid) {
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_nofollow(abs, &err);
    if (!node) return err;
    int rc = tmpfs_chown_common(node->inode, uid, gid);
    if (rc == 0) inotify_event(abs, IN_ATTRIB);
    return rc;
}

static int tmpfs_op_truncate(struct mount *mnt, const char *relpath, int64_t length) {
    if (length < 0) return -EINVAL;
    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(abs, &err);
    if (!node) return err;
    struct vfs_inode *inode = node->inode;
    if (inode->type == VFS_DIR) return -EISDIR;
    size_t new_size = (size_t)length;
    uint64_t irqf;
    spin_lock_irq(&inode->lock, &irqf);
    if (new_size > inode->size) {
        int gr = grow_file(inode, new_size);
        if (gr < 0) {
            spin_unlock_irq(&inode->lock, irqf);
            return gr; /* propagate -ENOSPC (budget) or -ENOMEM (alloc) */
        }
        /* New pages from grow_file are already zeroed by alloc_page;
         * zero only the tail of the previously-last page if it was reused. */
        size_t old_size = inode->size;
        size_t old_page_end = (old_size + 4095) & ~(size_t)0xFFF;
        if (old_page_end > old_size) {
            size_t z = old_page_end - old_size;
            if (z > new_size - old_size) z = new_size - old_size;
            ramfs_zero(inode, old_size, z);
        }
    } else if (new_size < inode->size) {
        shrink_file(inode, new_size);
    }
    inode->size = new_size;
    spin_unlock_irq(&inode->lock, irqf);
    return 0;
}

static int tmpfs_op_utimensat(struct mount *mnt, const char *relpath,
                              const int64_t times[4], int flags) {
    (void)flags;
    #define UTIME_NOW  ((1L << 30) - 1L)
    #define UTIME_OMIT ((1L << 30) - 2L)

    char _a[512]; const char *abs = tmpfs_abspath(mnt, relpath, _a, sizeof(_a));
    int err = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(abs, &err);
    if (!node) return err;
    struct vfs_inode *inode = node->inode;
    extern uint32_t timer_epoch_sec(void);
    if (times) {
        uint32_t now = timer_epoch_sec();
        int64_t atime_nsec = times[1], mtime_nsec = times[3];
        if (atime_nsec != UTIME_OMIT)
            inode->atime = (atime_nsec == UTIME_NOW) ? now : (uint64_t)times[0];
        if (mtime_nsec != UTIME_OMIT)
            inode->mtime = (mtime_nsec == UTIME_NOW) ? now : (uint64_t)times[2];
    } else {
        uint32_t now = timer_epoch_sec();
        inode->atime = now;
        inode->mtime = now;
    }
    return 0;
}

/* ── file_ops ───────────────────────────────────── */

static long tmpfs_op_read(struct vfs_file *f, void *buf, size_t count) {
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;

    size_t got = ramfs_read_locked(inode, buf, (size_t)f->offset, count);
    f->offset += got;
    return (long)got;
}

static long tmpfs_op_write(struct vfs_file *f, const void *buf, size_t count) {
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;

    uint64_t irqf;
    spin_lock_irq(&inode->lock, &irqf);

    if (f->flags & O_APPEND)
        f->offset = inode->size;

    size_t off = (size_t)f->offset;
    size_t end = off + count;
    int gr = grow_file(inode, end);
    if (gr < 0) {
        spin_unlock_irq(&inode->lock, irqf);
        return gr; /* propagate -ENOSPC / -ENOMEM */
    }
    ramfs_write(inode, buf, off, count);
    f->offset = end;
    if (end > inode->size) inode->size = end;

    extern uint32_t timer_epoch_sec(void);
    uint32_t now = timer_epoch_sec();
    inode->mtime = now;
    inode->ctime = now;
    spin_unlock_irq(&inode->lock, irqf);

    if (f->path[0])
        inotify_event(f->path, IN_MODIFY);
    extern void page_cache_invalidate_ino(uint64_t ino);
    page_cache_invalidate_ino(inode->ino);
    return (long)count;
}

static long tmpfs_op_lseek(struct vfs_file *f, long offset, int whence) {
    if (!f->inode) return -EBADF;
    long new_off;
    switch (whence) {
    case SEEK_SET: new_off = offset; break;
    case SEEK_CUR: new_off = (long)f->offset + offset; break;
    case SEEK_END: new_off = (long)f->inode->size + offset; break;
    default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}

static long tmpfs_op_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset) {
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;
    return (long)ramfs_read_locked(inode, buf, (size_t)offset, count);
}

static long tmpfs_op_pwrite(struct vfs_file *f, const void *buf,
                            size_t count, uint64_t offset) {
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    if (inode->type != VFS_FILE) return -EISDIR;
    uint64_t irqf;
    spin_lock_irq(&inode->lock, &irqf);
    size_t off = (size_t)offset;
    size_t end = off + count;
    int gr = grow_file(inode, end);
    if (gr < 0) {
        spin_unlock_irq(&inode->lock, irqf);
        return gr; /* propagate -ENOSPC / -ENOMEM */
    }
    ramfs_write(inode, buf, off, count);
    if (end > inode->size) inode->size = end;
    extern uint32_t timer_epoch_sec(void);
    uint32_t now = timer_epoch_sec();
    inode->mtime = now;
    inode->ctime = now;
    spin_unlock_irq(&inode->lock, irqf);
    if (f->path[0])
        inotify_event(f->path, IN_MODIFY);
    return (long)count;
}

static int tmpfs_op_close(struct vfs_file *f) {
    if (f->inode) {
        inode_decref(f->inode);
        f->inode = 0;
    }
    return 0;
}

static int tmpfs_op_fstat(struct vfs_file *f, struct k_stat *buf) {
    if (!f->inode) return -EBADF;
    fill_stat(f->inode, buf);
    return 0;
}

static int tmpfs_op_ftruncate(struct vfs_file *f, int64_t length) {
    if (length < 0) return -EINVAL;
    if (!f->inode) return -EBADF;
    struct vfs_inode *inode = f->inode;
    size_t new_size = (size_t)length;
    uint64_t irqf;
    spin_lock_irq(&inode->lock, &irqf);
    if (new_size > inode->size) {
        int gr = grow_file(inode, new_size);
        if (gr < 0) {
            spin_unlock_irq(&inode->lock, irqf);
            return gr; /* propagate -ENOSPC / -ENOMEM */
        }
        size_t old_size = inode->size;
        size_t old_page_end = (old_size + 4095) & ~(size_t)0xFFF;
        if (old_page_end > old_size) {
            size_t z = old_page_end - old_size;
            if (z > new_size - old_size) z = new_size - old_size;
            ramfs_zero(inode, old_size, z);
        }
    } else if (new_size < inode->size) {
        shrink_file(inode, new_size);
    }
    inode->size = new_size;
    spin_unlock_irq(&inode->lock, irqf);
    return 0;
}

static int tmpfs_op_fchmod(struct vfs_file *f, uint32_t mode) {
    if (!f->inode) return -EBADF;
    process_t *cur = proc_current();
    if (!cred_can_chmod(cur, f->inode->uid)) return -EPERM;
    uint32_t new_mode = mode & 07777;
    if (cur && cur->euid != 0 && (new_mode & S_ISGID) &&
        !cred_in_group(cur, f->inode->gid))
        new_mode &= ~(uint32_t)S_ISGID;
    extern uint32_t timer_epoch_sec(void);
    f->inode->mode = new_mode;
    f->inode->ctime = timer_epoch_sec();
    return 0;
}

static int tmpfs_op_fchown(struct vfs_file *f, uint32_t uid, uint32_t gid) {
    if (!f->inode) return -EBADF;
    return tmpfs_chown_common(f->inode, uid, gid);
}

static int tmpfs_op_fsync(struct vfs_file *f) {
    (void)f;
    return 0; /* in-memory, nothing to sync */
}

/* ── Exported ops tables ────────────────────────── */

struct inode_ops tmpfs_inode_ops = {
    .stat      = tmpfs_op_stat,
    .lstat     = tmpfs_op_lstat,
    .mkdir     = tmpfs_op_mkdir,
    .rmdir     = tmpfs_op_rmdir,
    .unlink    = tmpfs_op_unlink,
    .rename    = tmpfs_op_rename,
    .symlink   = tmpfs_op_symlink,
    .readlink  = tmpfs_op_readlink,
    .link      = tmpfs_op_link,
    .chmod     = tmpfs_op_chmod,
    .chown     = tmpfs_op_chown,
    .lchown    = tmpfs_op_lchown,
    .truncate  = tmpfs_op_truncate,
    .utimensat = tmpfs_op_utimensat,
};

struct file_ops tmpfs_file_ops = {
    .read      = tmpfs_op_read,
    .write     = tmpfs_op_write,
    .lseek     = tmpfs_op_lseek,
    .pread     = tmpfs_op_pread,
    .pwrite    = tmpfs_op_pwrite,
    .close     = tmpfs_op_close,
    .fstat     = tmpfs_op_fstat,
    .ftruncate = tmpfs_op_ftruncate,
    .fchmod    = tmpfs_op_fchmod,
    .fchown    = tmpfs_op_fchown,
    .fsync     = tmpfs_op_fsync,
};
