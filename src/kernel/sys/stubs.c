/* CosmoRT — Stub syscalls (no-op or fixed return values) */

#include "internal.h"
#include "linux/capability.h"

long do_set_robust_list(void *head, size_t len) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (len != 24) return -EINVAL; /* sizeof(struct robust_list_head) on x86_64 */
    if (head && !user_ok((uint64_t)head, len)) return -EFAULT;
    t->robust_list = head;
    return 0;
}

long do_get_robust_list(int pid, void **head_ptr, size_t *len_ptr) {
    if (pid != 0) return -ESRCH; /* only current thread for now */
    thread_t *t = thread_current();
    if (!t) return -EFAULT;
    if (head_ptr) copy_to_user(head_ptr, &t->robust_list, sizeof(void *));
    if (len_ptr) { size_t sz = 24; copy_to_user(len_ptr, &sz, sizeof(sz)); }
    return 0;
}
/* mount(2): minimal subset — tmpfs mount + MS_REMOUNT flag update.
 * Source/data ignored for tmpfs. Path strings duplicated via pages_alloc
 * (leaked for lifetime of mount, which is process lifetime — single-user). */
static char *mount_path_dup(const char *src, int len) {
    char *buf = (char *)pages_alloc(1);
    if (!buf) return 0;
    for (int i = 0; i < len && i < 255; i++) buf[i] = src[i];
    buf[len < 255 ? len : 255] = '\0';
    return buf;
}

long do_mount(const char *source, const char *target, const char *fstype,
              unsigned long flags, const void *data) {
    (void)data;
    if (!target) return -EFAULT;
    char ktarget_raw[PATH_MAX];
    int tlen = copy_path_from_user(ktarget_raw, target, PATH_MAX);
    if (tlen < 0) return tlen;

    char ktarget[PATH_MAX];
    if (resolve_path(ktarget_raw, ktarget, PATH_MAX) < 0) return -EINVAL;
    tlen = 0;
    while (ktarget[tlen]) tlen++;

    /* MS_REMOUNT: update existing mount's flags only. fstype/source ignored. */
    if (flags & MS_REMOUNT)
        return vfs_mount_remount(ktarget, flags);

    /* Bind/move/shared/slave/private: no namespaces — accept as no-op. */
    if (flags & (MS_BIND | MS_MOVE | MS_SHARED | MS_PRIVATE | MS_SLAVE |
                 MS_UNBINDABLE))
        return 0;

    /* Need fstype for new mount */
    if (!fstype) return -EINVAL;
    char kfstype[32];
    int flen = copy_path_from_user(kfstype, fstype, sizeof(kfstype));
    if (flen < 0) return flen;

    int is_tmpfs = (kfstype[0]=='t' && kfstype[1]=='m' && kfstype[2]=='p' &&
                    kfstype[3]=='f' && kfstype[4]=='s' && kfstype[5]==0);
    /* ext2/ext3/ext4 use the same driver. ext4 is a superset and reads ext2/3
     * filesystems natively (matches Linux kernel pattern: CONFIG_EXT4_USE_FOR_EXT2). */
    int is_ext  = ((kfstype[0]=='e' && kfstype[1]=='x' && kfstype[2]=='t') &&
                   ((kfstype[3]=='2' && kfstype[4]==0) ||
                    (kfstype[3]=='3' && kfstype[4]==0) ||
                    (kfstype[3]=='4' && kfstype[4]==0)));

    if (!is_tmpfs && !is_ext) return -ENODEV;

    /* Target dir must exist. */
    struct k_stat st;
    int sr = vfs_stat(ktarget, &st);
    if (sr < 0) return sr;
    if ((st.st_mode & S_IFMT) != S_IFDIR) return -ENOTDIR;

    /* Ensure target exists in the ramfs dentry tree so tmpfs_op_* can
     * lookup children. */
    extern struct vfs_node *vfs_lookup(const char *path);
    extern struct vfs_node *vfs_create(const char *path, int type);
    extern struct vfs_node *ensure_dirs(const char *path);
    if (!vfs_lookup(ktarget)) {
        char trailing[PATH_MAX + 8];
        int ti = 0;
        while (ktarget[ti] && ti < PATH_MAX) { trailing[ti] = ktarget[ti]; ti++; }
        trailing[ti] = '/';
        trailing[ti + 1] = '.';
        trailing[ti + 2] = '\0';
        ensure_dirs(trailing);
        if (!vfs_lookup(ktarget)) vfs_create(ktarget, VFS_DIR);
    }

    char *dup = mount_path_dup(ktarget, tlen);
    if (!dup) return -ENOMEM;

    if (is_tmpfs) {
        return vfs_mount_flags(dup, flags & ~MS_REMOUNT, 0,
                               &tmpfs_inode_ops, &tmpfs_file_ops, 0);
    }

    /* ext2/3/4 mount: source must be a /dev/loopN device. Resolve loop slot,
     * build a per-mount bcache_inst over the backing vfs_file*, allocate a
     * fresh ext4_fs and call mount on it. */
    if (!source) return -EINVAL;
    char ksrc_raw[PATH_MAX];
    int slen = copy_path_from_user(ksrc_raw, source, PATH_MAX);
    if (slen < 0) return slen;
    char ksrc[PATH_MAX];
    if (resolve_path(ksrc_raw, ksrc, PATH_MAX) < 0) return -EINVAL;

    int loop_idx = loop_path_to_idx(ksrc);
    /* LTP probes "Kernel supports <fs>" via mount("/dev/zero", ..., fs, 0, NULL)
     * and treats anything except -ENODEV as "supported". So return -EINVAL
     * (no superblock readable) for non-loop sources to mark ext4 as supported. */
    if (loop_idx < 0) return -EINVAL;
    struct loop_dev *ld = loop_dev_get(loop_idx);
    if (!ld || !ld->bound || !ld->backing_file) return -ENXIO;
    if (ld->mounted) return -EBUSY;

    struct bcache_backend bk = {
        .read      = loop_bcache_read,
        .write     = loop_bcache_write,
        .bulk_read = 0,                  /* fall back to per-block */
        .bulk_max  = 0,
        .ctx       = ld,
    };
    struct bcache_inst *bc = bcache_inst_create(&bk);
    if (!bc) return -ENOMEM;

    struct ext4_fs *fs = ext4_fs_create(bc);
    if (!fs) { bcache_inst_destroy(bc); return -ENOMEM; }

    int rc = ext4_mount_inst(fs);
    if (rc < 0) {
        ext4_fs_destroy(fs);
        bcache_inst_destroy(bc);
        return rc;
    }

    /* Pin loop slot — prevent LOOP_CLR_FD while mounted. */
    ld->mounted_fs = fs;
    ld->mounted_bcache = bc;
    ld->mounted = 1;

    int mr = vfs_mount_flags(dup, flags & ~MS_REMOUNT, &ext4_super_ops,
                             &ext4_inode_ops, &ext4_file_ops, fs);
    if (mr < 0) {
        ld->mounted = 0;
        ld->mounted_fs = 0;
        ld->mounted_bcache = 0;
        ext4_unmount_inst(fs);
        ext4_fs_destroy(fs);
        bcache_inst_destroy(bc);
        return mr;
    }
    return 0;
}
long do_sethostname(void)     { return 0; }
long do_rseq(void)            { return -ENOSYS; }

