/* CosmoRT VFS — minimal in-memory filesystem (ramfs) */

#include "vfs.h"
#include "fd.h"
#include "process.h"
#include "percpu.h"
#include "slab.h"
#include "serial.h"
#include "page_alloc.h"
#include "memops.h"
#include "syscall.h"

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
    struct vfs_node *node = vfs_lookup(path);

    if (!node && (flags & O_CREAT)) {
        /* Ensure parent directories exist */
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
    f->offset = 0;
    f->node = node;

    if ((flags & O_TRUNC) && node->type == VFS_FILE) {
        node->size = 0;
    }

    /* Allocate FD in current process */
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
    if (!f || !f->node) return -EBADF;

    struct vfs_node *node = f->node;
    if (node->type != VFS_FILE) return -EISDIR;

    if (f->offset >= node->size) return 0; /* EOF */

    size_t avail = node->size - (size_t)f->offset;
    if (count > avail) count = avail;

    if (node->data)
        kmemcpy(buf, node->data + f->offset, count);

    f->offset += count;
    return (long)count;
}

long vfs_write(int fd, const void *buf, size_t count) {
    process_t *p = proc_current();
    if (!p) return -EFAULT;

    fd_entry_t *fde = fd_get(&p->fds, fd);
    if (!fde || fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || !f->node) return -EBADF;

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
    if (!f || !f->node) return -EBADF;

    long new_off;
    switch (whence) {
    case SEEK_SET: new_off = offset; break;
    case SEEK_CUR: new_off = (long)f->offset + offset; break;
    case SEEK_END: new_off = (long)f->node->size + offset; break;
    default: return -EINVAL;
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

int vfs_stat(const char *path, struct k_stat *buf) {
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

    if (fde->type != FD_FILE) return -EBADF;

    struct vfs_file *f = (struct vfs_file *)fde->obj;
    if (!f || !f->node) return -EBADF;

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
    struct vfs_node *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_DIR) return -ENOTDIR;

    char *cwd = get_cwd();
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
