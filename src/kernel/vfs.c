/* CosmoRT VFS — filesystem dispatch (ramfs + CosmoFS) */

#include "vfs.h"
#include "cosmofs.h"
#include "fd.h"
#include "process.h"
#include "percpu.h"
#include "slab.h"
#include "serial.h"
#include "page_alloc.h"
#include "memops.h"
#include "syscall.h"
#include "procfs.h"

/* ── Slab pools ──────────────────────────────────── */

#define VFS_NODE_MAX 256
#define VFS_FILE_MAX 512

static struct vfs_node node_pool[VFS_NODE_MAX];
static struct vfs_file file_pool[VFS_FILE_MAX];
static slab_t node_slab;
static slab_t file_slab;

static struct vfs_node *root_node;
static uint64_t next_ino = 1;

/* cwd is now per-process (process_t.cwd). Helper to get it: */
static char *get_cwd(void) {
    process_t *p = proc_current();
    return p ? p->cwd : 0;
}

/* ── String helpers ──────────────────────────────── */

static int kstrlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void kstrncpy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* ── procfs routing ──────────────────────────────── */

/* Returns pointer to name after "/proc/" or NULL */
static const char *procfs_name(const char *path) {
    if (path[0]=='/' && path[1]=='p' && path[2]=='r' && path[3]=='o' &&
        path[4]=='c' && path[5]=='/') {
        const char *name = path + 6;
        if (*name && *name != '/') return name;
    }
    return 0;
}

/* ── CosmoFS routing ─────────────────────────────── */

/* Returns 1 if path should use ramfs (not CosmoFS) */
static int is_ramfs_path(const char *path) {
    if (!cosmofs_mounted()) return 1;
    /* /dev/shm always ramfs */
    if (path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' &&
        path[4]=='/' && path[5]=='s' && path[6]=='h' && path[7]=='m' &&
        (path[8]=='/' || path[8]==0))
        return 1;
    return 0;
}

/* Walk a CosmoFS path component-by-component, returning the inode number.
 * Returns 0 on failure (inode 0 is invalid). */
