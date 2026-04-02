/* CosmoRT VFS — mkdir, rmdir, unlink, rename, link */

#include "fs/vfs_internal.h"

/* ── Helpers ─────────────────────────────────────── */

/* Callback for rmdir empty-check: counts non-dot entries */
static int rmdir_count_cb(const char *name, uint32_t ino, uint8_t type,
                          uint32_t next_pos, void *arg) {
    (void)ino; (void)type; (void)next_pos;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        return 0; /* skip . and .. */
    (*(int *)arg)++;
    return 1; /* stop on first real entry */
}

/* ── Directory/file mutation ─────────────────────── */

int vfs_mkdir(const char *path) {
    if (!is_ramfs_path(path)) {
        /* Check if exists */
        uint64_t ino64 = ext2_walk(path);
        if (ino64 != 0) return -EEXIST;

        const char *basename;
        uint64_t parent_ino64 = ext2_walk_parent(path, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) return -ENOENT;

        uint32_t new_ino;
        return ext2_mkdir(parent_ino, basename, 0755, &new_ino);
    }

    struct vfs_node *existing = vfs_lookup(path);
    if (existing) return -EEXIST;
    struct vfs_node *n = vfs_create(path, VFS_DIR);
    if (!n) return -ENOENT;
    return 0;
}

/* Remove child from parent's children list (on inode) */
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

int vfs_rmdir(const char *path) {
    if (!is_ramfs_path(path)) {
        const char *basename;
        uint64_t parent_ino64 = ext2_walk_parent(path, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) return -ENOENT;
        uint32_t child_ino;
        if (ext2_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
        struct ext2_inode ip;
        if (ext2_inode_read(child_ino, &ip) < 0) return -EIO;
        if ((ip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;
        /* Check empty — only . and .. should exist */
        int entry_count = 0;
        ext2_dir_iterate(child_ino, 0, rmdir_count_cb, &entry_count);
        if (entry_count > 0) return -ENOTEMPTY;

        int rc = ext2_dir_remove(parent_ino, basename);
        if (rc == 0) {
            ext2_truncate(child_ino, 0);
            ext2_inode_free(child_ino);
            /* Decrement parent link count (child's .. is gone) */
            struct ext2_inode pip;
            if (ext2_inode_read(parent_ino, &pip) == 0 && pip.i_links_count > 0) {
                pip.i_links_count--;
                ext2_inode_write(parent_ino, &pip);
            }
        }
        return rc;
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->inode->type != VFS_DIR) return -ENOTDIR;
    if (node->inode->children) return -ENOTEMPTY;
    if (node == vfs_root_node) return -EINVAL;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    inode_decref(node->inode);
    slab_free(&node_slab, node);
    return 0;
}

int vfs_unlink(const char *path) {
    if (!is_ramfs_path(path)) {
        const char *basename;
        uint64_t parent_ino64 = ext2_walk_parent(path, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) return -ENOENT;
        uint32_t child_ino;
        if (ext2_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
        struct ext2_inode ip;
        if (ext2_inode_read(child_ino, &ip) < 0) return -EIO;
        if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -EISDIR;
        int rc = ext2_dir_remove(parent_ino, basename);
        if (rc == 0) {
            if (ip.i_links_count > 0) ip.i_links_count--;
            ext2_inode_write(child_ino, &ip);
            if (ip.i_links_count == 0 && ext2_open_count(child_ino) == 0) {
                ext2_truncate(child_ino, 0);
                ext2_inode_free(child_ino);
            }
            inotify_event(path, IN_DELETE);
        }
        return rc;
    }

    /* Use nofollow: unlink removes the symlink itself, not the target */
    int lerr = 0;
    struct vfs_node *node = vfs_lookup_nofollow(path, &lerr);
    if (!node) return lerr ? lerr : -ENOENT;
    if (node->inode->type == VFS_DIR) return -EISDIR;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    inode_decref(node->inode);
    slab_free(&node_slab, node);
    inotify_event(path, IN_DELETE);
    return 0;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!is_ramfs_path(oldpath) && !is_ramfs_path(newpath)) {
        const char *old_base;
        uint64_t old_parent64 = ext2_walk_parent(oldpath, &old_base);
        uint32_t old_parent = (uint32_t)old_parent64;
        if (old_parent == 0) return -ENOENT;

        const char *new_base;
        uint64_t new_parent64 = ext2_walk_parent(newpath, &new_base);
        uint32_t new_parent = (uint32_t)new_parent64;
        if (new_parent == 0) return -ENOENT;

        int rc = ext2_rename(old_parent, old_base, new_parent, new_base);
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
        if (dst->inode->type == VFS_DIR && dst->inode->children) return -ENOTEMPTY;
        if (dst->parent) unlink_child(dst->parent, dst);
        inode_decref(dst->inode);
        slab_free(&node_slab, dst);
    }

    const char *basename;
    struct vfs_node *new_parent = lookup_parent(newpath, &basename);
    if (!new_parent || new_parent->inode->type != VFS_DIR) return -ENOENT;

    if (node->parent) unlink_child(node->parent, node);

    kstrncpy(node->name, basename, 256);
    node->parent = new_parent;
    node->next = new_parent->inode->children;
    new_parent->inode->children = node;

    inotify_event(oldpath, IN_MOVED_FROM);
    inotify_event(newpath, IN_MOVED_TO);
    return 0;
}

/* ── Hard link ───────────────────────────────────── */

int vfs_link(const char *oldpath, const char *newpath) {
    struct vfs_node *old_node = vfs_lookup(oldpath);
    if (!old_node) return -ENOENT;
    if (old_node->inode->type == VFS_DIR) return -EPERM;

    struct vfs_node *existing = vfs_lookup(newpath);
    if (existing) return -EEXIST;

    const char *new_name;
    struct vfs_node *parent = lookup_parent(newpath, &new_name);
    if (!parent || parent->inode->type != VFS_DIR) return -ENOENT;

    /* Allocate new dentry, share the same inode */
    struct vfs_node *link = (struct vfs_node *)slab_alloc(&node_slab);
    if (!link) return -ENOMEM;
    kstrncpy(link->name, new_name, 256);
    link->inode = old_node->inode;
    __sync_fetch_and_add(&old_node->inode->refcount, 1);
    link->parent = parent;
    link->next = parent->inode->children;
    parent->inode->children = link;

    inotify_event(oldpath, IN_CREATE);
    return 0;
}