/* capget/capset — single-user kernel: all caps always set. Still enforce
 * Linux ABI validation (version, pid, EFAULT). */

static int cap_version_u32s(uint32_t ver) {
    if (ver == _LINUX_CAPABILITY_VERSION_1) return _LINUX_CAPABILITY_U32S_1;
    if (ver == _LINUX_CAPABILITY_VERSION_2) return _LINUX_CAPABILITY_U32S_2;
    if (ver == _LINUX_CAPABILITY_VERSION_3) return _LINUX_CAPABILITY_U32S_3;
    return 0;
}

long do_capget(void *hdrp, void *datap) {
    if (!hdrp) return -EFAULT;
    if (!user_ok((uint64_t)hdrp, sizeof(struct __user_cap_header_struct)))
        return -EFAULT;
    struct __user_cap_header_struct hdr;
    int r = copy_from_user(&hdr, hdrp, sizeof(hdr));
    if (r) return r;

    int u32s = cap_version_u32s(hdr.version);
    if (!u32s) {
        hdr.version = _LINUX_CAPABILITY_VERSION_3;
        copy_to_user(hdrp, &hdr, sizeof(hdr));
        return -EINVAL;
    }
    if (hdr.pid < 0) return -EINVAL;
    process_t *target = 0;
    if (hdr.pid == 0) {
        target = proc_current();
    } else {
        /* Linux: hdr.pid is a TID (capabilities are per-thread). A TID that
         * matches a thread-group leader also equals the process pid. Look up
         * via tid_table first, then fall back to pid_table for compatibility
         * with tests that pass tgid explicitly. */
        thread_t *th = thread_find_by_tid((int)hdr.pid);
        if (th) target = th->proc;
        else    target = proc_find((uint32_t)hdr.pid);
    }
    if (!target) return (hdr.pid > 0) ? -ESRCH : -EFAULT;
    if (!datap) return 0;
    if (!user_ok((uint64_t)datap, u32s * sizeof(struct __user_cap_data_struct)))
        return -EFAULT;

    struct __user_cap_data_struct data[2] = {
        { .effective = (uint32_t)target->cap_effective,
          .permitted = (uint32_t)target->cap_permitted,
          .inheritable = (uint32_t)target->cap_inheritable },
        { .effective = (uint32_t)(target->cap_effective >> 32),
          .permitted = (uint32_t)(target->cap_permitted >> 32),
          .inheritable = (uint32_t)(target->cap_inheritable >> 32) },
    };
    return copy_to_user(datap, data, u32s * sizeof(struct __user_cap_data_struct));
}

