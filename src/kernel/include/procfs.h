/* CosmoRT procfs — virtual filesystem at /proc */
#ifndef PROCFS_H
#define PROCFS_H

#include <stddef.h>

/* Read callback: fill buf (up to size bytes) starting at offset.
 * Returns bytes written to buf. */
typedef int (*procfs_read_fn)(char *buf, int size, int offset, void *ctx);

/* Per-fd state for an open procfs file */
typedef struct {
    int handle;     /* procfs entry index + 1 */
    int offset;     /* current read position */
} procfs_fd_t;

/* Register a procfs entry. name is the filename under /proc (e.g. "dmesg"). */
void procfs_register(const char *name, procfs_read_fn fn, void *ctx);

/* Initialize procfs and register built-in entries (/proc/dmesg, etc.) */
void procfs_init(void);

/* VFS hooks — called when path starts with /proc/ */

/* Returns opaque handle (index+1) or 0 on failure. */
int procfs_open(const char *name);

/* Read from an open procfs handle. Returns bytes read. */
int procfs_read(int handle, char *buf, int size, int offset);

/* Close a procfs handle. */
void procfs_close(int handle);

/* Stat a procfs entry by name. Returns 0 on success, -1 if not found. */
int procfs_stat(const char *name, int *size_out);

/* Iterate procfs entries starting at offset. cb returns 0 to continue, nonzero to stop.
 * Returns number of entries visited. */
int procfs_iterate(int offset, int (*cb)(const char *name, void *ctx), void *ctx);

/* Allocate / free procfs_fd_t from a small static pool */
procfs_fd_t *procfs_fd_alloc(void);
void procfs_fd_free(procfs_fd_t *pf);

#endif
