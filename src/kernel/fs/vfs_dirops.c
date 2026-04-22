/* CosmoRT VFS — directory operations: mkdir, rmdir, unlink, rename, link
 *
 * Dispatch via mount table (mnt->i_ops->...).
 */

#include "fs/vfs_internal.h"
#include "event/epoll.h"
#include "linux/fcntl.h"

/* ── Detach child from parent's children list ──── */

int unlink_child(struct vfs_node *parent, struct vfs_node *child) {
    struct vfs_node **pp = &parent->inode->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next;
            child->next = 0;
            child->parent = 0;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

/* ── mkdir ──────────────────────────────────────── */

int vfs_mkdir_mode(const char *path, int mode) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (!mnt || !mnt->i_ops || !mnt->i_ops->mkdir) return -ENOENT;
    int ro = vfs_mount_writable(mnt);
    if (ro < 0) return ro;
    int r = mnt->i_ops->mkdir(mnt, relpath, mode);
    if (r == 0) dnotify_fire(path, DN_CREATE);
    return r;
}

int vfs_mkdir(const char *path) {
    return vfs_mkdir_mode(path, 0755);
}

/* ── rmdir ──────────────────────────────────────── */

int vfs_rmdir(const char *path) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (!mnt || !mnt->i_ops || !mnt->i_ops->rmdir) return -ENOENT;
    int ro = vfs_mount_writable(mnt);
    if (ro < 0) return ro;
    int r = mnt->i_ops->rmdir(mnt, relpath);
    if (r == 0) dnotify_fire(path, DN_DELETE);
    return r;
}

/* ── unlink ─────────────────────────────────────── */

int vfs_unlink(const char *path) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (!mnt || !mnt->i_ops || !mnt->i_ops->unlink) return -ENOENT;
    int ro = vfs_mount_writable(mnt);
    if (ro < 0) return ro;
    int r = mnt->i_ops->unlink(mnt, relpath);
    if (r == 0) dnotify_fire(path, DN_DELETE);
    return r;
}

/* ── rename ─────────────────────────────────────── */

int vfs_rename(const char *oldpath, const char *newpath) {
    const char *oldrel, *newrel;
    struct mount *old_mnt = vfs_resolve_mount(oldpath, &oldrel);
    struct mount *new_mnt = vfs_resolve_mount(newpath, &newrel);
    if (!old_mnt || !new_mnt) return -ENOENT;
    if (old_mnt != new_mnt) return -EXDEV; /* cross-device rename */
    int ro = vfs_mount_writable(old_mnt);
    if (ro < 0) return ro;
    if (old_mnt->i_ops && old_mnt->i_ops->rename) {
        int r = old_mnt->i_ops->rename(old_mnt, oldrel, newrel);
        if (r == 0) {
            /* Linux-dnotify: DN_RENAME-Event nur auf dem Parent, wenn
             * oldpath und newpath denselben Parent-Pfad haben (Rename
             * _innerhalb_ eines Verzeichnisses). Rename ueber Verzeichnis-
             * grenzen feuert DN_DELETE auf Alt-Parent und DN_CREATE auf
             * Neu-Parent, aber kein DN_RENAME. */
            int olen = 0; while (oldpath[olen]) olen++;
            int nlen = 0; while (newpath[nlen]) nlen++;
            while (olen > 0 && oldpath[olen - 1] != '/') olen--;
            while (nlen > 0 && newpath[nlen - 1] != '/') nlen--;
            int same = (olen == nlen);
            if (same) for (int i = 0; i < olen; i++) if (oldpath[i] != newpath[i]) { same = 0; break; }
            if (same) {
                /* Nur Event auf Parent (oldpath trifft parent per path-match) */
                dnotify_fire(oldpath, DN_RENAME);
            } else {
                dnotify_fire(oldpath, DN_DELETE);
                dnotify_fire(newpath, DN_CREATE);
            }
        }
        return r;
    }
    return -ENOENT;
}

/* ── link ───────────────────────────────────────── */

int vfs_link(const char *oldpath, const char *newpath) {
    const char *oldrel, *newrel;
    struct mount *old_mnt = vfs_resolve_mount(oldpath, &oldrel);
    struct mount *new_mnt = vfs_resolve_mount(newpath, &newrel);
    if (!old_mnt || !new_mnt) return -ENOENT;
    if (old_mnt != new_mnt) return -EXDEV;
    int ro = vfs_mount_writable(new_mnt);
    if (ro < 0) return ro;
    if (old_mnt->i_ops && old_mnt->i_ops->link)
        return old_mnt->i_ops->link(old_mnt, oldrel, newrel);
    return -ENOSYS;
}