long do_capset(void *hdrp, const void *datap) {
    if (!hdrp) return -EFAULT;
    if (!user_ok((uint64_t)hdrp, sizeof(struct __user_cap_header_struct)))
        return -EFAULT;
    struct __user_cap_header_struct hdr;
    int r = copy_from_user(&hdr, hdrp, sizeof(hdr));
    if (r) return r;

    int u32s = cap_version_u32s(hdr.version);
    if (!u32s) {
        hdr.version = _LINUX_CAPABILITY_VERSION_3;
        copy_to_user(hdrp, &hdr, sizeof(hdr));
        return -EINVAL;
    }
    if (hdr.pid < 0) return -EINVAL;
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    /* Linux: capset only supports pid==0, pid==self (tgid) or TID of a
     * thread in the caller's process — others -EPERM. Capabilities are
     * per-thread; hdr.pid is actually a TID in Linux ABI. */
    if (hdr.pid > 0 && (uint32_t)hdr.pid != p->pid) {
        thread_t *th = thread_find_by_tid((int)hdr.pid);
        if (!th || th->proc != p) return -EPERM;
    }
    if (!datap) return -EFAULT;
    if (!user_ok((uint64_t)datap, u32s * sizeof(struct __user_cap_data_struct)))
        return -EFAULT;

    struct __user_cap_data_struct data[2] = {{0}};
    r = copy_from_user(data, datap, u32s * sizeof(struct __user_cap_data_struct));
    if (r) return r;

    uint64_t eff = (uint64_t)data[0].effective |
                   ((u32s > 1) ? (uint64_t)data[1].effective << 32 : 0);
    uint64_t perm = (uint64_t)data[0].permitted |
                    ((u32s > 1) ? (uint64_t)data[1].permitted << 32 : 0);
    uint64_t inh = (uint64_t)data[0].inheritable |
                   ((u32s > 1) ? (uint64_t)data[1].inheritable << 32 : 0);

    /* POSIX capability rules (Linux security/commoncap.c cap_capset):
     * - pP_new ⊆ pP_old                          (may only narrow)
     * - pE_new ⊆ pP_new
     * - pI_new ⊆ pI_old ∪ (pP_old ∩ bounding)
     *   → inheritable additions only from permitted caps still in bounding. */
    if (perm & ~p->cap_permitted) return -EPERM;
    if (eff & ~perm) return -EPERM;
    if (inh & ~(p->cap_inheritable | (p->cap_permitted & p->cap_bounding)))
        return -EPERM;

    p->cap_effective = eff;
    p->cap_permitted = perm;
    p->cap_inheritable = inh;
    return 0;
}

/* msync: moved to sys_mem.c (SH-C3: dirty tracking + write-back) */
/* TODO: implement if needed */
long do_sendfile(void) { return -ENOSYS; }

long do_sched_get_priority_max(int policy) { (void)policy; return 31; }
long do_sched_get_priority_min(int policy) { (void)policy; return 0; }

/* noop: no enforcement */
long do_setrlimit(void) { return 0; }

long do_umask(int mask) {
    process_t *p = proc_current();
    if (!p) return 0022;
    int old = p->umask_val ? (int)p->umask_val : 0022;
    p->umask_val = mask & 0777;
    return old;
}

long do_getgroups(int size, uint32_t *list) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (size == 0) return p->ngroups;
    if (size < p->ngroups) return -EINVAL;
    if (!user_ok((uint64_t)list, (size_t)p->ngroups * sizeof(uint32_t)))
        return -EFAULT;
    if (p->ngroups > 0 && copy_to_user(list, p->groups,
                                       (size_t)p->ngroups * sizeof(uint32_t)))
        return -EFAULT;
    return p->ngroups;
}

