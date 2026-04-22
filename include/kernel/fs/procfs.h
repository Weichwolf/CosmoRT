/* CosmoRT procfs — virtual filesystem at /proc */
#ifndef PROCFS_H
#define PROCFS_H

#include <stddef.h>

/* Read callback: fill buf (up to size bytes) starting at offset.
 * Returns bytes written to buf. */
typedef int (*procfs_read_fn)(char *buf, int size, int offset, void *ctx);

/* Write callback: consume up to size bytes of buf starting at offset.
 * Returns bytes consumed, or a negative errno on error. Entries without
 * a write handler return -EACCES on write. */
typedef long (*procfs_write_fn)(const char *buf, int size, int offset, void *ctx);

/* Per-fd state for an open procfs file */
typedef struct {
    int handle;     /* procfs entry index + 1, or -2 for per-pid dynamic */
    int offset;     /* current read position */
    char name[64];  /* for per-pid dynamic entries (handle == -2) */
} procfs_fd_t;

/* Register a procfs entry. name is the filename under /proc (e.g. "dmesg"). */
void procfs_register(const char *name, procfs_read_fn fn, void *ctx);

/* Register a read-write procfs entry. Either read or write may be NULL to
 * restrict the respective direction (open returns -EACCES for forbidden
 * modes). */
void procfs_register_rw(const char *name, procfs_read_fn rfn,
                        procfs_write_fn wfn, void *ctx);

/* Write to an open procfs handle. Returns bytes consumed, or -errno. */
long procfs_write(int handle, const char *buf, int size, int offset);

/* Query writability of an entry by name. Returns 1 if writable, else 0. */
int procfs_writable(const char *name);

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

/* Per-PID routing: read /proc/<pid>/<file> or /proc/self/<file>.
 * Returns bytes read, or -1 if not a per-pid path. */
int procfs_pid_read(const char *name, char *buf, int size, int offset);

/* Check if a per-pid procfs path exists. Returns 0=no, 1=file, 2=symlink. */
int procfs_pid_exists(const char *name);

#endif
