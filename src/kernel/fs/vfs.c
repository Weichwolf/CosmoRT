/* CosmoRT VFS — core init, mount, alloc, routing helpers */

#include "fs/vfs_internal.h"

/* ── Slab pools ──────────────────────────────────── */

#define VFS_NODE_MAX 256
#define VFS_FILE_MAX 512

static struct vfs_node node_pool[VFS_NODE_MAX];
static struct vfs_file file_pool[VFS_FILE_MAX];
slab_t node_slab;
slab_t file_slab;

struct vfs_node *vfs_root_node;
uint64_t vfs_next_ino = 1;

/* cwd is now per-process (process_t.cwd). Helper to get it: */
char *vfs_get_cwd(void) {
    process_t *p = proc_current();
    return p ? p->cwd : 0;
}

/* ── String helpers ──────────────────────────────── */

int kstrlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void kstrncpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int kstreq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ── procfs routing ──────────────────────────────── */

/* Returns pointer to name after "/proc/" or NULL */
const char *procfs_name(const char *path) {
    if (path[0]=='/' && path[1]=='p' && path[2]=='r' && path[3]=='o' &&
        path[4]=='c') {
        if (path[5] == '\0' || path[5] == '/') {
            const char *name = path + 5;
            if (*name == '/') name++;
            return name; /* "" for /proc, "meminfo" for /proc/meminfo, etc. */
        }
    }
    return 0;
}

/* ── ext2 routing ─────────────────────────────────── */

/* Returns 1 if path should use ramfs (not ext2) */
int is_ramfs_path(const char *path) {
    if (!ext2_mounted()) return 1;
    /* /dev/shm always ramfs */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='s' && path[6]=='h' && path[7]=='m' &&
        (path[8]=='/' || path[8]==0))
        return 1;
    return 0;
}

/* Walk an ext2 path component-by-component, following symlinks.
 * Returns final inode number, or 0 on failure. */
uint64_t ext2_walk(const char *path) {
    if (!path || path[0] != '/') return 0;
    if (path[0] == '/' && path[1] == 0) return EXT2_ROOT_INO;

    /* Mutable copy for symlink restart */
    char buf[512];
    int blen = kstrlen(path);
    if (blen >= (int)sizeof(buf)) return 0;
    for (int i = 0; i <= blen; i++) buf[i] = path[i];

    int symloop = 0;

restart:
    if (symloop > 8) return 0;  /* ELOOP */

    uint32_t cur = EXT2_ROOT_INO;
    char *p = buf + 1;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        /* Extract component */
        char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        char name[256];
        int cplen = len < 255 ? len : 255;
        for (int i = 0; i < cplen; i++) name[i] = start[i];
        name[cplen] = 0;

        uint32_t child;
        if (ext2_dir_lookup(cur, name, &child) < 0)
            return 0;

        /* Check if child is a symlink — resolve transparently */
        struct ext2_inode ip;
        if (ext2_inode_read(child, &ip) == 0 &&
            (ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK && ip.i_size > 0) {
            symloop++;
            char target[256];
            int tlen = ext2_readlink(child, target, sizeof(target) - 1);
            if (tlen <= 0) return 0;
            target[tlen] = 0;

            /* Remaining path after this component */
            int rlen = kstrlen(p);
            int ttlen = tlen;
            if (target[0] == '/') {
                /* Absolute symlink: target + "/" + remaining */
                if (ttlen + 1 + rlen >= (int)sizeof(buf)) return 0;
                for (int i = rlen; i >= 0; i--) buf[sizeof(buf) - 1 - rlen + i] = p[i];
                for (int i = 0; i < ttlen; i++) buf[i] = target[i];
                int off = ttlen;
                if (rlen > 0 && buf[off - 1] != '/') buf[off++] = '/';
                for (int i = 0; i < rlen; i++) buf[off + i] = buf[sizeof(buf) - 1 - rlen + i];
                buf[off + rlen] = 0;
            } else {
                /* Relative symlink: resolve from parent of current component */
                int prefix_len = (int)(start - buf);
                if (prefix_len + ttlen + 1 + rlen >= (int)sizeof(buf)) return 0;
                for (int i = rlen; i >= 0; i--) buf[sizeof(buf) - 1 - rlen + i] = p[i];
                for (int i = 0; i < ttlen; i++) buf[prefix_len + i] = target[i];
                int off = prefix_len + ttlen;
                if (rlen > 0 && buf[off - 1] != '/') buf[off++] = '/';
                for (int i = 0; i < rlen; i++) buf[off + i] = buf[sizeof(buf) - 1 - rlen + i];
                buf[off + rlen] = 0;
            }
            goto restart;
        }

        cur = child;
    }
    return cur;
}