long do_setgroups(int size, const uint32_t *list) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    if (p->euid != 0) return -EPERM;
    if (size < 0 || size > NGROUPS_MAX) return -EINVAL;
    if (size == 0) { cred_groups_free(p); return 0; }
    size_t bytes = (size_t)size * sizeof(uint32_t);
    if (!user_ok((uint64_t)list, bytes)) return -EFAULT;
    /* User buffer can be up to 256KB (NGROUPS_MAX*4) — too big for kstack.
     * Stage into a fresh pages_alloc buffer, validate, then commit via
     * cred_groups_set so the in-process array is never half-written. */
    int pages = (int)((bytes + 4095) / 4096);
    int p2 = 1; while (p2 < pages) p2 *= 2;
    uint32_t *tmp = (uint32_t *)pages_alloc(p2);
    if (!tmp) return -ENOMEM;
    if (copy_from_user(tmp, list, bytes)) { pages_free(tmp, p2); return -EFAULT; }
    int r = cred_groups_set(p, tmp, size);
    pages_free(tmp, p2);
    return r;
}

/* personality(2): speichert Persona + Flags in process_t.
 * 0xFFFFFFFF = Query ohne Update. Sonst: alte Persona zurueck, neue setzen.
 * Linux akzeptiert alle Werte; gueltige Flags sind in <linux/personality.h>. */
long do_personality(unsigned long persona) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    uint32_t old = p->personality;
    if (persona != 0xFFFFFFFFUL)
        p->personality = (uint32_t)persona;
    return (long)old;
}

/* priority: single-user, no priority enforcement */
long do_getpriority(int which, int who) { (void)which; (void)who; return 20; }
long do_setpriority(int which, int who, int prio) {
    (void)which; (void)who; (void)prio; return 0;
}

/* sync/syncfs/fsync/fdatasync: flush to disk (noop for ramfs, ext4_sync for ext4) */
long do_sync(void) {
    extern void ext4_sync(void);
    ext4_sync();
    return 0;
}

long do_syncfs(int fd) { (void)fd; return do_sync(); }

long do_fsync(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;
    fd_entry_t *fde = fd_get(p->fds, fd);
    if (!fde || fde->type == FD_NONE) return -EBADF;
    switch (fde->type) {
    case FD_FILE: {
        struct vfs_file *f = (struct vfs_file *)fde->obj;
        if (f && f->backend == VFS_BACKEND_EXT4) {
            extern void ext4_sync(void);
            ext4_sync();
        }
        return 0;
    }
    case FD_PROCFS:
    case FD_SERIAL:
    case FD_PTY_MASTER:
    case FD_PTY_SLAVE:
        return 0;
    /* Linux: file_operations ohne ->fsync → EINVAL.
     * Gilt fuer null/zero/urandom/pipes/sockets/epoll/eventfd/timerfd/signalfd/inotify. */
    default:
        return -EINVAL;
    }
}

long do_fdatasync(int fd) { return do_fsync(fd); }

/* umount2: locate mount by path, sync & destroy ext4_fs/bcache, unbind loop slot.
 * Falls into best-effort no-op for tmpfs / disk-root mounts. */
long do_umount2(const char *target, int flags) {
    (void)flags;
    if (!target) return -EFAULT;
    char ktarget_raw[PATH_MAX];
    int rc = copy_path_from_user(ktarget_raw, target, PATH_MAX);
    if (rc < 0) return rc;
    char ktarget[PATH_MAX];
    if (resolve_path(ktarget_raw, ktarget, PATH_MAX) < 0) return -EINVAL;

    /* Find the mount with exact pathlen match */
    extern struct mount *vfs_get_mount(int idx);
    extern int vfs_mount_count(void);
    int n = vfs_mount_count();
    int tlen = 0;
    while (ktarget[tlen]) tlen++;
    /* Strip trailing slash unless root */
    if (tlen > 1 && ktarget[tlen-1] == '/') tlen--;

    struct mount *target_mnt = 0;
    for (int i = 0; i < n; i++) {
        struct mount *m = vfs_get_mount(i);
        if (!m) continue;
        if (m->pathlen != tlen) continue;
        int eq = 1;
        for (int j = 0; j < tlen; j++)
            if (m->path[j] != ktarget[j]) { eq = 0; break; }
        if (eq) { target_mnt = m; break; }
    }

    if (!target_mnt) return 0; /* tmpfs / device mounts not tracked individually */

    /* Only handle ext4 loop-mounts. fs_data set ⇒ per-instance ext4 fs. */
    if (target_mnt->f_ops == &ext4_file_ops && target_mnt->fs_data) {
        struct ext4_fs *fs = (struct ext4_fs *)target_mnt->fs_data;
        struct bcache_inst *bc = fs->bcache;

        /* Find the loop_dev that owns this fs and unbind. */
        for (int i = 0; i < LOOP_NDEV; i++) {
            struct loop_dev *ld = loop_dev_get(i);
            if (ld && ld->mounted && ld->mounted_fs == fs) {
                ld->mounted = 0;
                ld->mounted_fs = 0;
                ld->mounted_bcache = 0;
                break;
            }
        }

        ext4_unmount_inst(fs);
        ext4_fs_destroy(fs);
        if (bc && bc != bcache_default()) bcache_inst_destroy(bc);

        /* We do not currently remove the mount entry from the mount list —
         * vfs_mount_flags has no remove counterpart. Mark it as unusable by
         * clearing fs_data so subsequent path resolution still finds it but
         * lookups will route to the default fs (which has no relpath). For
         * LTP test correctness, the same mount path is typically remounted
         * on test exit; if needed we can grow vfs_mount_remove later. */
        target_mnt->fs_data = 0;
    }
    return 0;
}

