/* CosmoRT VFS — symlink, readlink, lstat
 *
 * Dispatch via mount table.
 */

#include "fs/vfs_internal.h"
#include "net/net_ns.h"

/* ── symlink ────────────────────────────────────── */

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EFAULT;
    int tlen = kstrlen(target);
    if (tlen == 0 || tlen >= 256) return -ENAMETOOLONG;

    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(linkpath, &relpath);
    if (!mnt || !mnt->i_ops || !mnt->i_ops->symlink) return -ENOENT;
    int ro = vfs_mount_writable(mnt);
    if (ro < 0) return ro;
    return mnt->i_ops->symlink(mnt, target, relpath);
}

/* ── readlink ───────────────────────────────────── */

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return -EINVAL;

    /* /proc/self/exe and /proc/<pid>/exe → executable path.
     * /proc/<pid>/ns/{net,time,time_for_children} → "kind:[<id>]" form. */
    const char *pn = procfs_name(path);
    if (pn && procfs_pid_exists(pn) == 2) {
        process_t *p = 0;
        const char *file = 0;
        if (pn[0]=='s' && pn[1]=='e' && pn[2]=='l' && pn[3]=='f' && pn[4]=='/') {
            p = proc_current();
            file = pn + 5;
        } else {
            int pid = 0;
            const char *s = pn;
            while (*s >= '0' && *s <= '9') { pid = pid * 10 + (*s - '0'); s++; }
            if (*s == '/') { p = proc_find((uint32_t)pid); file = s + 1; }
        }

        /* /proc/.../ns/{net,time,time_for_children} */
        if (p && file && file[0]=='n' && file[1]=='s' && file[2]=='/') {
            const char *sub = file + 3;
            const char *kind = 0;
            unsigned long ns_id = 0;
            if (sub[0]=='n' && sub[1]=='e' && sub[2]=='t' && sub[3]==0) {
                kind = "net"; ns_id = p->net_ns ? p->net_ns->ns_id : 0;
            } else if (sub[0]=='t' && sub[1]=='i' && sub[2]=='m' && sub[3]=='e') {
                if (sub[4] == 0) {
                    kind = "time";
                    /* time_namespace has no ns_id in CosmoRT — use pointer
                     * mod 2^32 as a stable per-NS ID. */
                    ns_id = (unsigned long)(uintptr_t)p->time_ns;
                } else if (sub[4]=='_' && sub[5]=='f' && sub[6]=='o' && sub[7]=='r' &&
                           sub[8]=='_' && sub[9]=='c' && sub[10]=='h' && sub[11]=='i' &&
                           sub[12]=='l' && sub[13]=='d' && sub[14]=='r' && sub[15]=='e' &&
                           sub[16]=='n' && sub[17]==0) {
                    kind = "time_for_children";
                    ns_id = (unsigned long)(uintptr_t)p->time_ns_for_children;
                }
            }
            if (kind) {
                /* Render "kind:[<id>]" */
                char out[64];
                int o = 0;
                while (kind[o]) { out[o] = kind[o]; o++; }
                out[o++] = ':'; out[o++] = '[';
                /* uint to ascii */
                char num[24]; int np = 0;
                unsigned long v = ns_id;
                if (v == 0) num[np++] = '0';
                else while (v) { num[np++] = (char)('0' + v % 10); v /= 10; }
                while (np--) out[o++] = num[np];
                out[o++] = ']';
                if ((size_t)o > bufsiz) o = (int)bufsiz;
                kmemcpy(buf, out, (size_t)o);
                return o;
            }
        }

        /* /proc/<pid>/exe → executable path (existing behaviour). */
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