static uint64_t cosmofs_walk(const char *path) {
    if (!path || path[0] != '/') return 0;
    if (path[0] == '/' && path[1] == 0) return cosmofs_root_ino();

    uint64_t cur = cosmofs_root_ino();
    const char *p = path + 1;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        /* Extract component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        char name[256];
        int cplen = len < 255 ? len : 255;
        for (int i = 0; i < cplen; i++) name[i] = start[i];
        name[cplen] = 0;

        uint64_t child;
        if (cosmofs_dir_lookup(cur, name, &child) < 0)
            return 0;
        cur = child;
    }
    return cur;
}

/* Walk to parent directory, extract basename. Returns parent inode or 0. */
static uint64_t cosmofs_walk_parent(const char *path, const char **basename_out) {
    if (!path || path[0] != '/') return 0;

    int len = kstrlen(path);
    int last_slash = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    *basename_out = path + last_slash + 1;
    if (!**basename_out) return 0;

    if (last_slash == 0) return cosmofs_root_ino();

    /* Build parent path */
    char parent_path[256];
    int plen = last_slash < 255 ? last_slash : 255;
    for (int i = 0; i < plen; i++) parent_path[i] = path[i];
    parent_path[plen] = 0;

    return cosmofs_walk(parent_path);
}

/* ── Node allocation ─────────────────────────────── */

static struct vfs_node *node_alloc(const char *name, int type) {
    struct vfs_node *n = (struct vfs_node *)slab_alloc(&node_slab);
    if (!n) return 0;
    kstrncpy(n->name, name, 256);
    n->type = type;
    n->data = 0;
    n->size = 0;
    n->capacity = 0;
    n->ino = next_ino++;
    n->children = 0;
    n->next = 0;
    n->parent = 0;
    return n;
}

static struct vfs_file *file_alloc(void) {
    return (struct vfs_file *)slab_alloc(&file_slab);
}

static void file_free(struct vfs_file *f) {
    slab_free(&file_slab, f);
}

/* Increment refcount on a vfs_file (for fork fd duplication) */
void vfs_file_incref(struct vfs_file *f) {
    if (f) f->refcount++;
}

/* Free a vfs_file object by external pointer (used by proc_cleanup) */
void vfs_file_free_obj(void *obj) {
    if (!obj) return;
    struct vfs_file *f = (struct vfs_file *)obj;
    if (--f->refcount <= 0)
        file_free(f);
}

/* ── Init ────────────────────────────────────────── */

void vfs_init(void) {
    slab_init(&node_slab, node_pool, (int)sizeof(struct vfs_node), VFS_NODE_MAX);
    slab_init(&file_slab, file_pool, (int)sizeof(struct vfs_file), VFS_FILE_MAX);
    root_node = node_alloc("/", VFS_DIR);
    serial_puts("vfs: init (ramfs)\n");
}

/* ── Path lookup ─────────────────────────────────── */

struct vfs_node *vfs_lookup(const char *path) {
    if (!path || !path[0]) return 0;
    if (path[0] == '/' && path[1] == 0) return root_node;

    struct vfs_node *cur = root_node;
    const char *p = path;
    if (*p == '/') p++;

    while (*p) {
        /* Skip trailing slashes */
        while (*p == '/') p++;
        if (!*p) break;

        /* Extract component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        if (cur->type != VFS_DIR) return 0;

        /* Search children */
        struct vfs_node *child = cur->children;
        struct vfs_node *found = 0;
        while (child) {
            int nlen = kstrlen(child->name);
            if (nlen == len) {
                int match = 1;
                for (int i = 0; i < len; i++) {
                    if (child->name[i] != start[i]) { match = 0; break; }
                }
                if (match) { found = child; break; }
            }
            child = child->next;
        }
        if (!found) return 0;
        cur = found;
    }
    return cur;
}

/* Find parent directory and extract basename */
static struct vfs_node *lookup_parent(const char *path, const char **basename) {
    if (!path || path[0] != '/') return 0;

    /* Find last component */
    int len = kstrlen(path);
    int last_slash = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    *basename = path + last_slash + 1;
    if (!**basename) return 0;

    /* Lookup parent path */
    if (last_slash == 0) return root_node;

    /* Build parent path */
    char parent_path[256];
    int plen = last_slash;
    if (plen >= 256) plen = 255;
    for (int i = 0; i < plen; i++) parent_path[i] = path[i];
    parent_path[plen] = 0;

    return vfs_lookup(parent_path);
}

/* ── Create ──────────────────────────────────────── */

struct vfs_node *vfs_create(const char *path, int type) {
    const char *basename;
    struct vfs_node *parent = lookup_parent(path, &basename);
    if (!parent || parent->type != VFS_DIR) return 0;

    /* Check if already exists */
    struct vfs_node *existing = vfs_lookup(path);
    if (existing) return existing;

    struct vfs_node *n = node_alloc(basename, type);
    if (!n) return 0;

    n->parent = parent;
    n->next = parent->children;
    parent->children = n;
    return n;
}

/* Create directories along a path recursively */
static struct vfs_node *ensure_dirs(const char *path) {
    struct vfs_node *cur = root_node;
    const char *p = path;
    if (*p == '/') p++;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        int len = (int)(p - start);

        /* If there's more path after this, it's a directory component */
        if (!*p) break; /* last component = the file, don't create as dir */

        /* Search for existing child */
        struct vfs_node *child = cur->children;
        struct vfs_node *found = 0;
        while (child) {
            int nlen = kstrlen(child->name);
            if (nlen == len) {
                int match = 1;
                for (int i = 0; i < len; i++) {
                    if (child->name[i] != start[i]) { match = 0; break; }
                }
                if (match) { found = child; break; }
            }
            child = child->next;
        }

        if (found) {
            cur = found;
        } else {
            /* Create directory */
            char name[256];
            int copylen = len < 255 ? len : 255;
            for (int i = 0; i < copylen; i++) name[i] = start[i];
            name[copylen] = 0;

            struct vfs_node *n = node_alloc(name, VFS_DIR);
            if (!n) return 0;
            n->parent = cur;
            n->next = cur->children;
            cur->children = n;
            cur = n;
        }
    }
    return cur;
}

