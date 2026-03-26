/* CosmoRT File Descriptor Table
 *
 * Each process has a static FD table (1024 entries).
 * FDs are integers indexing into this table.
 * Kernel objects (serial, pipe, socket) are referenced via type+pointer.
 *
 * Free-bitmap: bit set = slot free. __builtin_ctzll finds lowest free FD
 * in O(1) per 64-bit word, O(FD_MAX/64) worst-case = 16 iterations.
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

/* O_RDONLY, O_WRONLY, O_RDWR, O_NONBLOCK — from linux.h */

#define FD_MAX 1024
#define FD_BITMAP_WORDS (FD_MAX / 64)

typedef struct {
    int   type;     /* FD_NONE if unused */
    void *obj;      /* kernel object pointer (type-specific) */
    int   flags;    /* O_RDONLY, O_WRONLY, etc. */
} fd_entry_t;

typedef struct {
    fd_entry_t entries[FD_MAX];
    uint64_t   free_bitmap[FD_BITMAP_WORDS]; /* bit set = free */
    int        max_fd;  /* highest allocated fd + 1 */
} fd_table_t;

/* Bitmap helpers — mark FD as used (clear bit) or free (set bit) */
static inline void fd_mark_used(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] &= ~(1ULL << (fd % 64));
}
static inline void fd_mark_free(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] |= (1ULL << (fd % 64));
}

/* Find lowest free FD >= minfd. Returns -1 if none. */
static inline int fd_find_free(fd_table_t *fdt, int minfd) {
    if (minfd < 0) minfd = 0;
    if (minfd >= FD_MAX) return -1;
    int word = minfd / 64;
    int bit  = minfd % 64;
    /* First word: mask out bits below minfd */
    uint64_t w = fdt->free_bitmap[word] & (~0ULL << bit);
    for (; word < FD_BITMAP_WORDS; word++, w = fdt->free_bitmap[word]) {
        if (w) {
            int fd = word * 64 + __builtin_ctzll(w);
            return (fd < FD_MAX) ? fd : -1;
        }
    }
    return -1;
}

/* Initialize FD table with stdin(0)/stdout(1)/stderr(2) → serial */
void fd_table_init(fd_table_t *fdt);

/* Allocate lowest free FD. Returns fd number or -EMFILE. */
int fd_alloc(fd_table_t *fdt, int type, void *obj, int flags);

/* Close an FD. Returns 0 or -1. */
int fd_close(fd_table_t *fdt, int fd);

/* Get FD entry. Returns NULL if invalid. */
fd_entry_t *fd_get(fd_table_t *fdt, int fd);

#endif
