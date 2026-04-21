/* CosmoRT File Descriptor Table
 *
 * Per-process FD table grows on demand (Linux expand_fdtable model).
 * Initial FD_INIT_SLOTS, doubled when fd_alloc runs out of slots, up to
 * FD_CEILING (buddy-allocator cap). RLIMIT_NOFILE bounds the actual
 * user-visible FD number; default FD_DEFAULT_NOFILE = 1024 (Linux ulimit -n).
 *
 * Layout:
 *   entries[]       — fd_entry_t array, pages_alloc backed (contiguous)
 *   free_bitmap[]   — one bit per slot, bit set = free
 *   max_slots       — current capacity (power of 2, >= FD_INIT_SLOTS)
 *   max_fd          — highest allocated fd + 1
 */
#ifndef FD_H
#define FD_H

#include <stdint.h>

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

/* Linux BITS_PER_LONG on x86_64 */
#define FD_INIT_SLOTS     64

/* Linux ulimit -n default */
#define FD_DEFAULT_NOFILE 1024

/* Hard ceiling: bounded by buddy allocator (pages_alloc max 512 pages = 2MB).
 * sizeof(fd_entry_t) = 24 → 2MB/24 ≈ 87381 → power-of-two floor = 65536.
 * Linux cap is 1<<20, but our page allocator can't serve contiguous 24MB.
 * Single-user: 64k fds per process is 64x Linux default ulimit. */
#define FD_CEILING       65536

typedef struct {
    int   type;     /* FD_NONE if unused */
    void *obj;      /* kernel object pointer (type-specific) */
    int   flags;    /* O_RDONLY, O_WRONLY, etc. */
} fd_entry_t;

typedef struct {
    fd_entry_t *entries;      /* pages_alloc backed, max_slots entries */
    uint64_t   *free_bitmap;  /* pages_alloc backed, max_slots/64 words */
    int         max_slots;    /* current capacity (power of 2) */
    int         max_fd;       /* highest allocated fd + 1 */
} fd_table_t;

/* Bitmap helpers — mark FD as used (clear bit) or free (set bit).
 * Callers must ensure fd is within max_slots. */
static inline void fd_mark_used(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] &= ~(1ULL << (fd % 64));
}
static inline void fd_mark_free(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] |= (1ULL << (fd % 64));
}

/* Find lowest free FD >= minfd within current capacity. Returns -1 if none.
 * Does NOT expand the table — caller (fd_alloc) handles expansion. */
static inline int fd_find_free(fd_table_t *fdt, int minfd) {
    if (minfd < 0) minfd = 0;
    if (minfd >= fdt->max_slots) return -1;
    int nwords = fdt->max_slots / 64;
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
