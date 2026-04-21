/* CosmoRT devfs — /dev device nodes
 *
 * Mounted at /dev. Provides: null, zero, urandom, tty, console, ttyN, pts/N.
 * Devices are not vfs_file objects — they use FD_DEVICE or FD_PTY_SLAVE directly.
 */

#include "fs/vfs_internal.h"

#define DEV_NULL    1
#define DEV_ZERO    2
#define DEV_URANDOM 3
#define DEV_TTY     4

/* ── Path parsing helpers ───────────────────────── */

static int parse_int(const char **s) {
    int v = 0;
    while (**s >= '0' && **s <= '9') v = v * 10 + (*(*s)++ - '0');
    return v;
}

/* ── inode_ops ──────────────────────────────────── */

static int devfs_op_stat(struct mount *mnt, const char *relpath, struct k_stat *buf) {
    (void)mnt;
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_blksize = 4096;

    /* /dev itself (relpath == "/") */
    if (relpath[0] == '/' && relpath[1] == 0) {
        buf->st_mode = S_IFDIR | 0755;
        buf->st_ino = 3;
        buf->st_nlink = 2;
        return 0;
    }

    const char *name = relpath;
    if (name[0] == '/') name++;

    /* Check for path traversal through a non-directory device node:
     * e.g. "/null/invalid" → ENOTDIR (null is a char device, not a dir) */
    {
        static const char *devnames[] = {
            "null", "zero", "urandom", "random", "tty", "console", 0
        };
        for (int i = 0; devnames[i]; i++) {
            const char *d = devnames[i];
            const char *n = name;
            while (*d && *n == *d) { n++; d++; }
            if (*d == 0 && *n == '/') return -ENOTDIR;
        }
    }

    if (kstreq(name, "null")) {
        buf->st_mode = S_IFCHR | 0666; buf->st_rdev = 0x0103; buf->st_ino = 5; return 0;
    }
    if (kstreq(name, "zero")) {
        buf->st_mode = S_IFCHR | 0666; buf->st_rdev = 0x0105; buf->st_ino = 6; return 0;
    }
    if (kstreq(name, "urandom") || kstreq(name, "random")) {
        buf->st_mode = S_IFCHR | 0666; buf->st_rdev = 0x0109; buf->st_ino = 7; return 0;
    }
    if (kstreq(name, "tty")) {
        buf->st_mode = S_IFCHR | 0666; buf->st_rdev = 0x0500; buf->st_ino = 8; return 0;
    }
    if (kstreq(name, "console")) {
        buf->st_mode = S_IFCHR | 0600; buf->st_rdev = 0x0501; buf->st_ino = 9; return 0;
    }

    /* /dev/ttyN */
    if (name[0]=='t' && name[1]=='t' && name[2]=='y' && name[3] >= '1' && name[3] <= '9') {
        const char *d = name + 3;
        int vt_num = parse_int(&d);
        if (*d == '\0' && vt_num >= 1 && vt_num <= 12) {
            buf->st_mode = S_IFCHR | 0620;
            buf->st_rdev = (uint64_t)(0x0400 + vt_num);
            buf->st_ino = (uint64_t)(100 + vt_num);
            return 0;
        }
    }

    /* /dev/pts/N */
    if (name[0]=='p' && name[1]=='t' && name[2]=='s' && name[3]=='/') {
        const char *d = name + 4;
        if (*d >= '0' && *d <= '9') {
            int pts_id = parse_int(&d);
            if (*d == '\0' && pts_id >= 0 && pts_id < PTY_DEV_ID_MAX) {
                buf->st_mode = S_IFCHR | 0620;
                buf->st_rdev = (uint64_t)(0x8800 + pts_id);
                buf->st_ino = (uint64_t)(200 + pts_id);
                return 0;
            }
        }
    }

    /* /dev/shm is handled by tmpfs mount, /dev/pts dir */
    if (kstreq(name, "pts")) {
        buf->st_mode = S_IFDIR | 0755;
        buf->st_ino = 4;
        buf->st_nlink = 2;
        return 0;
    }
    if (kstreq(name, "fd")) {
        buf->st_mode = S_IFLNK | 0777;
        buf->st_ino = 10;
        buf->st_size = 13; /* "/proc/self/fd" */
        return 0;
    }
    if (kstreq(name, "stdin") || kstreq(name, "stdout") || kstreq(name, "stderr")) {
        buf->st_mode = S_IFLNK | 0777;
        buf->st_ino = 11;
        buf->st_size = 15;
        return 0;
    }

    return -ENOENT;
}

/* ── Exported ops tables ────────────────────────── */

struct inode_ops devfs_inode_ops = {
    .stat      = devfs_op_stat,
    .lstat     = devfs_op_stat,
};

struct file_ops devfs_file_ops = {0};
