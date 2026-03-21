/* CosmoRT Pseudo-Terminal (PTY) — master/slave pairs for VTs
 *
 * Master side: keyboard input → process (via input buffer)
 * Slave side: process output → VT renderer (via output buffer)
 * Line discipline: echo, canonical mode, Ctrl+C/D/L
 */
#ifndef PTY_H
#define PTY_H

#include <stdint.h>
#include "spinlock.h"

#define PTY_MAX       4
#define PTY_BUF_SIZE  4096
#define PTY_LINE_MAX  256

typedef struct {
    /* Master → Slave (keyboard → process stdin) */
    char     input_buf[PTY_BUF_SIZE];
    int      input_head, input_tail;

    /* Slave → Master (process stdout → VT renderer) */
    char     output_buf[PTY_BUF_SIZE];
    int      output_head, output_tail;

    /* Line discipline */
    char     line_buf[PTY_LINE_MAX];
    int      line_pos;
    int      echo;          /* echo input back to output */
    int      canon;         /* canonical mode (line buffered) */

    /* Ownership */
    int      slave_pid;
    int      in_use;

    /* Blocked reader (thread waiting for input data) */
    struct thread *blocked_reader;

    spinlock_t lock;
} pty_t;

void pty_init(void);
int  pty_alloc(void);                                           /* returns pty index or -1 */
int  pty_master_write(int id, const char *buf, int len);        /* keyboard → input buf */
int  pty_master_read(int id, char *buf, int len);               /* output buf → VT */
int  pty_slave_write(int id, const char *buf, int len);         /* process → output buf */
int  pty_slave_read(int id, char *buf, int len);                /* input buf → process */

/* Get PTY by index (for ioctl winsize etc.) */
pty_t *pty_get(int id);

#endif