/* ── Open/Close ──────────────────────────────────── */

int vfs_open(const char *path, int flags, int mode) {
    (void)mode;

    /* procfs path? */
    const char *pname = procfs_name(path);
    if (pname) {
        int handle = procfs_open(pname);
        if (!handle) return -ENOENT;

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

    /* CosmoFS path? */
    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);

        if (ino == 0 && (flags & O_CREAT)) {
            /* Create file on CosmoFS */
            const char *basename;
            uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
            if (parent_ino == 0) return -ENOENT;

            /* Ensure parent is a directory */
            struct cosmofs_inode *pip = cosmofs_inode_read(parent_ino);
            if (!pip || pip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;

            /* Allocate inode */
            uint64_t new_ino = cosmofs_inode_alloc();
            if (new_ino == 0) return -ENOMEM;

            /* Set up as file */
            struct cosmofs_inode new_in;
            kmemset(&new_in, 0, sizeof(new_in));
            new_in.type = COSMOFS_TYPE_FILE;
            cosmofs_inode_write(new_ino, &new_in);

            /* Add to parent directory */
            cosmofs_dir_create(parent_ino, basename, new_ino);
            ino = new_ino;
        }

        if (ino == 0) return -ENOENT;

        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;

        if ((flags & O_DIRECTORY) && ip->type != COSMOFS_TYPE_DIR)
            return -ENOTDIR;

        struct vfs_file *f = file_alloc();
        if (!f) return -ENOMEM;

        f->type = (ip->type == COSMOFS_TYPE_DIR) ? VFS_DIR : VFS_FILE;
        f->flags = flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND | O_CLOEXEC);
        f->refcount = 1;
        f->backend = VFS_BACKEND_COSMOFS;
        f->offset = 0;
        f->node = 0;
        f->cosmofs_ino = ino;
        f->cosmofs_size = ip->size;
        f->cosmofs_dir_ino = 0;

        if ((flags & O_TRUNC) && ip->type == COSMOFS_TYPE_FILE)
            cosmofs_truncate(ino, 0);

        process_t *p = proc_current();
        if (!p) { file_free(f); return -EFAULT; }

        int fd = fd_alloc(&p->fds, FD_FILE, f, f->flags);
        if (fd < 0) { file_free(f); return -EMFILE; }
        return fd;
    }

    /* ramfs path */
    struct vfs_node *node = vfs_lookup(path);

    if (!node && (flags & O_CREAT)) {
        ensure_dirs(path);
        node = vfs_create(path, VFS_FILE);
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
    f->cosmofs_ino = 0;
    f->cosmofs_size = 0;
    f->cosmofs_dir_ino = 0;

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
        if (--f->refcount <= 0)
            file_free(f);
    }

    fde->type = FD_NONE;
    fde->obj = 0;
    return 0;
}

/* ── Read/Write ──────────────────────────────────── */

static int grow_file(struct vfs_node *node, size_t needed) {
    if (needed <= node->capacity) return 0;

    /* Round up to page boundary */
    size_t new_cap = (needed + 4095) & ~4095ULL;
    int new_pages = (int)(new_cap / 4096);
    if (new_pages <= 0) new_pages = 1;

    uint8_t *new_data = (uint8_t *)pages_alloc(new_pages);
    if (!new_data) return -ENOMEM;

    if (node->data && node->size > 0) {
        kmemcpy(new_data, node->data, node->size);
    }

    if (node->data) {
        int old_pages = (int)((node->capacity + 4095) / 4096);
        if (old_pages > 0) pages_free(node->data, old_pages);
    }

    node->data = new_data;
    node->capacity = new_cap;
    return 0;
}

long vfs_read(int fd, void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_FILE) return -EISDIR;
        int rc = cosmofs_read(f->cosmofs_ino, buf, (size_t)f->offset, count);
        if (rc < 0) return rc;
        f->offset += (uint64_t)rc;
        return (long)rc;
    }

    /* ramfs */
    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    if (f->offset >= node->size) return 0;

    size_t avail = node->size - (size_t)f->offset;
    if (count > avail) count = avail;

    if (node->data)
        kmemcpy(buf, node->data + f->offset, count);

    f->offset += count;
    return (long)count;
}

