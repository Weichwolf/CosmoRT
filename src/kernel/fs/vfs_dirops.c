/* CosmoRT VFS — directory operations: mkdir, rmdir, unlink, rename, link
 *
 * Dispatch via mount table (mnt->i_ops->...).
 */

#include "fs/vfs_internal.h"

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

int vfs_mkdir(const char *path) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->mkdir)
        return mnt->i_ops->mkdir(mnt, relpath, 0755);
    return -ENOENT;
}

/* ── rmdir ──────────────────────────────────────── */

int vfs_rmdir(const char *path) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->rmdir)
        return mnt->i_ops->rmdir(mnt, relpath);
    return -ENOENT;
}

/* ── unlink ─────────────────────────────────────── */

int vfs_unlink(const char *path) {
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->unlink)
        return mnt->i_ops->unlink(mnt, relpath);
    return -ENOENT;
}

/* ── rename ─────────────────────────────────────── */

int vfs_rename(const char *oldpath, const char *newpath) {
    const char *oldrel, *newrel;
    struct mount *old_mnt = vfs_resolve_mount(oldpath, &oldrel);
    struct mount *new_mnt = vfs_resolve_mount(newpath, &newrel);
    if (!old_mnt || !new_mnt) return -ENOENT;
    if (old_mnt != new_mnt) return -EXDEV; /* cross-device rename */
    if (old_mnt->i_ops && old_mnt->i_ops->rename)
        return old_mnt->i_ops->rename(old_mnt, oldrel, newrel);
    return -ENOENT;
}

/* ── link ───────────────────────────────────────── */

int vfs_link(const char *oldpath, const char *newpath) {
    const char *oldrel, *newrel;
    struct mount *old_mnt = vfs_resolve_mount(oldpath, &oldrel);
    struct mount *new_mnt = vfs_resolve_mount(newpath, &newrel);
    if (!old_mnt || !new_mnt) return -ENOENT;
    if (old_mnt != new_mnt) return -EXDEV;
    if (old_mnt->i_ops && old_mnt->i_ops->link)
        return old_mnt->i_ops->link(old_mnt, oldrel, newrel);
    return -ENOSYS;
}
