/* CosmoRT VFS — symlink, readlink, lstat */

#include "fs/vfs_internal.h"

/* ── Symlink operations ──────────────────────────── */

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EFAULT;
    int tlen = kstrlen(target);
    if (tlen == 0 || tlen >= 256) return -ENAMETOOLONG;

    if (!is_ramfs_path(linkpath)) {
        /* CosmoFS symlink: create inode with COSMOFS_TYPE_SYMLINK,
         * store target as file data */
        uint64_t ino = cosmofs_walk(linkpath);
        if (ino != 0) return -EEXIST;

        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(linkpath, &basename);
        if (parent_ino == 0) return -ENOENT;

        struct cosmofs_inode *pip = cosmofs_inode_read(parent_ino);
        if (!pip || pip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;

        uint64_t new_ino = cosmofs_inode_alloc();
        if (new_ino == 0) return -ENOMEM;

        struct cosmofs_inode new_in;
        kmemset(&new_in, 0, sizeof(new_in));
        new_in.type = COSMOFS_TYPE_SYMLINK;
        cosmofs_inode_write(new_ino, &new_in);

        /* Write target path as file data */
        int rc = cosmofs_write(new_ino, target, 0, (size_t)(tlen + 1));
        if (rc < 0) {
            cosmofs_inode_free(new_ino);
            return rc;
        }

        return cosmofs_dir_create(parent_ino, basename, new_ino);
    }

    /* ramfs symlink */
    struct vfs_node *existing = vfs_lookup(linkpath);
    if (existing) return -EEXIST;

    ensure_dirs(linkpath);
    struct vfs_node *n = vfs_create(linkpath, VFS_SYMLINK);
    if (!n) return -ENOMEM;

    /* vfs_create may return existing node with wrong type */
    n->type = VFS_SYMLINK;
    kstrncpy(n->symlink_target, target, 256);
    return 0;
}

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return -EINVAL;

    /* /proc/self/exe or /proc/<pid>/exe → executable path from process_t */
    const char *pn = procfs_name(path);
    if (pn && procfs_pid_exists(pn) == 2) {
        process_t *p = 0;
        if (pn[0]=='s' && pn[1]=='e' && pn[2]=='l' && pn[3]=='f' && pn[4]=='/') {
            p = proc_current();
        } else {
            /* parse numeric pid */
            int pid = 0;
            const char *s = pn;
            while (*s >= '0' && *s <= '9') { pid = pid * 10 + (*s - '0'); s++; }
            if (*s == '/') p = proc_find((uint32_t)pid);
        }
        const char *exe = (p && p->exe_path[0]) ? p->exe_path : "/usr/bin/unknown";
        int len = 0;
        while (exe[len]) len++;
        if ((size_t)len > bufsiz) len = (int)bufsiz;
        kmemcpy(buf, exe, (size_t)len);
        return len;
    }

    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;

        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;
        if (ip->type != COSMOFS_TYPE_SYMLINK) return -EINVAL;

        /* Read target from file data */
        size_t len = ip->size;
        /* size includes NUL, readlink returns without NUL */
        if (len > 0) len--;
        if (len > bufsiz) len = bufsiz;

        return cosmofs_read(ino, buf, 0, len);
    }

    /* ramfs — use nofollow lookup so final symlink isn't resolved,
     * but intermediate symlinks are followed with ELOOP detection. */
    int lookup_err = 0;
    struct vfs_node *node = vfs_lookup_nofollow(path, &lookup_err);
    if (!node) return lookup_err ? lookup_err : -ENOENT;
    if (node->type != VFS_SYMLINK) return -EINVAL;

    int tlen = kstrlen(node->symlink_target);
    if ((size_t)tlen > bufsiz) tlen = (int)bufsiz;
    kmemcpy(buf, node->symlink_target, (size_t)tlen);
    return tlen;
}

/* ── lstat (no symlink follow) ───────────────────── */

static void fill_symlink_stat(struct vfs_node *node, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = node->ino;
    buf->st_nlink = 1;
    int tlen = kstrlen(node->symlink_target);
    buf->st_size = (int64_t)tlen;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)((tlen + 511) / 512);
    buf->st_mode = S_IFLNK | 0777;
}

static void fill_cosmofs_symlink_stat(uint64_t ino, struct cosmofs_inode *ip, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = ino;
    buf->st_dev = 1;
    buf->st_nlink = 1;
    int64_t sz = (int64_t)ip->size;
    if (sz > 0) sz--; /* stored with NUL */
    buf->st_size = sz;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)((sz + 511) / 512);
    buf->st_mode = S_IFLNK | 0777;
    buf->st_atime_sec = (int64_t)(ip->atime / 1000000000ULL);
    buf->st_atime_nsec = (int64_t)(ip->atime % 1000000000ULL);
    buf->st_mtime_sec = (int64_t)(ip->mtime / 1000000000ULL);
    buf->st_mtime_nsec = (int64_t)(ip->mtime % 1000000000ULL);
    buf->st_ctime_sec = (int64_t)(ip->ctime / 1000000000ULL);
    buf->st_ctime_nsec = (int64_t)(ip->ctime % 1000000000ULL);
}

int vfs_lstat(const char *path, struct k_stat *buf) {
    const char *pn = procfs_name(path);
    if (pn) return vfs_stat(path, buf);  /* vfs_stat handles exe symlinks */

    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;
        if (ip->type == COSMOFS_TYPE_SYMLINK) {
            fill_cosmofs_symlink_stat(ino, ip, buf);
            return 0;
        }
        fill_cosmofs_stat(ino, ip, buf);
        return 0;
    }

    int lerr = 0;
    struct vfs_node *node = vfs_lookup_nofollow(path, &lerr);
    if (!node) return lerr ? lerr : -ENOENT;
    if (node->type == VFS_SYMLINK) {
        fill_symlink_stat(node, buf);
        return 0;
    }
    fill_stat(node, buf);
    return 0;
}
