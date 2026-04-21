/* CosmoRT VFS — mount table and path dispatch
 *
 * Slab-allocated mount entries linked in a list sorted by pathlen descending,
 * so longest-prefix match is simply a forward scan. */

#include "fs/vfs.h"
#include "hw/serial.h"
#include "mm/slab.h"

static slab_t mount_slab;
static int    mount_slab_inited;
static struct mount *mount_head;
static int    mount_count;

static void mount_slab_ensure(void) {
    if (__sync_bool_compare_and_swap(&mount_slab_inited, 0, 1))
        slab_init_dynamic(&mount_slab, (int)sizeof(struct mount), 0);
}

int vfs_mount_flags(const char *path, unsigned long flags,
                    struct super_ops *s_ops, struct inode_ops *i_ops,
                    struct file_ops *f_ops, void *fs_data) {
    mount_slab_ensure();

    /* Compute path length (exclude trailing slash unless root) */
    int len = 0;
    while (path[len]) len++;
    if (len > 1 && path[len - 1] == '/') len--;

    struct mount *m = (struct mount *)slab_alloc(&mount_slab);
    if (!m) return -ENOMEM;

    m->path      = path;
    m->pathlen   = len;
    m->mnt_flags = flags;
    m->s_ops     = s_ops;
    m->i_ops     = i_ops;
    m->f_ops     = f_ops;
    m->fs_data   = fs_data;

    /* Insert sorted by pathlen descending, newer mount shadows older at same
     * pathlen (do_mount over existing mount point — Linux mnt_hash-Verhalten). */
    struct mount **pp = &mount_head;
    while (*pp && (*pp)->pathlen > len)
        pp = &(*pp)->next;
    m->next = *pp;
    *pp = m;
    mount_count++;

    serial_puts("vfs: mount ");
    serial_puts(path);
    serial_puts(" (");
    if (i_ops) serial_puts("ok");
    serial_puts(")\n");
    return 0;
}

/* Longest-prefix match. Returns mount entry and sets *relpath to
 * the path relative to the mount point (always starts with '/'). */
struct mount *vfs_resolve_mount(const char *abspath, const char **relpath) {
    for (struct mount *m = mount_head; m; m = m->next) {
        int plen = m->pathlen;

        /* Root "/" matches everything */
        if (plen == 1 && m->path[0] == '/') {
            *relpath = abspath;
            return m;
        }

        /* Check prefix match */
        int match = 1;
        for (int j = 0; j < plen; j++) {
            if (abspath[j] != m->path[j]) { match = 0; break; }
        }
        if (!match) continue;

        /* After prefix: must be end-of-string or '/' */
        char next = abspath[plen];
        if (next == 0) {
            *relpath = "/";
            return m;
        }
        if (next == '/') {
            *relpath = abspath + plen;
            return m;
        }
        /* Partial match (e.g. "/dev" vs "/devices") — skip */
    }
    return 0;
}

/* Get mount by index (for iteration, e.g. /proc/mounts) */
struct mount *vfs_get_mount(int idx) {
    if (idx < 0) return 0;
    struct mount *m = mount_head;
    while (m && idx > 0) { m = m->next; idx--; }
    return m;
}

int vfs_mount_count(void) {
    return mount_count;
}

int vfs_mount(const char *path, struct super_ops *s_ops,
              struct inode_ops *i_ops, struct file_ops *f_ops, void *fs_data) {
    return vfs_mount_flags(path, 0, s_ops, i_ops, f_ops, fs_data);
}

/* Remount: locate existing mount by exact path, update mnt_flags.
 * Linux mount(2) with MS_REMOUNT requires the mountpoint to exist. */
int vfs_mount_remount(const char *path, unsigned long flags) {
    int len = 0;
    while (path[len]) len++;
    if (len > 1 && path[len - 1] == '/') len--;

    for (struct mount *m = mount_head; m; m = m->next) {
        if (m->pathlen != len) continue;
        int match = 1;
        for (int j = 0; j < len; j++)
            if (m->path[j] != path[j]) { match = 0; break; }
        if (!match) continue;
        m->mnt_flags = (m->mnt_flags & ~MS_RMT_MASK) | (flags & MS_RMT_MASK);
        return 0;
    }
    return -EINVAL;
}

int vfs_mount_writable(struct mount *mnt) {
    if (!mnt) return 0;
    if (mnt->mnt_flags & MS_RDONLY) return -EROFS;
    return 0;
}
