/* CosmoRT Pseudo-Terminal (PTY) — master/slave pairs for VTs
 *
 * Master side: keyboard input → process (via input buffer)
 * Slave side: process output → VT renderer (via output buffer)
 * Line discipline: echo, canonical mode, signal generation, termios
 */
#ifndef PTY_H
#define PTY_H

#include <stdint.h>
#include "spinlock.h"

#define PTY_MAX       4
#define PTY_BUF_SIZE  4096
#define PTY_LINE_MAX  256

/* Linux kernel struct termios (36 bytes, matches x86_64 ABI) */
#define NCCS 19
struct kernel_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
};

/* c_iflag bits */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

/* c_oflag bits */
#define OPOST   0000001
#define ONLCR   0000004

/* c_cflag bits */
#define B38400  0000017
#define CS8     0000060
#define CREAD   0000200

/* c_lflag bits */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0100000

/* c_cc indices */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT  15
#define VEOL2   16

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

    /* Full termios state (36 bytes) */
    struct kernel_termios termios;

    /* Ownership */
    int      slave_pid;
    int      in_use;
    int      fg_pgid;       /* foreground process group (TIOCSPGRP/TIOCGPGRP) */

    /* Terminal dimensions */
    struct pty_winsize {
        uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
    } ws;

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
