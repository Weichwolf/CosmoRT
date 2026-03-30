/* CosmoRT File Descriptor Table */
#ifndef FD_H
#define FD_H

#include <stdint.h>

#define FD_NONE    0
#define FD_SERIAL  1
#define FD_PIPE    2
#define FD_SOCKET  3
#define FD_DEVICE  4
#define FD_FILE    5
#define FD_PROCFS  6
#define FD_EPOLL   7
#define FD_EVENTFD 8
#define FD_TIMERFD 9
#define FD_INOTIFY 10
#define FD_PTY_MASTER 11
#define FD_PTY_SLAVE  12
#define FD_UNIX_SOCK  13

#define FD_MAX 1024
#define FD_BITMAP_WORDS (FD_MAX / 64)

typedef struct {
    int   type;
    void *obj;
    int   flags;
} fd_entry_t;

typedef struct {
    fd_entry_t entries[FD_MAX];
    uint64_t   free_bitmap[FD_BITMAP_WORDS];
    int        max_fd;
} fd_table_t;

static inline void fd_mark_used(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] &= ~(1ULL << (fd % 64));
}
static inline void fd_mark_free(fd_table_t *fdt, int fd) {
    fdt->free_bitmap[fd / 64] |= (1ULL << (fd % 64));
}

static inline int fd_find_free(fd_table_t *fdt, int minfd) {
    if (minfd < 0) minfd = 0;
    if (minfd >= FD_MAX) return -1;
    int word = minfd / 64;
    int bit  = minfd % 64;
    uint64_t w = fdt->free_bitmap[word] & (~0ULL << bit);
    while (word < FD_BITMAP_WORDS) {
        if (w) {
            int fd = word * 64 + __builtin_ctzll(w);
            return (fd < FD_MAX) ? fd : -1;
        }
        if (++word < FD_BITMAP_WORDS)
            w = fdt->free_bitmap[word];
    }
    return -1;
}

void fd_table_init(fd_table_t *fdt);

int fd_alloc(fd_table_t *fdt, int type, void *obj, int flags);

int fd_close(fd_table_t *fdt, int fd);

fd_entry_t *fd_get(fd_table_t *fdt, int fd);

#endif
