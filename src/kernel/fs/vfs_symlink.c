/* CosmoRT VFS — symlink, readlink, lstat */

#include "fs/vfs_internal.h"

/* ── Symlink operations ──────────────────────────── */

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EFAULT;
    int tlen = kstrlen(target);
    if (tlen == 0 || tlen >= 256) return -ENAMETOOLONG;

    if (!is_ramfs_path(linkpath)) {
        /* ext2 symlink */
        uint64_t ino64 = ext2_walk(linkpath);
        if (ino64 != 0) return -EEXIST;

        const char *basename;
        uint64_t parent_ino64 = ext2_walk_parent(linkpath, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) return -ENOENT;

        struct ext2_inode pip;
        if (ext2_inode_read(parent_ino, &pip) < 0) return -EIO;
        if ((pip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

        return ext2_symlink_create(parent_ino, basename, target);
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
        /* ext2: walk WITHOUT following final symlink */
        /* We need to walk to parent, then lookup the name to get the symlink inode */
        const char *basename;
        uint64_t parent_ino64 = ext2_walk_parent(path, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) return -ENOENT;

        uint32_t ino;
        if (ext2_dir_lookup(parent_ino, basename, &ino) < 0) return -ENOENT;

        return ext2_readlink(ino, buf, bufsiz);
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

static void fill_ext2_symlink_stat(uint32_t ino, struct ext2_inode *ip, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = ino;
    buf->st_dev = 1;
    buf->st_nlink = ip->i_links_count;
    buf->st_size = (int64_t)ip->i_size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)ip->i_blocks;
    buf->st_mode = S_IFLNK | 0777;
    buf->st_atime_sec = (int64_t)ip->i_atime;
    buf->st_mtime_sec = (int64_t)ip->i_mtime;
    buf->st_ctime_sec = (int64_t)ip->i_ctime;
}

int vfs_lstat(const char *path, struct k_stat *buf) {
    const char *pn = procfs_name(path);
    if (pn) return vfs_stat(path, buf);  /* vfs_stat handles exe symlinks */

    if (!is_ramfs_path(path)) {
        /* Walk to parent, lookup name without following symlinks */
        const char *basename;
        uint64_t parent_ino64 = ext2_walk_parent(path, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) {
            /* Could be root "/" */
            if (path[0] == '/' && path[1] == 0) {
                struct ext2_inode ip;
                if (ext2_inode_read(EXT2_ROOT_INO, &ip) < 0) return -EIO;
                fill_ext2_stat(EXT2_ROOT_INO, &ip, buf);
                return 0;
            }
            return -ENOENT;
        }

        uint32_t ino;
        if (ext2_dir_lookup(parent_ino, basename, &ino) < 0) return -ENOENT;

        struct ext2_inode ip;
        if (ext2_inode_read(ino, &ip) < 0) return -EIO;
        if ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
            fill_ext2_symlink_stat(ino, &ip, buf);
            return 0;
        }
        fill_ext2_stat(ino, &ip, buf);
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
