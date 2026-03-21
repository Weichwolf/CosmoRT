/* CosmoRT File Descriptor Table
 *
 * Each process has a static FD table (256 entries, POSIX minimum 20).
 * FDs are integers indexing into this table.
 * Kernel objects (serial, pipe, socket) are referenced via type+pointer.
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

/* Open flags (subset of POSIX) */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002

#define FD_MAX 256

typedef struct {
    int   type;     /* FD_NONE if unused */
    void *obj;      /* kernel object pointer (type-specific) */
    int   flags;    /* O_RDONLY, O_WRONLY, etc. */
} fd_entry_t;

typedef struct {
    fd_entry_t entries[FD_MAX];
    int        max_fd;  /* highest allocated fd + 1 */
} fd_table_t;

/* Initialize FD table with stdin(0)/stdout(1)/stderr(2) → serial */
void fd_table_init(fd_table_t *fdt);

/* Allocate next free FD. Returns fd number or -1. */
int fd_alloc(fd_table_t *fdt, int type, void *obj, int flags);

/* Close an FD. Returns 0 or -1. */
int fd_close(fd_table_t *fdt, int fd);

/* Get FD entry. Returns NULL if invalid. */
fd_entry_t *fd_get(fd_table_t *fdt, int fd);

#endif