long vfs_pread(struct vfs_file *f, void *buf, size_t count, uint64_t offset) {
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_FILE) return -EISDIR;
        extern int cosmofs_read(uint64_t ino, void *buf, size_t offset, size_t len);
        int rc = cosmofs_read(f->cosmofs_ino, buf, (size_t)offset, count);
        return (long)rc;
    }

    /* ramfs */
    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    if (offset >= node->size) return 0;

    size_t avail = node->size - (size_t)offset;
    if (count > avail) count = avail;

    if (node->data)
        kmemcpy(buf, node->data + offset, count);

    return (long)count;
}

long vfs_write(int fd, const void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        if (f->type != VFS_FILE) return -EISDIR;
        if (f->flags & O_APPEND) {
            /* Re-read inode size for append */
            struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
            if (ip) f->offset = ip->size;
        }
        int rc = cosmofs_write(f->cosmofs_ino, buf, (size_t)f->offset, count);
        if (rc < 0) return rc;
        f->offset += (uint64_t)rc;
        f->cosmofs_size = f->offset;
        return (long)rc;
    }

    /* ramfs */
    if (!f->node) return -EBADF;
    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    if (f->flags & O_APPEND)
        f->offset = node->size;

    size_t end = (size_t)f->offset + count;
    if (end > node->capacity) {
        if (grow_file(node, end) < 0) return -ENOMEM;
    }

    kmemcpy(node->data + f->offset, buf, count);
    f->offset = end;
    if (end > node->size) node->size = end;

    return (long)count;
}

long vfs_lseek(int fd, long offset, int whence) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    long new_off;
    if (f->backend == VFS_BACKEND_COSMOFS) {
        uint64_t sz = f->cosmofs_size;
        struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
        if (ip) sz = ip->size;
        switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)f->offset + offset; break;
        case SEEK_END: new_off = (long)sz + offset; break;
        default: return -EINVAL;
        }
    } else {
        if (!f->node) return -EBADF;
        switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)f->offset + offset; break;
        case SEEK_END: new_off = (long)f->node->size + offset; break;
        default: return -EINVAL;
        }
    }

    if (new_off < 0) return -EINVAL;
    f->offset = (uint64_t)new_off;
    return new_off;
}

/* ── Stat ────────────────────────────────────────── */

static void fill_stat(struct vfs_node *node, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = node->ino;
    buf->st_nlink = 1;
    buf->st_size = (int64_t)node->size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)((node->size + 511) / 512);

    if (node->type == VFS_DIR)
        buf->st_mode = S_IFDIR | S_IRWXU;
    else if (node->type == VFS_FILE)
        buf->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
    else if (node->type == VFS_PIPE)
        buf->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
}

static void fill_cosmofs_stat(uint64_t ino, struct cosmofs_inode *ip, struct k_stat *buf) {
    kmemset(buf, 0, sizeof(struct k_stat));
    buf->st_ino = ino;
    buf->st_dev = 1;  /* CosmoFS device */
    buf->st_nlink = 1;
    buf->st_size = (int64_t)ip->size;
    buf->st_blksize = 4096;
    buf->st_blocks = (int64_t)((ip->size + 511) / 512);
    buf->st_atime_sec = (int64_t)(ip->atime / 1000000000ULL);
    buf->st_atime_nsec = (int64_t)(ip->atime % 1000000000ULL);
    buf->st_mtime_sec = (int64_t)(ip->mtime / 1000000000ULL);
    buf->st_mtime_nsec = (int64_t)(ip->mtime % 1000000000ULL);
    buf->st_ctime_sec = (int64_t)(ip->ctime / 1000000000ULL);
    buf->st_ctime_nsec = (int64_t)(ip->ctime % 1000000000ULL);

    if (ip->type == COSMOFS_TYPE_DIR)
        buf->st_mode = S_IFDIR | S_IRWXU;
    else
        buf->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IXUSR;
}