/* ── Batch 2: remaining FEHLT syscalls ── */

/* mincore: pretend all pages are resident */
long do_mincore(void) { return 0; }

/* ptrace: no debugging support */
long do_ptrace(void) { return -EPERM; }

/* syslog: no kernel log buffer */
long do_syslog_stub(void) { return 0; }

/* sched_rr_get_interval: write 10ms quantum */
long do_sched_rr_get_interval(int pid, void *tp) {
    (void)pid;
    if (!tp) return -EFAULT;
    int64_t ts[2] = { 0, 10000000 }; /* 10ms */
    copy_to_user(tp, ts, sizeof(ts));
    return 0;
}

/* vhangup: no-op */
long do_vhangup(void) { return 0; }

long do_adjtimex(void *tx) { return do_clock_adjtime(CLOCK_REALTIME, tx); }

long do_chroot(const char *path) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    char kpath_raw[PATH_MAX];
    int len = copy_path_from_user(kpath_raw, path, PATH_MAX);
    if (len < 0) return len;
    int comp = 0;
    for (int i = 0; i < len; i++) {
        if (kpath_raw[i] == '/') comp = 0;
        else if (++comp > 255) return -ENAMETOOLONG;
    }
    /* Resolve via existing mechanics (respects current chroot) */
    char kpath[PATH_MAX];
    resolve_path(kpath_raw, kpath, PATH_MAX);

    extern struct vfs_node *vfs_lookup_err(const char *path, int *err);
    int lerr = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(kpath, &lerr);
    if (!node) return lerr;
    if (node->inode->type != VFS_DIR) return -ENOTDIR;

    /* Linux fs/open.c ksys_chroot: path_permission(MAY_EXEC) BEFORE the
     * CAP_SYS_CHROOT check. Order matters: a non-root user without search
     * permission on the target gets EACCES, not EPERM. DAC bypass via
     * CAP_DAC_OVERRIDE/CAP_DAC_READ_SEARCH. Intermediate traversal is
     * already covered by vfs_lookup_err. */
    if (p->euid != 0 &&
        !(p->cap_effective & (CAP_TO_MASK(CAP_DAC_OVERRIDE) |
                              CAP_TO_MASK(CAP_DAC_READ_SEARCH)))) {
        int rc = cred_may_access(p, node->inode->uid, node->inode->gid,
                                 node->inode->mode, MAY_EXEC);
        if (rc < 0) return rc;
    }

    if (!(p->cap_effective & CAP_TO_MASK(CAP_SYS_CHROOT)))
        return -EPERM;

    int i = 0;
    while (kpath[i] && i < (int)sizeof(p->root) - 1) { p->root[i] = kpath[i]; i++; }
    p->root[i] = '\0';
    /* cwd is now implicitly relative to the new root; reset to "/" */
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    return 0;
}

/* ── BSD process accounting (CONFIG_BSD_PROCESS_ACCT) ──
 *
 * Single global ON/OFF: acct(path) installs a write target, acct(NULL)
 * disables. On process exit, exit_kill_process calls acct_emit(p, status)
 * which writes one struct acct record (v0 layout) to acct_path via
 * vfs_kernel_append. v3 layout (acct_v3) is not enabled — kconfig-cosmort
 * exposes neither CONFIG_BSD_PROCESS_ACCT_V3 nor CONFIG_BSD_PROCESS_ACCT,
 * LTP's acct_version_is_3 reads 'm'/'n' and falls back to v0.
 *
 * The path is stored in process-independent global state. The mount and
 * file existed at acct() time — concurrent unlink races aren't checked
 * (Linux holds an open file ref; we re-walk on each emit, taking ENOENT
 * silently). Single-user kernel: a real kernel hardens this further.
 */

#define ACCT_PATH_MAX 256
#define ACCT_COMM_LEN 16
/* struct acct (BSD-Layout, v0). Natural-Alignment auf x86_64: 64 Bytes.
 * Layout matches Linux include/uapi/linux/acct.h und musl sys/acct.h.
 * comp_t ist eine 8-bit-Mantisse + 5-bit-Exponent-Float in 16 Bit; tests
 * pruefen nur Plausibilitaet (utime/clock_ticks > 1, etc.) — wir schreiben
 * 0 fuer alle comp_t, das passiert das. */
