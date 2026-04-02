/* CosmoRT VFS — mount table and path dispatch */

#include "fs/vfs.h"
#include "hw/serial.h"

/* ── Static mount table ─────────────────────────── */

static struct mount mounts[MOUNT_MAX];
static int mount_count;

int vfs_mount(const char *path, struct super_ops *s_ops,
              struct inode_ops *i_ops, struct file_ops *f_ops, void *fs_data) {
    if (mount_count >= MOUNT_MAX) return -ENOMEM;

    /* Compute path length (exclude trailing slash unless root) */
    int len = 0;
    while (path[len]) len++;
    if (len > 1 && path[len - 1] == '/') len--;

    /* Insert sorted by pathlen descending (longest prefix first) */
    int pos = mount_count;
    for (int i = 0; i < mount_count; i++) {
        if (len > mounts[i].pathlen) { pos = i; break; }
    }
    for (int i = mount_count; i > pos; i--)
        mounts[i] = mounts[i - 1];

    mounts[pos].path    = path;
    mounts[pos].pathlen = len;
    mounts[pos].s_ops   = s_ops;
    mounts[pos].i_ops   = i_ops;
    mounts[pos].f_ops   = f_ops;
    mounts[pos].fs_data = fs_data;
    mount_count++;

    serial_puts("vfs: mount ");
    serial_puts(path);
    serial_puts(" (");
    if (i_ops) {
        /* Walk the extern names to find a label — cheap hack for debug */
        serial_puts("ok");
    }
    serial_puts(")\n");
    return 0;
}

/* Longest-prefix match. Returns mount entry and sets *relpath to
 * the path relative to the mount point (always starts with '/').
 *
 * Example: path="/proc/self/maps", mount="/proc"
 *   → relpath="/self/maps", returns &mounts[proc]
 *
 * path="/tmp/foo", mount="/tmp" (tmpfs)
 *   → relpath="/foo"
 *
 * path="/bin/sh", mount="/" (ext2)
 *   → relpath="/bin/sh"
 */
struct mount *vfs_resolve_mount(const char *abspath, const char **relpath) {
    for (int i = 0; i < mount_count; i++) {
        struct mount *m = &mounts[i];
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
            /* Exact match: relpath is "/" */
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
    if (idx < 0 || idx >= mount_count) return 0;
    return &mounts[idx];
}

int vfs_mount_count(void) {
    return mount_count;
}
