/* CosmoRT VFS — mkdir, rmdir, unlink, rename, link */

#include "fs/vfs_internal.h"

/* ── Directory/file mutation ─────────────────────── */

int vfs_mkdir(const char *path) {
    if (!is_ramfs_path(path)) {
        /* Check if exists */
        uint64_t ino = cosmofs_walk(path);
        if (ino != 0) return -EEXIST;

        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
        if (parent_ino == 0) return -ENOENT;

        uint64_t new_ino = cosmofs_inode_alloc();
        if (new_ino == 0) return -ENOMEM;

        struct cosmofs_inode dir_in;
        kmemset(&dir_in, 0, sizeof(dir_in));
        dir_in.type = COSMOFS_TYPE_DIR;
        cosmofs_inode_write(new_ino, &dir_in);

        return cosmofs_dir_create(parent_ino, basename, new_ino);
    }

    struct vfs_node *existing = vfs_lookup(path);
    if (existing) return -EEXIST;
    struct vfs_node *n = vfs_create(path, VFS_DIR);
    if (!n) return -ENOENT;
    return 0;
}

/* Remove child from parent's linked list */
int unlink_child(struct vfs_node *parent, struct vfs_node *child) {
    struct vfs_node **pp = &parent->children;
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

int vfs_rmdir(const char *path) {
    if (!is_ramfs_path(path)) {
        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
        if (parent_ino == 0) return -ENOENT;
        uint64_t child_ino;
        if (cosmofs_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(child_ino);
        if (!ip || ip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;
        /* Check empty (size == entry count in our dir impl) */
        if (ip->size > 0) return -ENOTEMPTY;
        int rc = cosmofs_dir_remove(parent_ino, basename);
        if (rc == 0) cosmofs_inode_free(child_ino);
        return rc;
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_DIR) return -ENOTDIR;
    if (node->children) return -ENOTEMPTY;
    if (node == vfs_root_node) return -EINVAL;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    slab_free(&node_slab, node);
    return 0;
}

int vfs_unlink(const char *path) {
    if (!is_ramfs_path(path)) {
        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
        if (parent_ino == 0) return -ENOENT;
        uint64_t child_ino;
        if (cosmofs_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(child_ino);
        if (!ip) return -EIO;
        if (ip->type == COSMOFS_TYPE_DIR) return -EISDIR;
        int rc = cosmofs_dir_remove(parent_ino, basename);
        if (rc == 0) {
            cosmofs_truncate(child_ino, 0);
            cosmofs_inode_free(child_ino);
            inotify_event(path, IN_DELETE);
        }
        return rc;
    }

    /* Use nofollow: unlink removes the symlink itself, not the target */
    int lerr = 0;
    struct vfs_node *node = vfs_lookup_nofollow(path, &lerr);
    if (!node) return lerr ? lerr : -ENOENT;
    if (node->type == VFS_DIR) return -EISDIR;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    if (node->data && node->capacity > 0) {
        int npages = (int)((node->capacity + 4095) / 4096);
        if (npages > 0) pages_free(node->data, npages);
    }
    slab_free(&node_slab, node);
    inotify_event(path, IN_DELETE);
    return 0;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!is_ramfs_path(oldpath) && !is_ramfs_path(newpath)) {
        const char *old_base;
        uint64_t old_parent = cosmofs_walk_parent(oldpath, &old_base);
        if (old_parent == 0) return -ENOENT;

        const char *new_base;
        uint64_t new_parent = cosmofs_walk_parent(newpath, &new_base);
        if (new_parent == 0) return -ENOENT;

        int rc = cosmofs_dir_rename(old_parent, old_base, new_parent, new_base);
        if (rc == 0) {
            inotify_event(oldpath, IN_MOVED_FROM);
            inotify_event(newpath, IN_MOVED_TO);
        }
        return rc;
    }

    /* ramfs path */
    struct vfs_node *node = vfs_lookup(oldpath);
    if (!node) return -ENOENT;
    if (node == vfs_root_node) return -EINVAL;

    struct vfs_node *dst = vfs_lookup(newpath);
    if (dst) {
        if (dst->type == VFS_DIR && dst->children) return -ENOTEMPTY;
        if (dst->parent) unlink_child(dst->parent, dst);
        if (dst->type == VFS_FILE && dst->data && dst->capacity > 0) {
            int np = (int)((dst->capacity + 4095) / 4096);
            if (np > 0) pages_free(dst->data, np);
        }
        slab_free(&node_slab, dst);
    }

    const char *basename;
    struct vfs_node *new_parent = lookup_parent(newpath, &basename);
    if (!new_parent || new_parent->type != VFS_DIR) return -ENOENT;

    if (node->parent) unlink_child(node->parent, node);

    kstrncpy(node->name, basename, 256);
    node->parent = new_parent;
    node->next = new_parent->children;
    new_parent->children = node;

    inotify_event(oldpath, IN_MOVED_FROM);
    inotify_event(newpath, IN_MOVED_TO);
    return 0;
}

/* ── Hard link ───────────────────────────────────── */

int vfs_link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -ENOSYS; /* CosmoFS doesn't support hard links yet */
}