/* Public accessor for execve — walk ext2 path to inode */
uint64_t ext2_walk_path(const char *path) {
    return ext2_walk(path);
}

/* Public accessor for execve — get file size from ext2 inode */
uint64_t ext2_file_size(uint64_t ino) {
    struct ext2_inode ip;
    if (ext2_inode_read((uint32_t)ino, &ip) < 0) return 0;
    if ((ip.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return 0;
    /* i_size holds lower 32 bits; i_dir_acl holds upper 32 bits for regular files */
    uint64_t sz = ip.i_size;
    /* Conservative: only use high bits if the FS actually uses them */
    if (ip.i_dir_acl) sz |= (uint64_t)ip.i_dir_acl << 32;
    return sz;
}

/* Walk to parent directory, extract basename. Returns parent inode or 0. */
uint64_t ext2_walk_parent(const char *path, const char **basename_out) {
    if (!path || path[0] != '/') return 0;

    int len = kstrlen(path);
    int last_slash = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    *basename_out = path + last_slash + 1;
    if (!**basename_out) return 0;

    if (last_slash == 0) return EXT2_ROOT_INO;

    /* Build parent path */
    char parent_path[256];
    int plen = last_slash < 255 ? last_slash : 255;
    for (int i = 0; i < plen; i++) parent_path[i] = path[i];
    parent_path[plen] = 0;

    return ext2_walk(parent_path);
}

/* ── Node allocation ─────────────────────────────── */

struct vfs_node *node_alloc(const char *name, int type) {
    struct vfs_node *n = (struct vfs_node *)slab_alloc(&node_slab);
    if (!n) return 0;
    kstrncpy(n->name, name, 256);
    n->type = type;
    n->data = 0;
    n->size = 0;
    n->capacity = 0;
    n->ino = vfs_next_ino++;
    n->mode = 0755;
    n->uid = 0;
    n->gid = 0;
    { extern uint32_t timer_epoch_sec(void);
      uint32_t now = timer_epoch_sec();
      n->atime = now; n->mtime = now; n->ctime = now; }
    n->symlink_target[0] = 0;
    n->children = 0;
    n->next = 0;
    n->parent = 0;
    return n;
}

struct vfs_file *file_alloc(void) {
    return (struct vfs_file *)slab_alloc(&file_slab);
}

void file_free(struct vfs_file *f) {
    slab_free(&file_slab, f);
}

/* Increment refcount on a vfs_file (for fork fd duplication) */
void vfs_file_incref(struct vfs_file *f) {
    if (f) __sync_fetch_and_add(&f->refcount, 1);
}

/* Free a vfs_file object by external pointer (used by proc_cleanup) */
void vfs_file_free_obj(void *obj) {
    if (!obj) return;
    struct vfs_file *f = (struct vfs_file *)obj;
    if (__sync_sub_and_fetch(&f->refcount, 1) <= 0)
        file_free(f);
}

/* ── Inotify path helper ─────────────────────────── */

/* Build full path from a ramfs node (for inotify events).
 * Returns 1 on success, 0 on failure. */
static int vfs_node_path(struct vfs_node *node, char *buf, int bufsize) {
    if (!node || bufsize < 2) return 0;
    /* Stack of ancestors */
    struct vfs_node *stack[32];
    int depth = 0;
    struct vfs_node *n = node;
    while (n && n != vfs_root_node && depth < 32) {
        stack[depth++] = n;
        n = n->parent;
    }
    int pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0; i--) {
        int nlen = kstrlen(stack[i]->name);
        if (pos + nlen + 1 >= bufsize) return 0;
        for (int j = 0; j < nlen; j++) buf[pos++] = stack[i]->name[j];
        if (i > 0) buf[pos++] = '/';
    }
    buf[pos] = 0;
    return 1;
}

/* Fire IN_MODIFY for a ramfs file node */
void vfs_notify_modify(struct vfs_node *node) {
    if (!node) return;
    char path[256];
    if (vfs_node_path(node, path, 256))
        inotify_event(path, IN_MODIFY);
}

/* ── Init ────────────────────────────────────────── */

void vfs_init(void) {
    slab_init(&node_slab, node_pool, (int)sizeof(struct vfs_node), VFS_NODE_MAX);
    slab_init(&file_slab, file_pool, (int)sizeof(struct vfs_file), VFS_FILE_MAX);
    vfs_root_node = node_alloc("/", VFS_DIR);
    serial_puts("vfs: init (ramfs)\n");
}

/* ── Open/Close ──────────────────────────────────── */

int vfs_open(const char *path, int flags, int mode) {
    (void)mode;

    /* Device files */
    /* /dev/console → PTY slave 0 (VT0) */
    if (kstreq(path, "/dev/console")) {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        int fd = fd_alloc(&p->fds, FD_PTY_SLAVE, (void *)0L, flags & 3);
        return fd < 0 ? -EMFILE : fd;
    }
    /* /dev/tty → current process's PTY (controlling terminal) */
    if (kstreq(path, "/dev/tty")) {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        /* Find the PTY from existing fds (stdin is typically PTY_SLAVE) */
        for (int i = 0; i < 3; i++) {
            if (p->fds.entries[i].type == FD_PTY_SLAVE) {
                int fd = fd_alloc(&p->fds, FD_PTY_SLAVE,
                                  p->fds.entries[i].obj, flags & 3);
                return fd < 0 ? -EMFILE : fd;
            }
        }
        return -ENOTTY; /* no controlling terminal */
    }
    if (kstreq(path, "/dev/null") || kstreq(path, "/dev/zero") ||
        kstreq(path, "/dev/urandom") || kstreq(path, "/dev/random")) {
        int devid = 0;
        if (kstreq(path, "/dev/null"))    devid = DEV_NULL;
        if (kstreq(path, "/dev/zero"))    devid = DEV_ZERO;
        if (kstreq(path, "/dev/urandom") || kstreq(path, "/dev/random"))
                                           devid = DEV_URANDOM;
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        int fd = fd_alloc(&p->fds, FD_DEVICE, (void *)(uintptr_t)devid, flags & 3);
        return fd < 0 ? -EMFILE : fd;
    }

    /* /proc directory itself? */
    if (kstreq(path, "/proc") || kstreq(path, "/proc/")) {
        procfs_fd_t *pf = procfs_fd_alloc();
        if (!pf) return -ENOMEM;
        pf->handle = -1;  /* sentinel: directory listing, not a file */
        pf->offset = 0;

        process_t *p = proc_current();
        if (!p) { procfs_fd_free(pf); return -EFAULT; }

        int fd = fd_alloc(&p->fds, FD_PROCFS, pf, O_RDONLY);
        if (fd < 0) { procfs_fd_free(pf); return -EMFILE; }
        return fd;
    }

    /* procfs path? */
    const char *pname = procfs_name(path);
    if (pname) {
        int handle = procfs_open(pname);
        if (!handle) {
            /* Try per-PID dynamic path (e.g. /proc/123/stat) */
            int ptype = procfs_pid_exists(pname);
            if (ptype == 0) return -ENOENT;
            if (ptype == 2) return -ELOOP; /* symlink — must readlink, not open */

            procfs_fd_t *pf = procfs_fd_alloc();
            if (!pf) return -ENOMEM;
            pf->handle = (ptype == 3) ? -3 : -2; /* -3 = fd directory, -2 = per-pid file */
            pf->offset = 0;
            int ni = 0;
            while (ni < 63 && pname[ni]) { pf->name[ni] = pname[ni]; ni++; }
            pf->name[ni] = 0;

            process_t *p = proc_current();
            if (!p) { procfs_fd_free(pf); return -EFAULT; }
            int fd = fd_alloc(&p->fds, FD_PROCFS, pf, O_RDONLY);
            if (fd < 0) { procfs_fd_free(pf); return -EMFILE; }
            return fd;
        }

        procfs_fd_t *pf = procfs_fd_alloc();
        if (!pf) return -ENOMEM;
        pf->handle = handle;
        pf->offset = 0;

        process_t *p = proc_current();
        if (!p) { procfs_fd_free(pf); return -EFAULT; }

        int fd = fd_alloc(&p->fds, FD_PROCFS, pf, O_RDONLY);
        if (fd < 0) { procfs_fd_free(pf); return -EMFILE; }
        return fd;
    }

    /* ext2 path? */
    if (!is_ramfs_path(path)) {
        uint64_t ino64 = ext2_walk(path);
        uint32_t ino = (uint32_t)ino64;

        if (ino != 0 && (flags & O_CREAT) && (flags & O_EXCL))
            return -EEXIST;

        if (ino == 0 && (flags & O_CREAT)) {
            /* Create file on ext2 */
            const char *basename;
            uint64_t parent_ino64 = ext2_walk_parent(path, &basename);
            uint32_t parent_ino = (uint32_t)parent_ino64;
            if (parent_ino == 0) return -ENOENT;

            /* Ensure parent is a directory */
            struct ext2_inode pip;
            if (ext2_inode_read(parent_ino, &pip) < 0) return -EIO;
            if ((pip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

            uint32_t new_ino;
            int rc = ext2_create(parent_ino, basename, 0644, &new_ino);
            if (rc < 0) return rc;
            ino = new_ino;
            inotify_event(path, IN_CREATE);
        }

        if (ino == 0) return -ENOENT;

        struct ext2_inode ip;
        if (ext2_inode_read(ino, &ip) < 0) return -EIO;

        int is_dir = ((ip.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);

        if ((flags & O_DIRECTORY) && !is_dir)
            return -ENOTDIR;

        struct vfs_file *f = file_alloc();
        if (!f) return -ENOMEM;

        f->type = is_dir ? VFS_DIR : VFS_FILE;
        f->flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_CLOEXEC);
        f->refcount = 1;
        f->backend = VFS_BACKEND_EXT2;
        f->offset = 0;
        f->node = 0;
        f->disk_ino = ino;
        f->disk_size = ip.i_size;
        f->disk_dir_ino = 0;
        kstrncpy(f->path, path, 256);

        if ((flags & O_TRUNC) && !is_dir)
            ext2_truncate(ino, 0);

        process_t *p = proc_current();
        if (!p) { file_free(f); return -EFAULT; }

        int fd = fd_alloc(&p->fds, FD_FILE, f, f->flags);
        if (fd < 0) { file_free(f); return -EMFILE; }
        return fd;
    }

    /* ramfs path */
    struct vfs_node *node = vfs_lookup(path);

    /* O_NOFOLLOW: reject if final component is a symlink */
    if (node && (flags & O_NOFOLLOW)) {
        int lerr = 0;
        struct vfs_node *raw = vfs_lookup_nofollow(path, &lerr);
        if (raw && raw->type == VFS_SYMLINK) return -ELOOP;
    }

    if (node && (flags & O_CREAT) && (flags & O_EXCL))
        return -EEXIST;

    if (!node && (flags & O_CREAT)) {
        ensure_dirs(path);
        node = vfs_create(path, VFS_FILE);
        if (node) inotify_event(path, IN_CREATE);
    }
    if (!node) return -ENOENT;

    if ((flags & O_DIRECTORY) && node->type != VFS_DIR)
        return -ENOTDIR;

    struct vfs_file *f = file_alloc();
    if (!f) return -ENOMEM;

    f->type = node->type;
    f->flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_CLOEXEC);
    f->refcount = 1;
    f->backend = VFS_BACKEND_RAM;
    f->offset = 0;
    f->node = node;
    f->disk_ino = 0;
    f->disk_size = 0;
    f->disk_dir_ino = 0;
    kstrncpy(f->path, path, 256);

    if ((flags & O_TRUNC) && node->type == VFS_FILE) {
        node->size = 0;
    }

    process_t *p = proc_current();
    if (!p) { file_free(f); return -EFAULT; }

    int fd = fd_alloc(&p->fds, FD_FILE, f, f->flags);
    if (fd < 0) { file_free(f); return -EMFILE; }

    return fd;
}

int vfs_close(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (f) {
        if (__sync_sub_and_fetch(&f->refcount, 1) <= 0)
            file_free(f);
    }

    fd_close(&p->fds, fd);
    return 0;
}

/* ── CWD ─────────────────────────────────────────── */

int vfs_getcwd(char *buf, size_t size) {
    char *cwd = vfs_get_cwd();
    if (!cwd) return -EFAULT;
    int len = kstrlen(cwd);
    if ((size_t)(len + 1) > size) return -ERANGE;
    kmemcpy(buf, cwd, (size_t)(len + 1));
    return len;
}

int vfs_chdir(const char *path) {
    if (!is_ramfs_path(path)) {
        uint64_t ino64 = ext2_walk(path);
        uint32_t ino = (uint32_t)ino64;
        if (ino == 0) return -ENOENT;
        struct ext2_inode ip;
        if (ext2_inode_read(ino, &ip) < 0) return -EIO;
        if ((ip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;
        char *cwd = vfs_get_cwd();
        if (!cwd) return -EFAULT;
        kstrncpy(cwd, path, 256);
        return 0;
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_DIR) return -ENOTDIR;

    char *cwd = vfs_get_cwd();
    if (!cwd) return -EFAULT;
    kstrncpy(cwd, path, 256);
    return 0;
}

/* ── Populate ramfs ──────────────────────────────── */

int vfs_add_file(const char *path, const void *data, size_t len) {
    /* Ensure parent directories exist */
    ensure_dirs(path);

    struct vfs_node *node = vfs_create(path, VFS_FILE);
    if (!node) return -ENOMEM;

    if (len > 0) {
        if (grow_file(node, len) < 0) return -ENOMEM;
        kmemcpy(node->data, data, len);
        node->size = len;
    }

    serial_puts("vfs: added ");
    serial_puts(path);
    serial_puts(" (");
    /* Print size */
    char t[12]; int ti = 0;
    size_t v = len;
    do { t[ti++] = '0' + (char)(v % 10); v /= 10; } while (v);
    while (ti--) serial_putchar(t[ti]);
    serial_puts(" bytes)\n");

    return 0;
}

/* ── Kernel-internal file append (no process context) ── */

long vfs_kernel_append(const char *path, const void *buf, size_t len) {
    if (!buf || !len) return 0;

    if (is_ramfs_path(path)) {
        /* ramfs: find or create the node, append directly */
        struct vfs_node *node = vfs_lookup(path);
        if (!node) {
            ensure_dirs(path);
            node = vfs_create(path, VFS_FILE);
            if (!node) return -ENOMEM;
        }
        size_t end = node->size + len;
        if (end > node->capacity) {
            if (grow_file(node, end) < 0) return -ENOMEM;
        }
        kmemcpy(node->data + node->size, buf, len);
        node->size = end;
        return (long)len;
    }

    /* ext2: walk path, create if missing, append via ext2_write */
    uint64_t ino64 = ext2_walk(path);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) {
        const char *basename;
        uint64_t parent_ino64 = ext2_walk_parent(path, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) return -ENOENT;

        struct ext2_inode pip;
        if (ext2_inode_read(parent_ino, &pip) < 0) return -EIO;
        if ((pip.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -ENOTDIR;

        uint32_t new_ino;
        int rc = ext2_create(parent_ino, basename, 0644, &new_ino);
        if (rc < 0) return rc;
        ino = new_ino;
    }

    struct ext2_inode ip;
    if (ext2_inode_read(ino, &ip) < 0) return -EIO;
    size_t off = ip.i_size;

    int rc = ext2_write(ino, buf, off, len);
    return (long)rc;
}

/* ── Mount ext2 ───────────────────────────────────── */

void vfs_mount_ext2(void) {
    if (ext2_mount() == 0)
        serial_puts("vfs: ext2 mounted as /\n");
    else
        serial_puts("vfs: ext2 mount failed, using ramfs\n");
}

uint64_t vfs_ext2_lookup(const char *path) {
    if (!ext2_mounted()) return 0;
    return ext2_walk(path);
}

int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size) {
    /* Try ramfs first (for embedded binaries like /lib/ld-musl-x86_64.so.1) */
    struct vfs_node *node = vfs_lookup(path);
    if (node && node->type == VFS_FILE && node->data && node->size > 0) {
        size_t sz = node->size;
        int npages = (int)((sz + 4095) / 4096);
        uint8_t *buf = (uint8_t *)pages_alloc(npages);
        if (!buf) return -ENOMEM;
        kmemcpy(buf, node->data, sz);
        *out_data = buf;
        *out_size = sz;
        return 0;
    }

    /* Try ext2 */
    if (ext2_mounted()) {
        uint64_t ino64 = ext2_walk(path);
        uint32_t ino = (uint32_t)ino64;
        if (ino == 0) return -ENOENT;

        struct ext2_inode ip;
        if (ext2_inode_read(ino, &ip) < 0) return -EIO;
        if ((ip.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -EACCES;
        if (ip.i_size == 0) return -ENOEXEC;

        size_t sz = ip.i_size;
        int npages = (int)((sz + 4095) / 4096);
        uint8_t *buf = (uint8_t *)pages_alloc(npages);
        if (!buf) return -ENOMEM;

        /* Read in chunks */
        size_t off = 0;
        while (off < sz) {
            size_t chunk = sz - off;
            if (chunk > 65536) chunk = 65536;
            int r = ext2_read(ino, buf + off, off, chunk);
            if (r < 0) {
                pages_free(buf, npages);
                return r;
            }
            off += (size_t)r;
            if ((size_t)r < chunk) break; /* EOF */
        }

        *out_data = buf;
        *out_size = off;
        return 0;
    }

    return -ENOENT;
}
