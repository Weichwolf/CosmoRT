/* CosmoRT VFS — core init, mount, alloc, routing helpers */

#include "fs/vfs_internal.h"
#include "fs/loop.h"

/* ── Slab pools ──────────────────────────────────── */

#define NAME_MAX      255

slab_t node_slab;
slab_t file_slab;
slab_t inode_slab;

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

/* ── ext4 routing ─────────────────────────────────── */

/* Returns 1 if path should use ramfs (not ext4) */
int is_ramfs_path(const char *path) {
    if (!ext4_mounted()) return 1;
    /* /dev/shm always ramfs */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='s' && path[6]=='h' && path[7]=='m' &&
        (path[8]=='/' || path[8]==0))
        return 1;
    return 0;
}

/* Walk an ext4 path component-by-component, following symlinks.
 * Returns final inode number, or 0 on failure. */
uint64_t ext4_walk_err(const char *path, int *err) {
    if (!path || path[0] != '/') { if (err) *err = -ENOENT; return 0; }
    if (path[0] == '/' && path[1] == 0) return EXT4_ROOT_INO;

    /* Mutable copy for symlink restart */
    char buf[512];
    int blen = kstrlen(path);
    if (blen >= (int)sizeof(buf)) { if (err) *err = -ENAMETOOLONG; return 0; }
    for (int i = 0; i <= blen; i++) buf[i] = path[i];

    int symloop = 0;

restart:
    if (symloop > 8) { if (err) *err = -ELOOP; return 0; }

    uint32_t cur = EXT4_ROOT_INO;
    char *p = buf + 1;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        /* Extract component */
        char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        if (len > NAME_MAX) { if (err) *err = -ENAMETOOLONG; return 0; }

        char name[256];
        for (int i = 0; i < len; i++) name[i] = start[i];
        name[len] = 0;

        /* Intermediate component must be a directory.
         * Always read the inode (root inode too) so DAC traversal checks
         * see the real mode bits, not a stale stack value. */
        struct ext4_inode cur_ip;
        if (ext4_inode_read(cur, &cur_ip) < 0) { if (err) *err = -EIO; return 0; }
        if ((cur_ip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
            if (err) *err = -ENOTDIR;
            return 0;
        }

        /* DAC: each directory on the path needs MAY_EXEC for non-root. */
        {
            process_t *pcur = proc_current();
            if (pcur && pcur->euid != 0) {
                int rc = cred_may_access(pcur, cur_ip.i_uid, cur_ip.i_gid,
                                         cur_ip.i_mode, MAY_EXEC);
                if (rc < 0) { if (err) *err = rc; return 0; }
            }
        }

        uint32_t child;
        if (ext4_dir_lookup(cur, name, &child) < 0) {
            if (err) *err = -ENOENT;
            return 0;
        }

        /* Check if child is a symlink — resolve transparently */
        struct ext4_inode ip;
        if (ext4_inode_read(child, &ip) == 0 &&
            (ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK && ip.i_size > 0) {
            symloop++;
            char target[256];
            int tlen = ext4_readlink(child, target, sizeof(target) - 1);
            if (tlen <= 0) { if (err) *err = -ENOENT; return 0; }
            target[tlen] = 0;

            /* Remaining path after this component */
            int rlen = kstrlen(p);
            int ttlen = tlen;
            if (target[0] == '/') {
                /* Absolute symlink: target + "/" + remaining */
                if (ttlen + 1 + rlen >= (int)sizeof(buf)) {
                    if (err) *err = -ENAMETOOLONG;
                    return 0;
                }
                for (int i = rlen; i >= 0; i--) buf[sizeof(buf) - 1 - rlen + i] = p[i];
                for (int i = 0; i < ttlen; i++) buf[i] = target[i];
                int off = ttlen;
                if (rlen > 0 && buf[off - 1] != '/') buf[off++] = '/';
                for (int i = 0; i < rlen; i++) buf[off + i] = buf[sizeof(buf) - 1 - rlen + i];
                buf[off + rlen] = 0;
            } else {
                /* Relative symlink: resolve from parent of current component */
                int prefix_len = (int)(start - buf);
                if (prefix_len + ttlen + 1 + rlen >= (int)sizeof(buf)) {
                    if (err) *err = -ENAMETOOLONG;
                    return 0;
                }
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

uint64_t ext4_walk(const char *path) {
    return ext4_walk_err(path, 0);
}

/* Public accessor for execve — walk ext4 path to inode */
uint64_t ext4_walk_path(const char *path) {
    return ext4_walk(path);
}

/* Public accessor for execve — get file size from ext4 inode */
uint64_t ext4_file_size(uint64_t ino) {
    struct ext4_inode ip;
    if (ext4_inode_read((uint32_t)ino, &ip) < 0) return 0;
    if ((ip.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG) return 0;
    /* i_size holds lower 32 bits; i_dir_acl holds upper 32 bits for regular files */
    uint64_t sz = ip.i_size;
    /* Conservative: only use high bits if the FS actually uses them */
    if (ip.i_dir_acl) sz |= (uint64_t)ip.i_dir_acl << 32;
    return sz;
}

/* Walk to parent directory, extract basename. Returns parent inode or 0. */
uint64_t ext4_walk_parent_err(const char *path, const char **basename_out, int *err) {
    if (!path || path[0] != '/') { if (err) *err = -ENOENT; return 0; }

    int len = kstrlen(path);
    int last_slash = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    *basename_out = path + last_slash + 1;
    int blen = len - last_slash - 1;
    if (blen == 0) { if (err) *err = -ENOENT; return 0; }
    if (blen > NAME_MAX) { if (err) *err = -ENAMETOOLONG; return 0; }

    if (last_slash == 0) return EXT4_ROOT_INO;

    /* Build parent path */
    char parent_path[256];
    int plen = last_slash < 255 ? last_slash : 255;
    for (int i = 0; i < plen; i++) parent_path[i] = path[i];
    parent_path[plen] = 0;

    return ext4_walk_err(parent_path, err);
}

uint64_t ext4_walk_parent(const char *path, const char **basename_out) {
    return ext4_walk_parent_err(path, basename_out, 0);
}

/* ── Node/Inode allocation ───────────────────────── */

struct vfs_node *node_alloc(const char *name, int type) {
    struct vfs_node *n = (struct vfs_node *)slab_alloc(&node_slab);
    if (!n) return 0;

    struct vfs_inode *ino = (struct vfs_inode *)slab_alloc(&inode_slab);
    if (!ino) { slab_free(&node_slab, n); return 0; }

    /* Inode */
    ino->type = type;
    ino->pages = 0;
    ino->pages_cap = 0;
    ino->npages = 0;
    ino->size = 0;
    { spinlock_t fresh = SPINLOCK_INIT; ino->lock = fresh; }
    ino->ino = vfs_next_ino++;
    ino->mode = (type == VFS_DIR) ? 0755
              : (type == VFS_SYMLINK) ? 0777
              : 0644;
    ino->uid = 0;
    ino->gid = 0;
    ino->nlink = 1;
    { extern uint32_t timer_epoch_sec(void);
      uint32_t now = timer_epoch_sec();
      ino->atime = now; ino->mtime = now; ino->ctime = now; }
    ino->symlink_target[0] = 0;
    ino->refcount = 1;
    ino->children = 0;

    /* Dentry */
    kstrncpy(n->name, name, 256);
    n->inode = ino;
    n->next = 0;
    n->parent = 0;
    return n;
}

/* Destroy inode: free per-page storage + pages-array container, return slab */
void inode_destroy(struct vfs_inode *ino) {
    extern void page_cache_invalidate_ino(uint64_t ino);
    page_cache_invalidate_ino(ino->ino);
    if (ino->pages) {
        for (size_t i = 0; i < ino->npages; i++) {
            if (ino->pages[i]) page_free(ino->pages[i]);
        }
        size_t bytes = ino->pages_cap * sizeof(uint8_t *);
        int n_container = (int)((bytes + 4095) / 4096);
        int n_round = 1;
        while (n_round < n_container) n_round *= 2;
        if (n_round < 1) n_round = 1;
        pages_free(ino->pages, n_round);
        ino->pages = 0;
        ino->pages_cap = 0;
        ino->npages = 0;
    }
    slab_free(&inode_slab, ino);
}

/* Decrement inode refcount. Destroys at 0. */
void inode_decref(struct vfs_inode *ino) {
    if (!ino) return;
    if (__sync_sub_and_fetch(&ino->refcount, 1) <= 0)
        inode_destroy(ino);
}

/* Destroy dentry: slab_free only, does NOT touch inode. */
void node_destroy(struct vfs_node *node) {
    slab_free(&node_slab, node);
}

/* ── ext4 open inode tracking ────────────────────── */

#define EXT4_OPEN_HASH_SIZE 64
_Static_assert((EXT4_OPEN_HASH_SIZE & (EXT4_OPEN_HASH_SIZE - 1)) == 0,
               "EXT4_OPEN_HASH_SIZE must be power of 2");

typedef struct ext4_open_entry {
    uint32_t ino;
    int count;
    struct ext4_open_entry *next;
} ext4_open_entry_t;

static ext4_open_entry_t *ext4_open_hash[EXT4_OPEN_HASH_SIZE];
static spinlock_t ext4_open_lock = SPINLOCK_INIT;
static slab_t ext4_open_slab;
static int ext4_open_slab_ready;

static inline uint32_t ext4_open_hash_fn(uint32_t ino) {
    return (uint32_t)((ino * 2654435761u) & (EXT4_OPEN_HASH_SIZE - 1));
}

static void ext4_open_ensure_slab(void) {
    if (__builtin_expect(ext4_open_slab_ready, 1)) return;
    slab_init_dynamic(&ext4_open_slab, sizeof(ext4_open_entry_t), 64);
    ext4_open_slab_ready = 1;
}

void ext4_open_inc(uint32_t ino) {
    ext4_open_ensure_slab();
    uint32_t idx = ext4_open_hash_fn(ino);

    uint64_t flags;
    spin_lock_irq(&ext4_open_lock, &flags);
    for (ext4_open_entry_t *e = ext4_open_hash[idx]; e; e = e->next) {
        if (e->ino == ino) {
            e->count++;
            spin_unlock_irq(&ext4_open_lock, flags);
            return;
        }
    }
    spin_unlock_irq(&ext4_open_lock, flags);

    ext4_open_entry_t *ne = slab_alloc(&ext4_open_slab);
    if (!ne) return;
    ne->ino = ino;
    ne->count = 1;

    spin_lock_irq(&ext4_open_lock, &flags);
    for (ext4_open_entry_t *e = ext4_open_hash[idx]; e; e = e->next) {
        if (e->ino == ino) {
            e->count++;
            spin_unlock_irq(&ext4_open_lock, flags);
            slab_free(&ext4_open_slab, ne);
            return;
        }
    }
    ne->next = ext4_open_hash[idx];
    ext4_open_hash[idx] = ne;
    spin_unlock_irq(&ext4_open_lock, flags);
}

/* Returns open count AFTER decrement. Removes entry at 0. */
int ext4_open_dec(uint32_t ino) {
    if (!ext4_open_slab_ready) return 0;
    uint32_t idx = ext4_open_hash_fn(ino);

    uint64_t flags;
    spin_lock_irq(&ext4_open_lock, &flags);
    ext4_open_entry_t **pp = &ext4_open_hash[idx];
    while (*pp) {
        ext4_open_entry_t *e = *pp;
        if (e->ino == ino) {
            int n = --e->count;
            if (n <= 0) {
                *pp = e->next;
                spin_unlock_irq(&ext4_open_lock, flags);
                slab_free(&ext4_open_slab, e);
                return n;
            }
            spin_unlock_irq(&ext4_open_lock, flags);
            return n;
        }
        pp = &e->next;
    }
    spin_unlock_irq(&ext4_open_lock, flags);
    return 0;
}

int ext4_open_count(uint32_t ino) {
    if (!ext4_open_slab_ready) return 0;
    uint32_t idx = ext4_open_hash_fn(ino);

    uint64_t flags;
    spin_lock_irq(&ext4_open_lock, &flags);
    for (ext4_open_entry_t *e = ext4_open_hash[idx]; e; e = e->next) {
        if (e->ino == ino) {
            int n = e->count;
            spin_unlock_irq(&ext4_open_lock, flags);
            return n;
        }
    }
    spin_unlock_irq(&ext4_open_lock, flags);
    return 0;
}

/* ── i_writecount tracking (ETXTBSY semantics) ──────
 *
 * Linux fs/open.c: i_writecount is a signed counter.
 *   > 0: one or more writable opens, exec returns -ETXTBSY.
 *   < 0: deny_write_access from exec; open(W) returns -ETXTBSY.
 *   = 0: idle.
 *
 * Tracked per (backend, ino) for both tmpfs and ext4 so ELF-exec on an
 * ext4 binary denies writer opens on the same inode.
 */
#define WC_HASH_SIZE 128
_Static_assert((WC_HASH_SIZE & (WC_HASH_SIZE - 1)) == 0,
               "WC_HASH_SIZE must be power of 2");

typedef struct wc_entry {
    int       backend;
    uint64_t  ino;
    int       count;        /* signed: +writers or -deny */
    struct wc_entry *next;
} wc_entry_t;

static wc_entry_t *wc_hash[WC_HASH_SIZE];
static spinlock_t  wc_lock = SPINLOCK_INIT;
static slab_t      wc_slab;
static int         wc_slab_ready;

static inline uint32_t wc_hash_fn(int backend, uint64_t ino) {
    uint64_t k = ((uint64_t)backend << 56) ^ ino;
    return (uint32_t)((k * 2654435761ULL) & (WC_HASH_SIZE - 1));
}

static void wc_ensure_slab(void) {
    if (__builtin_expect(wc_slab_ready, 1)) return;
    slab_init_dynamic(&wc_slab, sizeof(wc_entry_t), 32);
    wc_slab_ready = 1;
}

/* Apply delta to (backend,ino) writecount. If result drops to 0, entry is
 * removed. Returns NEW value after applying delta. Caller must check for
 * ETXTBSY conditions before calling (this is an unconditional apply). */
static int wc_apply(int backend, uint64_t ino, int delta) {
    wc_ensure_slab();
    uint32_t idx = wc_hash_fn(backend, ino);
    uint64_t flags;
    spin_lock_irq(&wc_lock, &flags);
    wc_entry_t **pp = &wc_hash[idx];
    while (*pp) {
        wc_entry_t *e = *pp;
        if (e->backend == backend && e->ino == ino) {
            e->count += delta;
            int n = e->count;
            if (n == 0) {
                *pp = e->next;
                spin_unlock_irq(&wc_lock, flags);
                slab_free(&wc_slab, e);
                return 0;
            }
            spin_unlock_irq(&wc_lock, flags);
            return n;
        }
        pp = &e->next;
    }
    spin_unlock_irq(&wc_lock, flags);

    wc_entry_t *ne = slab_alloc(&wc_slab);
    if (!ne) return 0;
    ne->backend = backend;
    ne->ino = ino;
    ne->count = delta;

    spin_lock_irq(&wc_lock, &flags);
    for (wc_entry_t *e = wc_hash[idx]; e; e = e->next) {
        if (e->backend == backend && e->ino == ino) {
            e->count += delta;
            int n = e->count;
            spin_unlock_irq(&wc_lock, flags);
            slab_free(&wc_slab, ne);
            return n;
        }
    }
    ne->next = wc_hash[idx];
    wc_hash[idx] = ne;
    int n = ne->count;
    spin_unlock_irq(&wc_lock, flags);
    if (n == 0) {
        /* Shouldn't happen with non-zero delta, but be safe. */
        return 0;
    }
    return n;
}

static int wc_peek(int backend, uint64_t ino) {
    if (!wc_slab_ready) return 0;
    uint32_t idx = wc_hash_fn(backend, ino);
    uint64_t flags;
    spin_lock_irq(&wc_lock, &flags);
    for (wc_entry_t *e = wc_hash[idx]; e; e = e->next) {
        if (e->backend == backend && e->ino == ino) {
            int n = e->count;
            spin_unlock_irq(&wc_lock, flags);
            return n;
        }
    }
    spin_unlock_irq(&wc_lock, flags);
    return 0;
}

/* Public API:
 *   i_writecount_get_write: open(W) → 0 or -ETXTBSY
 *   i_writecount_put_write: close on writable fd
 *   i_writecount_deny_write: exec → 0 or -ETXTBSY
 *   i_writecount_allow_write: exec teardown */
int i_writecount_get_write(int backend, uint64_t ino) {
    if (wc_peek(backend, ino) < 0) return -ETXTBSY;
    wc_apply(backend, ino, +1);
    return 0;
}

void i_writecount_put_write(int backend, uint64_t ino) {
    wc_apply(backend, ino, -1);
}

int i_writecount_deny_write(int backend, uint64_t ino) {
    if (wc_peek(backend, ino) > 0) return -ETXTBSY;
    wc_apply(backend, ino, -1);
    return 0;
}

void i_writecount_allow_write(int backend, uint64_t ino) {
    wc_apply(backend, ino, +1);
}

void i_writecount_deny_add(int backend, uint64_t ino) {
    wc_apply(backend, ino, -1);
}

int wc_peek_writers(int backend, uint64_t ino) {
    int n = wc_peek(backend, ino);
    return n > 0 ? n : 0;
}

/* ── File alloc/free ─────────────────────────────── */

struct vfs_file *file_alloc(void) {
    struct vfs_file *f = (struct vfs_file *)slab_alloc(&file_slab);
    if (f) { f->f_owner = 0; f->f_sig = 0; }
    return f;
}

void file_free(struct vfs_file *f) {
    slab_free(&file_slab, f);
}

/* Increment refcount on a vfs_file (for fork fd duplication) */
void vfs_file_incref(struct vfs_file *f) {
    if (f) __sync_fetch_and_add(&f->refcount, 1);
}

/* Release file resources when last refcount drops */
static void vfs_file_release(struct vfs_file *f) {
    extern void flock_release_file(void *vfs_file_ptr);
    flock_release_file(f);
    /* Decrement i_writecount for writable opens (matches the inc in vfs_open). */
    if (f->type == VFS_FILE &&
        ((f->flags & O_ACCMODE) == O_WRONLY ||
         (f->flags & O_ACCMODE) == O_RDWR)) {
        uint64_t ino = f->inode ? f->inode->ino : f->disk_ino;
        if (ino) i_writecount_put_write(f->backend, ino);
    }
    if (f->f_ops && f->f_ops->close)
        f->f_ops->close(f);
}

/* Free a vfs_file object by external pointer (used by proc_cleanup) */
void vfs_file_free_obj(void *obj) {
    if (!obj) return;
    struct vfs_file *f = (struct vfs_file *)obj;
    if (__sync_sub_and_fetch(&f->refcount, 1) <= 0) {
        vfs_file_release(f);
        file_free(f);
    }
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
    slab_init_dynamic(&node_slab,  (int)sizeof(struct vfs_node),  32);
    slab_init_dynamic(&file_slab,  (int)sizeof(struct vfs_file),  64);
    slab_init_dynamic(&inode_slab, (int)sizeof(struct vfs_inode), 32);
    vfs_root_node = node_alloc("/", VFS_DIR);

    /* Register mount points — tmpfs as initial root (ext4 overwrites in vfs_mount_ext4) */
    vfs_mount("/",        0, &tmpfs_inode_ops,  &tmpfs_file_ops,  0);
    vfs_mount("/proc",    0, &procfs_inode_ops, &procfs_file_ops, 0);
    vfs_mount("/dev",     0, &devfs_inode_ops,  &devfs_file_ops,  0);
    vfs_mount("/dev/shm", 0, &tmpfs_inode_ops,  &tmpfs_file_ops,  0);
    loop_init();
    serial_puts("vfs: init\n");
}

/* ── Open/Close ──────────────────────────────────── */

int vfs_open(const char *path, int flags, int mode) {

    /* Device files */
    /* /dev/console → PTY slave 0 (VT0) */
    if (kstreq(path, "/dev/console")) {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        int fd = fd_alloc(&p->fds, FD_PTY_SLAVE, (void *)0L,
                          (flags & 3) | (flags & O_PATH) | (flags & O_CLOEXEC));
        return fd < 0 ? -EMFILE : fd;
    }
    /* /dev/tty → current process's PTY (controlling terminal) */
    if (kstreq(path, "/dev/tty")) {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        /* Find the PTY from existing fds (stdin is typically PTY_SLAVE) */
        for (int i = 0; i < 3; i++) {
            fd_entry_t *e = fd_entry_at(&p->fds, i);
            if (e && e->type == FD_PTY_SLAVE) {
                int fd = fd_alloc(&p->fds, FD_PTY_SLAVE, e->obj,
                                  (flags & 3) | (flags & O_PATH) | (flags & O_CLOEXEC));
                return fd < 0 ? -EMFILE : fd;
            }
        }
        return -ENOTTY; /* no controlling terminal */
    }
    /* /dev/tty1 through /dev/tty12 → PTY slave for VT N-1
     * tty1=VT0, tty2=VT1, tty3=VT2, tty4=VT3. tty5-tty12 → ENOENT */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='t' && path[6]=='t' && path[7]=='y' &&
        path[8] >= '1' && path[8] <= '9') {
        int vt_num = 0;
        const char *d = path + 8;
        while (*d >= '0' && *d <= '9') vt_num = vt_num * 10 + (*d++ - '0');
        if (*d == '\0' && vt_num >= 1 && vt_num <= 12) {
            int vt_id = vt_num - 1; /* tty1 → VT0 */
            process_t *p = proc_current();
            if (!p) return -EFAULT;
            int fd = fd_alloc(&p->fds, FD_PTY_SLAVE, (void *)(long)vt_id,
                              (flags & 3) | (flags & O_PATH) | (flags & O_CLOEXEC));
            return fd < 0 ? -EMFILE : fd;
        }
    }
    /* /dev/pts/N → PTY slave N (same as /dev/tty(N+1)) */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='p' && path[6]=='t' && path[7]=='s' &&
        path[8]=='/') {
        int pts_id = 0;
        const char *d = path + 9;
        if (*d < '0' || *d > '9') goto not_pts;
        while (*d >= '0' && *d <= '9') pts_id = pts_id * 10 + (*d++ - '0');
        if (*d == '\0' && pts_id >= 0 && pts_id < PTY_DEV_ID_MAX) {
            process_t *p = proc_current();
            if (!p) return -EFAULT;
            int fd = fd_alloc(&p->fds, FD_PTY_SLAVE, (void *)(long)pts_id,
                              (flags & 3) | (flags & O_PATH) | (flags & O_CLOEXEC));
            return fd < 0 ? -EMFILE : fd;
        }
        return -ENOENT;
    }
not_pts:
    /* /dev/loop-control + /dev/loopN */
    if (kstreq(path, "/dev/loop-control")) {
        process_t *p = proc_current();
        if (!p) return -EFAULT;
        int fd = fd_alloc(&p->fds, FD_DEVICE, (void *)(uintptr_t)DEV_LOOP_CTL,
                          (flags & 3) | (flags & O_PATH) | (flags & O_CLOEXEC));
        return fd < 0 ? -EMFILE : fd;
    }
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='l' && path[6]=='o' && path[7]=='o' &&
        path[8]=='p' && path[9] >= '0' && path[9] <= '9') {
        int loop_num = 0;
        const char *d = path + 9;
        while (*d >= '0' && *d <= '9') loop_num = loop_num * 10 + (*d++ - '0');
        if (*d == '\0' && loop_num >= 0 && loop_num < 8) {
            process_t *p = proc_current();
            if (!p) return -EFAULT;
            int fd = fd_alloc(&p->fds, FD_DEVICE,
                              (void *)(uintptr_t)(DEV_LOOP_BASE + loop_num),
                              (flags & 3) | (flags & O_PATH) | (flags & O_CLOEXEC));
            return fd < 0 ? -EMFILE : fd;
        }
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
        int fd = fd_alloc(&p->fds, FD_DEVICE, (void *)(uintptr_t)devid,
                          (flags & 3) | (flags & O_PATH) | (flags & O_CLOEXEC));
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

            int want_write = (flags & O_ACCMODE) != O_RDONLY;
            /* Read-only per-pid files reject O_WRONLY/O_RDWR.
             * ptype 4 = writable per-pid (oom_score_adj, oom_adj). */
            if (want_write && ptype != 4) return -EACCES;

            procfs_fd_t *pf = procfs_fd_alloc();
            if (!pf) return -ENOMEM;
            pf->handle = (ptype == 3) ? -3 : -2; /* -3 = fd directory, -2/-4 = per-pid file */
            pf->offset = 0;
            int ni = 0;
            while (ni < 63 && pname[ni]) { pf->name[ni] = pname[ni]; ni++; }
            pf->name[ni] = 0;

            process_t *p = proc_current();
            if (!p) { procfs_fd_free(pf); return -EFAULT; }
            int fd = fd_alloc(&p->fds, FD_PROCFS, pf,
                              flags & (O_ACCMODE | O_CLOEXEC));
            if (fd < 0) { procfs_fd_free(pf); return -EMFILE; }
            return fd;
        }

        /* Write-access requested? Entry must expose a write handler or
         * we return -EACCES like Linux read-only procfs files. */
        int want_write = (flags & O_ACCMODE) != O_RDONLY;
        if (want_write && !procfs_writable(pname)) return -EACCES;

        procfs_fd_t *pf = procfs_fd_alloc();
        if (!pf) return -ENOMEM;
        pf->handle = handle;
        pf->offset = 0;

        process_t *p = proc_current();
        if (!p) { procfs_fd_free(pf); return -EFAULT; }

        int fd = fd_alloc(&p->fds, FD_PROCFS, pf,
                          flags & (O_ACCMODE | O_CLOEXEC));
        if (fd < 0) { procfs_fd_free(pf); return -EMFILE; }
        return fd;
    }

    /* Resolve mount for path */
    const char *relpath;
    struct mount *mnt = vfs_resolve_mount(path, &relpath);

    /* Read-only mount: reject any open that may modify (O_WRONLY/O_RDWR,
     * O_CREAT, O_TRUNC). Linux fs/open.c: do_open → may_open → EROFS. */
    if (mnt && (mnt->mnt_flags & MS_RDONLY) &&
        (((flags & O_ACCMODE) != O_RDONLY) || (flags & (O_CREAT | O_TRUNC))))
        return -EROFS;

    /* ext4 path? */
    if (mnt && mnt->f_ops == &ext4_file_ops) {
        int werr = -ENOENT;
        uint64_t ino64 = ext4_walk_err(path, &werr);
        uint32_t ino = (uint32_t)ino64;

        if (ino != 0 && (flags & O_CREAT) && (flags & O_EXCL))
            return -EEXIST;

        if (ino == 0 && (flags & O_CREAT) && werr == -ENOENT) {
            const char *basename;
            int perr = -ENOENT;
            uint64_t parent_ino64 = ext4_walk_parent_err(path, &basename, &perr);
            uint32_t parent_ino = (uint32_t)parent_ino64;
            if (parent_ino == 0) return perr;
            struct ext4_inode pip;
            if (ext4_inode_read(parent_ino, &pip) < 0) return -EIO;
            if ((pip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

            {
                process_t *pcheck = proc_current();
                if (pcheck && pcheck->euid != 0) {
                    int rc = cred_may_access(pcheck, pip.i_uid, pip.i_gid,
                                             pip.i_mode,
                                             MAY_WRITE | MAY_EXEC);
                    if (rc < 0) return rc;
                }
            }

            process_t *pcur = proc_current();
            uint32_t cmask = pcur ? pcur->umask_val : 0022;
            uint32_t raw = mode ? (mode & 07777) : 0644;
            uint32_t cmode = raw & ~(cmask & 0777);
            uint32_t new_gid = (pip.i_mode & S_ISGID)
                                   ? (uint32_t)pip.i_gid
                                   : (pcur ? pcur->fsgid : 0);
            if (pcur && pcur->euid != 0) {
                cmode &= ~(uint32_t)S_ISUID;
                if ((cmode & S_ISGID) && !cred_in_group(pcur, new_gid))
                    cmode &= ~(uint32_t)S_ISGID;
            }
            uint32_t new_ino;
            int rc = ext4_create(parent_ino, basename, (int)cmode, &new_ino);
            if (rc < 0) return rc;
            if (pcur) {
                struct ext4_inode nip;
                if (ext4_inode_read(new_ino, &nip) == 0) {
                    nip.i_uid = (uint16_t)pcur->fsuid;
                    nip.i_gid = (uint16_t)new_gid;
                    ext4_inode_write(new_ino, &nip);
                }
            }
            ino = new_ino;
            inotify_event(path, IN_CREATE);
        }

        if (ino == 0) return werr;

        struct ext4_inode ip;
        if (ext4_inode_read(ino, &ip) < 0) return -EIO;
        int is_dir = ((ip.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR);
        if ((flags & O_DIRECTORY) && !is_dir) return -ENOTDIR;
        if (is_dir && (flags & O_ACCMODE) != O_RDONLY) return -EISDIR;

        int writable = !is_dir &&
                       (((flags & O_ACCMODE) == O_WRONLY) ||
                        ((flags & O_ACCMODE) == O_RDWR) ||
                        (flags & O_TRUNC));
        if (writable) {
            int rc = i_writecount_get_write(VFS_BACKEND_EXT4, ino);
            if (rc < 0) return rc;
        }

        struct vfs_file *f = file_alloc();
        if (!f) {
            if (writable) i_writecount_put_write(VFS_BACKEND_EXT4, ino);
            return -ENOMEM;
        }
        f->type = is_dir ? VFS_DIR : VFS_FILE;
        f->flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_CLOEXEC | O_PATH);
        f->refcount = 1;
        f->backend = VFS_BACKEND_EXT4;
        f->f_ops = &ext4_file_ops;
        f->mnt = mnt;
        f->offset = 0;
        f->inode = 0;
        f->disk_ino = ino;
        f->disk_size = ip.i_size;
        f->disk_dir_ino = 0;
        kstrncpy(f->path, path, 256);
        if ((flags & O_TRUNC) && !is_dir) ext4_truncate(ino, 0);

        process_t *p = proc_current();
        if (!p) {
            if (writable) i_writecount_put_write(VFS_BACKEND_EXT4, ino);
            file_free(f);
            return -EFAULT;
        }
        int fd = fd_alloc(&p->fds, FD_FILE, f, f->flags);
        if (fd < 0) {
            if (writable) i_writecount_put_write(VFS_BACKEND_EXT4, ino);
            file_free(f);
            return -EMFILE;
        }
        ext4_open_inc(ino);
        return fd;
    }

    /* tmpfs path (ramfs) */
    int verr = -ENOENT;
    struct vfs_node *node = vfs_lookup_err(path, &verr);

    if (node && (flags & O_NOFOLLOW)) {
        int lerr = 0;
        struct vfs_node *raw = vfs_lookup_nofollow(path, &lerr);
        if (raw && raw->inode->type == VFS_SYMLINK) return -ELOOP;
    }

    if (node && (flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;

    if (!node && (flags & O_CREAT) && verr == -ENOENT) {
        /* O_CREAT requires MAY_WRITE on parent directory (non-root). */
        const char *bn;
        struct vfs_node *parent = lookup_parent(path, &bn);
        if (parent && parent->inode) {
            process_t *pcheck = proc_current();
            if (pcheck && pcheck->euid != 0) {
                int rc = cred_may_access(pcheck, parent->inode->uid,
                                         parent->inode->gid, parent->inode->mode,
                                         MAY_WRITE | MAY_EXEC);
                if (rc < 0) return rc;
            }
        }
        node = vfs_create(path, VFS_FILE);
        if (!node) return -ENOENT;
        process_t *pcur = proc_current();
        uint32_t cmask = pcur ? pcur->umask_val : 0022;
        uint32_t cmode = (mode & 07777) & ~(cmask & 0777);
        struct vfs_inode *parent_ino = node->parent ? node->parent->inode : 0;
        uint32_t new_gid = (parent_ino && (parent_ino->mode & S_ISGID))
                               ? parent_ino->gid
                               : (pcur ? pcur->fsgid : 0);
        if (pcur && pcur->euid != 0) {
            cmode &= ~(uint32_t)S_ISUID;
            if ((cmode & S_ISGID) && !cred_in_group(pcur, new_gid))
                cmode &= ~(uint32_t)S_ISGID;
        }
        node->inode->mode = cmode;
        if (pcur) {
            node->inode->uid = pcur->fsuid;
            node->inode->gid = new_gid;
        }
        inotify_event(path, IN_CREATE);
    }
    if (!node) return verr;

    if ((flags & O_DIRECTORY) && node->inode->type != VFS_DIR) return -ENOTDIR;

    /* Opening a directory for write → EISDIR (Linux fs/namei.c do_open) */
    if (node->inode->type == VFS_DIR &&
        (flags & O_ACCMODE) != O_RDONLY)
        return -EISDIR;

    int writable_tm = (node->inode->type == VFS_FILE) &&
                      (((flags & O_ACCMODE) == O_WRONLY) ||
                       ((flags & O_ACCMODE) == O_RDWR) ||
                       (flags & O_TRUNC));
    if (writable_tm) {
        int rc = i_writecount_get_write(VFS_BACKEND_RAM, node->inode->ino);
        if (rc < 0) return rc;
    }

    struct vfs_file *f = file_alloc();
    if (!f) {
        if (writable_tm) i_writecount_put_write(VFS_BACKEND_RAM, node->inode->ino);
        return -ENOMEM;
    }
    f->type = node->inode->type;
    f->flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_CLOEXEC | O_PATH);
    f->refcount = 1;
    f->backend = VFS_BACKEND_RAM;
    f->f_ops = &tmpfs_file_ops;
    f->mnt = mnt;
    f->offset = 0;
    f->inode = node->inode;
    f->disk_ino = 0;
    f->disk_size = 0;
    f->disk_dir_ino = 0;
    kstrncpy(f->path, path, 256);
    if ((flags & O_TRUNC) && node->inode->type == VFS_FILE) node->inode->size = 0;

    process_t *p = proc_current();
    if (!p) {
        if (writable_tm) i_writecount_put_write(VFS_BACKEND_RAM, node->inode->ino);
        file_free(f);
        return -EFAULT;
    }
    int fd = fd_alloc(&p->fds, FD_FILE, f, f->flags);
    if (fd < 0) {
        if (writable_tm) i_writecount_put_write(VFS_BACKEND_RAM, node->inode->ino);
        file_free(f);
        return -EMFILE;
    }
    __sync_fetch_and_add(&node->inode->refcount, 1);
    return fd;
}

int vfs_close(int fd) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (f) {
        /* Generate inotify close event based on open mode */
        if (f->path[0]) {
            int writable = (f->flags & (O_WRONLY | O_RDWR));
            inotify_event(f->path, writable ? IN_CLOSE_WRITE : IN_CLOSE_NOWRITE);
        }
        if (__sync_sub_and_fetch(&f->refcount, 1) <= 0) {
            vfs_file_release(f);
            file_free(f);
        }
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
    /* Verify path exists and is a directory via stat */
    struct k_stat st;
    int rc = vfs_stat(path, &st);
    if (rc < 0) return rc;
    if ((st.st_mode & S_IFMT) != S_IFDIR) return -ENOTDIR;

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
        if (grow_file(node->inode, len) < 0) return -ENOMEM;
        ramfs_write(node->inode, data, 0, len);
        node->inode->size = len;
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
        size_t off = node->inode->size;
        size_t end = off + len;
        if (grow_file(node->inode, end) < 0) return -ENOMEM;
        ramfs_write(node->inode, buf, off, len);
        node->inode->size = end;
        return (long)len;
    }

    /* ext4: walk path, create if missing, append via ext4_write */
    uint64_t ino64 = ext4_walk(path);
    uint32_t ino = (uint32_t)ino64;
    if (ino == 0) {
        const char *basename;
        uint64_t parent_ino64 = ext4_walk_parent(path, &basename);
        uint32_t parent_ino = (uint32_t)parent_ino64;
        if (parent_ino == 0) return -ENOENT;

        struct ext4_inode pip;
        if (ext4_inode_read(parent_ino, &pip) < 0) return -EIO;
        if ((pip.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -ENOTDIR;

        uint32_t new_ino;
        int rc = ext4_create(parent_ino, basename, 0644, &new_ino);
        if (rc < 0) return rc;
        ino = new_ino;
    }

    struct ext4_inode ip;
    if (ext4_inode_read(ino, &ip) < 0) return -EIO;
    size_t off = ip.i_size;

    int rc = ext4_write(ino, buf, off, len);
    return (long)rc;
}

/* ── Mount ext4 ───────────────────────────────────── */

void vfs_mount_ext4(void) {
    if (ext4_mount() == 0) {
        /* Replace tmpfs root mount with ext4 */
        vfs_mount("/", &ext4_super_ops, &ext4_inode_ops, &ext4_file_ops, 0);
        serial_puts("vfs: ext4 mounted as /\n");
    } else {
        serial_puts("vfs: ext4 mount failed, using ramfs\n");
    }
}

uint64_t vfs_ext4_lookup(const char *path) {
    if (!ext4_mounted()) return 0;
    return ext4_walk(path);
}

int vfs_read_file(const char *path, uint8_t **out_data, size_t *out_size) {
    /* Try ramfs first (for embedded binaries like /lib/ld-musl-x86_64.so.1) */
    struct vfs_node *node = vfs_lookup(path);
    if (node && node->inode->type == VFS_FILE && node->inode->size > 0) {
        size_t sz = node->inode->size;
        int npages = (int)((sz + 4095) / 4096);
        uint8_t *buf = (uint8_t *)pages_alloc(npages);
        if (!buf) return -ENOMEM;
        size_t got = vfs_inode_read(node->inode, buf, 0, sz);
        if (got != sz) {
            /* Inode mutated under us — return what we got, zero tail. */
            kmemset(buf + got, 0, sz - got);
        }
        *out_data = buf;
        *out_size = sz;
        return 0;
    }

    /* Try ext4 */
    if (ext4_mounted()) {
        uint64_t ino64 = ext4_walk(path);
        uint32_t ino = (uint32_t)ino64;
        if (ino == 0) return -ENOENT;

        struct ext4_inode ip;
        if (ext4_inode_read(ino, &ip) < 0) return -EIO;
        if ((ip.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG) return -EACCES;
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
            int r = ext4_read(ino, buf + off, off, chunk);
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