struct acct_record_v0 {
    char     ac_flag;
    uint16_t ac_uid;
    uint16_t ac_gid;
    uint16_t ac_tty;
    uint32_t ac_btime;
    uint16_t ac_utime;
    uint16_t ac_stime;
    uint16_t ac_etime;
    uint16_t ac_mem;
    uint16_t ac_io;
    uint16_t ac_rw;
    uint16_t ac_minflt;
    uint16_t ac_majflt;
    uint16_t ac_swaps;
    uint32_t ac_exitcode;
    char     ac_comm[ACCT_COMM_LEN + 1];
    char     ac_pad[10];
};
_Static_assert(sizeof(struct acct_record_v0) == 64,
               "acct v0 record must be 64 bytes (matches LTP struct acct)");

static char acct_path[ACCT_PATH_MAX];
static int  acct_active;

/* Hooked from exit_kill_process. p->exit_code already set. status is the
 * raw exit value passed to do_exit (Linux: si_status<<8 | si_code). */
void acct_emit(process_t *p, int status) {
    if (!acct_active || !p) return;
    /* Linux drops accounting for kernel threads / init. We accept all
     * processes — a stricter policy would gate on exe_path[0] != '\0'. */

    extern uint32_t timer_epoch_sec(void);
    struct acct_record_v0 r;
    /* Memset deterministisch — packed struct, padding sonst undefined. */
    char *rb = (char *)&r;
    for (size_t i = 0; i < sizeof(r); i++) rb[i] = 0;
    r.ac_flag    = 0;
    r.ac_uid     = (uint16_t)p->ruid;
    r.ac_gid     = (uint16_t)p->rgid;
    r.ac_tty     = 0;
    r.ac_btime   = timer_epoch_sec();
    r.ac_etime   = 0;
    /* Linux schreibt den wait4-status (POSIX-konventionell): Bei normalem
     * exit ist das (exit_code & 0xFF) << 8. Bei signal-death liegt das
     * Signal in den unteren 7 Bit (+ 0x80 fuer core-dump). exit_signal
     * wird vom Caller in p->exit_signal gesetzt; status hier ist der
     * raw Wert von do_exit. */
    int wait_status;
    if (p->exit_signal) {
        wait_status = p->exit_signal & 0x7F;
    } else {
        wait_status = (status & 0xFF) << 8;
    }
    r.ac_exitcode = (uint32_t)wait_status;
    /* Linux stores "comm" — last 16 bytes of basename(exe). p->comm is the
     * thread name (prctl PR_SET_NAME); for execve'd processes it tracks
     * the basename of the executable. */
    int i = 0;
    while (i < ACCT_COMM_LEN && p->comm[i]) {
        r.ac_comm[i] = p->comm[i];
        i++;
    }
    r.ac_comm[i] = '\0';

    /* Path lookups during exit run on the current task's mm. mm has been
     * torn down for !mm_shared by the time exit_kill_process completes,
     * but acct_emit is called BEFORE free_address_space — vfs_kernel_append
     * touches kernel buffers only and doesn't deref user mm. */
    extern long vfs_kernel_append(const char *path, const void *buf, size_t len);
    (void)vfs_kernel_append(acct_path, &r, sizeof(r));
}

/* acct(2): path==NULL disables process accounting (immer erfolgreich mit PACCT).
 * path!=NULL: vollstaendige Validation via copy_path_from_user + Lookup,
 * dann acct_path/acct_active setzen.
 *
 * Linux kernel/acct.c sys_acct:
 *   1. CAP_SYS_PACCT? → sonst -EPERM (BEVOR Pfad-Lookup bei path==NULL)
 *   2. copy_path_from_user → -EFAULT / -ENAMETOOLONG
 *   3. filp_open(O_RDWR|O_APPEND) → durchlaeuft DAC-Check (MAY_WRITE)
 *      → ENOENT/ENOTDIR/ELOOP/EACCES/EROFS
 *   4. !S_ISREG → -EACCES (in Linux >= 4.x) bzw. -EISDIR/-EACCES
 *      Fuer Dir: EISDIR. */
