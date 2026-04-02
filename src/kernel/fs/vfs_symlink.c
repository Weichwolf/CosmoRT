/* CosmoRT VFS — symlink, readlink, lstat
 *
 * Dispatch via mount table.
 */

#include "fs/vfs_internal.h"

/* ── symlink ────────────────────────────────────── */

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EFAULT;
    int tlen = kstrlen(target);
    if (tlen == 0 || tlen >= 256) return -ENAMETOOLONG;

    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(linkpath, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->symlink)
        return mnt->i_ops->symlink(mnt, target, relpath);
    return -ENOENT;
}

/* ── readlink ───────────────────────────────────── */

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return -EINVAL;

    /* /proc/self/exe and /proc/<pid>/exe → executable path */
    const char *pn = procfs_name(path);
    if (pn && procfs_pid_exists(pn) == 2) {
        process_t *p = 0;
        if (pn[0]=='s' && pn[1]=='e' && pn[2]=='l' && pn[3]=='f' && pn[4]=='/') {
            p = proc_current();
        } else {
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

    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->readlink)
        return mnt->i_ops->readlink(mnt, relpath, buf, bufsiz);
    return -EINVAL;
}

/* ── lstat ──────────────────────────────────────── */

int vfs_lstat(const char *path, struct k_stat *buf) {
    /* /proc → delegate to stat (procfs handles exe symlinks) */
    const char *pn = procfs_name(path);
    if (pn) return vfs_stat(path, buf);

    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);
    if (mnt && mnt->i_ops && mnt->i_ops->lstat)
        return mnt->i_ops->lstat(mnt, relpath, buf);
    return -ENOENT;
}