int vfs_stat(const char *path, struct k_stat *buf) {
    const char *pn = procfs_name(path);
    if (pn) {
        int dummy;
        if (procfs_stat(pn, &dummy) < 0) return -ENOENT;
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFREG | S_IRUSR;
        buf->st_ino = 0xFFFF0001;  /* synthetic inode */
        buf->st_blksize = 4096;
        return 0;
    }
    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip) return -EIO;
        fill_cosmofs_stat(ino, ip, buf);
        return 0;
    }
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    fill_stat(node, buf);
    return 0;
}

int vfs_fstat(int fd, struct k_stat *buf) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde) return -EBADF;

    /* Serial FDs get a minimal stat */
    if (fde->type == FD_SERIAL) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
        buf->st_blksize = 4096;
        return 0;
    }

    /* procfs FDs */
    if (fde->type == FD_PROCFS) {
        kmemset(buf, 0, sizeof(struct k_stat));
        buf->st_mode = S_IFREG | S_IRUSR;
        buf->st_ino = 0xFFFF0001;
        buf->st_blksize = 4096;
        return 0;
    }

    if (fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f) return -EBADF;

    if (f->backend == VFS_BACKEND_COSMOFS) {
        struct cosmofs_inode *ip = cosmofs_inode_read(f->cosmofs_ino);
        if (!ip) return -EIO;
        fill_cosmofs_stat(f->cosmofs_ino, ip, buf);
        return 0;
    }

    if (!f->node) return -EBADF;
    fill_stat(f->node, buf);
    return 0;
}

/* ── CWD ─────────────────────────────────────────── */

int vfs_getcwd(char *buf, size_t size) {
    char *cwd = get_cwd();
    if (!cwd) return -EFAULT;
    int len = kstrlen(cwd);
    if ((size_t)(len + 1) > size) return -ERANGE;
    kmemcpy(buf, cwd, (size_t)(len + 1));
    return len;
}