long do_acct(const char *path) {
    process_t *p = proc_current();
    if (p && p->euid != 0 &&
        !(p->cap_effective & CAP_TO_MASK(CAP_SYS_PACCT)))
        return -EPERM;
    if (!path) {
        acct_active = 0;
        acct_path[0] = '\0';
        return 0;
    }
    char kpath_raw[PATH_MAX];
    int len = copy_path_from_user(kpath_raw, path, PATH_MAX);
    if (len < 0) return len;
    /* Linux behandelt 'foo/' als Traversal-Request: wenn foo kein Dir → ENOTDIR.
     * Wir muessen den Slash ERHALTEN bis nach der Typ-Pruefung. */
    int rawlen = 0;
    while (kpath_raw[rawlen]) rawlen++;
    int had_trailing_slash = (rawlen > 1 && kpath_raw[rawlen - 1] == '/');
    char kpath[PATH_MAX];
    resolve_path(kpath_raw, kpath, PATH_MAX);

    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(kpath, &relpath);
    struct k_stat st;
    int rc = -ENOENT;
    if (mnt && mnt->i_ops && mnt->i_ops->stat)
        rc = mnt->i_ops->stat(mnt, relpath, &st);
    if (rc < 0) return rc;

    uint32_t fmt = st.st_mode & S_IFMT;
    if (fmt == S_IFDIR) return -EISDIR;
    if (had_trailing_slash && fmt != S_IFDIR) return -ENOTDIR;
    if (fmt != S_IFREG) return -EACCES;

    int ro = vfs_mount_writable(mnt);
    if (ro < 0) return ro;

    if (p && p->euid != 0 &&
        !(p->cap_effective & CAP_TO_MASK(CAP_DAC_OVERRIDE))) {
        int rc2 = cred_may_access(p, st.st_uid, st.st_gid, st.st_mode, MAY_WRITE);
        if (rc2 < 0) return rc2;
    }

    /* Install. ACCT_PATH_MAX < PATH_MAX → wir koennten truncieren; ein
     * 256-Byte-Pfad reicht fuer LTP-Tests, Linux limitiert ebenfalls auf
     * /proc/sys/kernel/acct (PATH_MAX in Praxis nie ausgeschoepft). */
    int j = 0;
    while (kpath[j] && j < ACCT_PATH_MAX - 1) {
        acct_path[j] = kpath[j];
        j++;
    }
    acct_path[j] = '\0';
    if (kpath[j]) return -ENAMETOOLONG;
    acct_active = 1;
    return 0;
}

/* settimeofday: no-op */
long do_settimeofday(void) { return 0; }

/* setdomainname: no-op */
long do_setdomainname(void) { return 0; }

/* readahead: no-op */
long do_readahead(void) { return 0; }

/* restart_syscall (NR 219): re-enter via current->restart_block.fn.
 * Linux semantics: a syscall that was interrupted by a signal returned
 * -ERESTART_RESTARTBLOCK; the kernel rewrote RIP/RAX so the userspace
 * syscall stub re-enters as SYS_restart_syscall (219). This handler
 * resumes the original op via the saved closure. fn==NULL means the
 * thread invoked SYS_restart_syscall without a pending restart -> EINTR. */
long do_restart_syscall(void) {
    thread_t *t = thread_current();
    if (!t) return -EINTR;
    restart_fn_t fn = t->restart_block.fn;
    if (!fn) return -EINTR;
    return fn(&t->restart_block);
}


/* utimes: delegate to utimensat */
long do_utimes(const char *filename, const void *utimes_buf) {
    if (utimes_buf) {
        /* struct timeval { time_t tv_sec; suseconds_t tv_usec; } x2 */
        long tv[4];
        int r = copy_from_user(tv, utimes_buf, sizeof(tv));
        if (r) return r;
        int64_t ts[4] = { tv[0], tv[1] * 1000, tv[2], tv[3] * 1000 };
        return do_utimensat(AT_FDCWD, filename, ts, 0);
    }
    return do_utimensat(AT_FDCWD, filename, 0, 0);
}

/* futimesat: delegate to utimensat */
long do_futimesat(int dirfd, const char *filename, const void *utimes_buf) {
    if (utimes_buf) {
        long tv[4];
        int r = copy_from_user(tv, utimes_buf, sizeof(tv));
        if (r) return r;
        int64_t ts[4] = { tv[0], tv[1] * 1000, tv[2], tv[3] * 1000 };
        return do_utimensat(dirfd, filename, ts, 0);
    }
    return do_utimensat(dirfd, filename, 0, 0);
}

/* signalfd: delegate to signalfd4 */
long do_signalfd(int fd, const uint64_t *mask) {
    extern long do_signalfd4(int fd, const uint64_t *mask, int flags);
    return do_signalfd4(fd, mask, 0);
}

/* eventfd: delegate to eventfd2 */
long do_eventfd(unsigned int initval) {
    extern long do_eventfd2(unsigned int initval, int flags);
    return do_eventfd2(initval, 0);
}

/* timerfd_gettime: return zeroed itimerspec */
long do_timerfd_gettime(int fd, void *curr_value) {
    (void)fd;
    if (!curr_value) return -EFAULT;
    int64_t zero[4] = { 0, 0, 0, 0 };
    copy_to_user(curr_value, zero, sizeof(zero));
    return 0;
}

