/* CosmoRT procfs — virtual filesystem at /proc */
#ifndef PROCFS_H
#define PROCFS_H

#include <stddef.h>

typedef int (*procfs_read_fn)(char *buf, int size, int offset, void *ctx);

typedef struct {
    int handle;
    int offset;
    char name[64];
} procfs_fd_t;

void procfs_register(const char *name, procfs_read_fn fn, void *ctx);

void procfs_init(void);

int procfs_open(const char *name);

int procfs_read(int handle, char *buf, int size, int offset);

void procfs_close(int handle);

int procfs_stat(const char *name, int *size_out);

int procfs_iterate(int offset, int (*cb)(const char *name, void *ctx), void *ctx);

procfs_fd_t *procfs_fd_alloc(void);
void procfs_fd_free(procfs_fd_t *pf);

int procfs_pid_read(const char *name, char *buf, int size, int offset);

int procfs_pid_exists(const char *name);

#endif