int vfs_chdir(const char *path) {
    if (!is_ramfs_path(path)) {
        uint64_t ino = cosmofs_walk(path);
        if (ino == 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(ino);
        if (!ip || ip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;
        char *cwd = get_cwd();
        if (!cwd) return -EFAULT;
        kstrncpy(cwd, path, 256);
        return 0;
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_DIR) return -ENOTDIR;

    char *cwd = get_cwd();
    if (!cwd) return -EFAULT;
    kstrncpy(cwd, path, 256);
    return 0;
}

/* ── Directory/file mutation ─────────────────────── */

int vfs_mkdir(const char *path) {
    if (!is_ramfs_path(path)) {
        /* Check if exists */
        uint64_t ino = cosmofs_walk(path);
        if (ino != 0) return -EEXIST;

        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
        if (parent_ino == 0) return -ENOENT;

        uint64_t new_ino = cosmofs_inode_alloc();
        if (new_ino == 0) return -ENOMEM;

        struct cosmofs_inode dir_in;
        kmemset(&dir_in, 0, sizeof(dir_in));
        dir_in.type = COSMOFS_TYPE_DIR;
        cosmofs_inode_write(new_ino, &dir_in);

        return cosmofs_dir_create(parent_ino, basename, new_ino);
    }

    struct vfs_node *existing = vfs_lookup(path);
    if (existing) return -EEXIST;
    struct vfs_node *n = vfs_create(path, VFS_DIR);
    if (!n) return -ENOENT;
    return 0;
}

/* Remove child from parent's linked list */
static int unlink_child(struct vfs_node *parent, struct vfs_node *child) {
    struct vfs_node **pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next;
            child->next = 0;
            child->parent = 0;
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

int vfs_rmdir(const char *path) {
    if (!is_ramfs_path(path)) {
        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
        if (parent_ino == 0) return -ENOENT;
        uint64_t child_ino;
        if (cosmofs_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(child_ino);
        if (!ip || ip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;
        /* Check empty (size == entry count in our dir impl) */
        if (ip->size > 0) return -ENOTEMPTY;
        int rc = cosmofs_dir_remove(parent_ino, basename);
        if (rc == 0) cosmofs_inode_free(child_ino);
        return rc;
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_DIR) return -ENOTDIR;
    if (node->children) return -ENOTEMPTY;
    if (node == root_node) return -EINVAL;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    slab_free(&node_slab, node);
    return 0;
}

int vfs_unlink(const char *path) {
    if (!is_ramfs_path(path)) {
        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
        if (parent_ino == 0) return -ENOENT;
        uint64_t child_ino;
        if (cosmofs_dir_lookup(parent_ino, basename, &child_ino) < 0) return -ENOENT;
        struct cosmofs_inode *ip = cosmofs_inode_read(child_ino);
        if (!ip) return -EIO;
        if (ip->type == COSMOFS_TYPE_DIR) return -EISDIR;
        int rc = cosmofs_dir_remove(parent_ino, basename);
        if (rc == 0) {
            /* TODO: free file data blocks */
            cosmofs_inode_free(child_ino);
        }
        return rc;
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type == VFS_DIR) return -EISDIR;
    if (!node->parent) return -EINVAL;
    unlink_child(node->parent, node);
    if (node->data && node->capacity > 0) {
        int npages = (int)((node->capacity + 4095) / 4096);
        if (npages > 0) pages_free(node->data, npages);
    }
    slab_free(&node_slab, node);
    return 0;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    if (!is_ramfs_path(oldpath) && !is_ramfs_path(newpath)) {
        const char *old_base;
        uint64_t old_parent = cosmofs_walk_parent(oldpath, &old_base);
        if (old_parent == 0) return -ENOENT;

        const char *new_base;
        uint64_t new_parent = cosmofs_walk_parent(newpath, &new_base);
        if (new_parent == 0) return -ENOENT;

        return cosmofs_dir_rename(old_parent, old_base, new_parent, new_base);
    }

    /* ramfs path */
    struct vfs_node *node = vfs_lookup(oldpath);
    if (!node) return -ENOENT;
    if (node == root_node) return -EINVAL;

    struct vfs_node *dst = vfs_lookup(newpath);
    if (dst) {
        if (dst->type == VFS_DIR && dst->children) return -ENOTEMPTY;
        if (dst->parent) unlink_child(dst->parent, dst);
        if (dst->type == VFS_FILE && dst->data && dst->capacity > 0) {
            int np = (int)((dst->capacity + 4095) / 4096);
            if (np > 0) pages_free(dst->data, np);
        }
        slab_free(&node_slab, dst);
    }

    const char *basename;
    struct vfs_node *new_parent = lookup_parent(newpath, &basename);
    if (!new_parent || new_parent->type != VFS_DIR) return -ENOENT;

    if (node->parent) unlink_child(node->parent, node);

    kstrncpy(node->name, basename, 256);
    node->parent = new_parent;
    node->next = new_parent->children;
    new_parent->children = node;
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

    /* CosmoFS: walk path, create if missing, append via cosmofs_write */
    uint64_t ino = cosmofs_walk(path);
    if (ino == 0) {
        const char *basename;
        uint64_t parent_ino = cosmofs_walk_parent(path, &basename);
        if (parent_ino == 0) return -ENOENT;

        struct cosmofs_inode *pip = cosmofs_inode_read(parent_ino);
        if (!pip || pip->type != COSMOFS_TYPE_DIR) return -ENOTDIR;

        ino = cosmofs_inode_alloc();
        if (ino == 0) return -ENOMEM;

        struct cosmofs_inode new_in;
        kmemset(&new_in, 0, sizeof(new_in));
        new_in.type = COSMOFS_TYPE_FILE;
        cosmofs_inode_write(ino, &new_in);
        cosmofs_dir_create(parent_ino, basename, ino);
    }

    struct cosmofs_inode *ip = cosmofs_inode_read(ino);
    if (!ip) return -EIO;
    size_t off = ip->size;

    int rc = cosmofs_write(ino, buf, off, len);
    return (long)rc;
}

/* ── Mount CosmoFS ────────────────────────────────── */

void vfs_mount_cosmofs(void) {
    extern int cosmofs_mount(void);
    if (cosmofs_mount() == 0)
        serial_puts("vfs: CosmoFS mounted as /\n");
    else
        serial_puts("vfs: CosmoFS mount failed, using ramfs\n");
}