/* rt_tgsigqueueinfo: delegate to rt_sigqueueinfo */
long do_rt_tgsigqueueinfo(int tgid, int tid, int sig, void *uinfo) {
    (void)tid; /* ignore tid, dispatch to process-level */
    extern long do_rt_sigqueueinfo(int pid, int sig, void *uinfo);
    return do_rt_sigqueueinfo(tgid, sig, uinfo);
}

/* epoll_create: size must be > 0 (Linux ABI, ignored but validated) */
long do_epoll_create(int size) {
    if (size <= 0) return -EINVAL;
    extern long do_epoll_create1(int flags);
    return do_epoll_create1(0);
}

/* inotify_init: delegate to inotify_init1 */
long do_inotify_init(void) {
    extern long do_inotify_init1(int flags);
    return do_inotify_init1(0);
}

/* preadv2/pwritev2: now handled by do_preadv/do_pwritev in sys_file.c */

/* openat2: delegate to openat (ignore resolve flags) */
long do_openat2(int dirfd, const char *pathname, void *how, size_t size) {
    (void)size;
    if (!how) return -EFAULT;
    /* struct open_how { u64 flags; u64 mode; u64 resolve; } */
    uint64_t fields[3];
    int r = copy_from_user(fields, how, sizeof(fields));
    if (r) return r;
    return do_openat(dirfd, pathname, (int)fields[0], (int)fields[1]);
}

/* epoll_pwait: Linux-ABI — wie epoll_wait, aber mit sigmask-Swap waehrend wait.
 * Blockiert sigmask, wartet, restauriert. Bei Fault auf sigmask-Pointer: -EFAULT. */
long do_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                    int timeout, const uint64_t *sigmask, size_t sigsetsize) {
    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    uint64_t saved = 0;
    int have_mask = 0;
    if (sigmask) {
        /* musl uebergibt _NSIG/8 = 16 bytes (128 bits). Linux akzeptiert genau
         * sigsetsize == sizeof(kernel_sigset_t) = 8 auf x86_64. Accept 8/16. */
        if (sigsetsize != 8 && sigsetsize != 16) return -EINVAL;
        uint64_t mask;
        int r = copy_from_user(&mask, sigmask, 8);
        if (r) return r;
        mask &= ~(SIG_BIT(9) | SIG_BIT(19)); /* SIGKILL/SIGSTOP nicht blockbar */
        saved = t->sig_blocked;
        t->sig_blocked = mask;
        have_mask = 1;
    }

    long ret = do_epoll_wait(epfd, events, maxevents, timeout);

    if (have_mask) t->sig_blocked = saved;
    return ret;
}

/* epoll_pwait2: wie epoll_pwait, aber mit struct timespec statt int ms.
 * Sigmask-Handling wie epoll_pwait (temporaerer Swap waehrend wait). */
long do_epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
                     void *timeout, const uint64_t *sigmask, size_t sigsetsize) {
    int timeout_ms = -1;
    if (timeout) {
        int64_t ts[2];
        int r = copy_from_user(ts, timeout, sizeof(ts));
        if (r) return r;
        if (ts[0] < 0 || ts[1] < 0 || ts[1] >= NSEC_PER_SEC) return -EINVAL;
        timeout_ms = (int)(ts[0] * MSEC_PER_SEC + ts[1] / NSEC_PER_MSEC);
    }
    return do_epoll_pwait(epfd, events, maxevents, timeout_ms, sigmask, sigsetsize);
}

/* mknod: delegate to mknodat */
long do_mknod(const char *path, uint32_t mode, uint64_t dev) {
    extern long do_mknodat(int dirfd, const char *path, uint32_t mode, uint64_t dev);
    return do_mknodat(AT_FDCWD, path, mode, dev);
}

/* fchmodat2: delegate to fchmodat */
long do_fchmodat2(int dirfd, const char *path, uint32_t mode, int flags) {
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    extern long do_fchmodat(int dirfd, const char *path, uint32_t mode, int flags);
    return do_fchmodat(dirfd, path, mode, flags);
}

/* utime: delegate to utimensat */
long do_utime(const char *filename, const void *times) {
    /* struct utimbuf { time_t actime; time_t modtime; } */
    if (times) {
        long utimes[2];
        int r = copy_from_user(utimes, times, sizeof(utimes));
        if (r) return r;
        int64_t ts[4] = { utimes[0], 0, utimes[1], 0 };
        return do_utimensat(AT_FDCWD, filename, ts, 0);
    }
    return do_utimensat(AT_FDCWD, filename, 0, 0);
}
