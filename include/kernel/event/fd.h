/* CosmoRT File Descriptor Table
 *
 * Per-process FD table grows on demand. Linux's expand_fdtable model uses
 * a flat array, capped at sysctl_nr_open=1<<20 by default. Our buddy
 * allocator can't serve a contiguous 1M*sizeof(fd_entry_t) array, so the
 * fd lookup is two-level: a top-level dir of 4 KB-page leaves, each leaf
 * holding FD_LEAF_PER_PAGE entries. Lookup stays O(1) with two pointer
 * dereferences.
 *
 * Layout:
 *   entries_dir[]   — array of leaf pointers (pages_alloc backed)
 *   free_bitmap[]   — one bit per slot, bit set = free (flat, pages_alloc)
 *   max_slots       — current capacity (multiple of FD_LEAF_PER_PAGE)
 *   max_fd          — highest allocated fd + 1
 */
#ifndef FD_H
#define FD_H

#include <stdint.h>
#include <stddef.h>

/* FD types */
#define FD_NONE    0
#define FD_SERIAL  1   /* stdin/stdout/stderr → serial port */
#define FD_PIPE    2
#define FD_SOCKET  3
#define FD_DEVICE  4
#define FD_FILE    5
#define FD_PROCFS  6   /* /proc virtual files */
#define FD_EPOLL   7
#define FD_EVENTFD 8
#define FD_TIMERFD 9
#define FD_INOTIFY 10
#define FD_PTY_MASTER 11
#define FD_PTY_SLAVE  12
#define FD_UNIX_SOCK  13
#define FD_NSFS       14  /* /proc/<pid>/ns/XXX namespace handle (setns source).
                           * obj = (struct nsfs_handle *) — see core/time_ns.h */

/* Linux BITS_PER_LONG on x86_64 */
#define FD_INIT_SLOTS     64

/* Linux ulimit -n default */
#define FD_DEFAULT_NOFILE 1024

typedef struct {
    int   type;     /* FD_NONE if unused */
    void *obj;      /* kernel object pointer (type-specific) */
    int   flags;    /* O_RDONLY, O_WRONLY, etc. */
} fd_entry_t;

/* fd_entry_t is 24 bytes on x86_64; one 4 KB leaf page therefore holds
 * 4096/24 = 170 entries (rest of the page wasted, ~16 bytes). The
 * static-assert pins the layout so a struct change doesn't silently
 * shrink/grow the leaf footprint. */
#define FD_LEAF_PER_PAGE   170
_Static_assert(sizeof(fd_entry_t) * FD_LEAF_PER_PAGE <= 4096,
               "FD_LEAF_PER_PAGE must fit in one 4KB page");

/* Hard ceiling — Linux sysctl_nr_open default = 1<<20 (1048576).
 * Bounded only by per-process pages_alloc + RLIMIT_NOFILE; the dir
 * needed to address 1M fds is ceil(1M/170)*8 ≈ 48 KB, well within
 * the buddy max-order. */
#define FD_CEILING       (1 << 20)

typedef struct {
    /* Top-level directory: each entry points to a leaf page of
     * FD_LEAF_PER_PAGE fd_entry_t's (or NULL if that leaf has never been
     * touched). The dir itself is pages_alloc'd. */
    fd_entry_t **entries_dir;
    int          dir_capacity;   /* number of leaf-pointer slots */
    int          leaf_count;     /* number of allocated leaves (for stats) */

    uint64_t    *free_bitmap;    /* one bit per slot, bit set = free */
    int          max_slots;      /* current capacity (multiple of FD_LEAF_PER_PAGE) */
    int          max_fd;         /* highest allocated fd + 1 */
} fd_table_t;

/* Internal: locate (and optionally allocate) the leaf page for fd.
 * fd_get/_set never call alloc; only fd_alloc/install do. */
static inline fd_entry_t *fd_leaf(const fd_table_t *fdt, int fd) {
    int leaf = fd / FD_LEAF_PER_PAGE;
    if (leaf >= fdt->dir_capacity) return 0;
    return fdt->entries_dir[leaf];
}

static inline fd_entry_t *fd_entry_at(const fd_table_t *fdt, int fd) {
    fd_entry_t *page = fd_leaf(fdt, fd);
    if (!page) return 0;
    return &page[fd % FD_LEAF_PER_PAGE];
}

/* Bitmap helpers — mark FD as used (clear bit) or free (set bit).
 * Callers must ensure fd is within max_slots. */
static inline void fd_mark_used(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] &= ~(1ULL << (fd % 64));
}
static inline void fd_mark_free(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] |= (1ULL << (fd % 64));
}

/* Find lowest free FD >= minfd within current capacity. Returns -1 if none.
 * Does NOT expand the table — caller (fd_alloc) handles expansion.
 * The bitmap is sized to round_up(max_slots, 64), with the slack bits
 * pre-marked as USED, so iterating over the round-up word count is safe. */
static inline int fd_find_free(fd_table_t *fdt, int minfd) {
    if (minfd < 0) minfd = 0;
    if (minfd >= fdt->max_slots) return -1;
    int alloc_slots = (fdt->max_slots + 63) & ~63;
    int nwords = alloc_slots / 64;
    int word = minfd / 64;
    int bit  = minfd % 64;
    uint64_t w = fdt->free_bitmap[word] & (~0ULL << bit);
    while (word < nwords) {
        if (w) {
            int fd = word * 64 + __builtin_ctzll(w);
            return (fd < fdt->max_slots) ? fd : -1;
        }
        if (++word < nwords)
            w = fdt->free_bitmap[word];
    }
    return -1;
}

/* Allocate the leaf page covering `fd` if not yet present. Caller has
 * ensured fd < fdt->max_slots. Returns 0 on success, -ENOMEM on OOM.
 * Used by fork to materialise child leaves only for live entries. */
int fd_table_leaf_ensure(fd_table_t *fdt, int fd);

/* Initialize a fresh FD table: allocate initial arrays + install stdio (0/1/2).
 * Returns 0 on success, -ENOMEM on OOM. */
int fd_table_init(fd_table_t *fdt);

/* Allocate empty table (no stdio) with at least `slots` capacity.
 * For fork: caller fills entries from parent manually. */
int fd_table_alloc_empty(fd_table_t *fdt, int slots);

/* Free backing pages; call before slab_free(proc). */
void fd_table_free(fd_table_t *fdt);

/* Allocate lowest free FD, expanding table on demand up to RLIMIT_NOFILE.
 * Returns fd number or -EMFILE. */
int fd_alloc(fd_table_t *fdt, int type, void *obj, int flags);

/* Install a duplicated entry at the lowest free fd >= minfd. Bumps refcount.
 * Used by dup/dup2/dup3/F_DUPFD where we need atomicity w.r.t. expansion:
 * caller holds no fd_entry_t* across this call. new_flags replaces src flags. */
int fd_dup_at(fd_table_t *fdt, int minfd, fd_entry_t src, int new_flags);

/* Install a duplicated entry at a specific fd (for dup2/dup3). If newfd is
 * beyond capacity, expand. Caller must have already closed any existing entry.
 * Returns 0 or -EMFILE/-ENOMEM. */
int fd_install_at(fd_table_t *fdt, int newfd, fd_entry_t src);

/* Close an FD. Returns 0 or -1. */
int fd_close(fd_table_t *fdt, int fd);

/* Get FD entry. Returns NULL if invalid.
 * Pointer is valid only until the next fd_alloc/fd_dup_at/fd_install_at
 * on the same table (expansion may reallocate entries[]). */
fd_entry_t *fd_get(fd_table_t *fdt, int fd);

#endif
