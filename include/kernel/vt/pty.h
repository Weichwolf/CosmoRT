/* CosmoRT Pseudo-Terminal (PTY) — master/slave pairs for VTs */
#ifndef PTY_H
#define PTY_H

#include <stdint.h>
#include "core/mutex.h"

#define PTY_MAX       12
#define PTY_BUF_SIZE  4096
#define PTY_LINE_MAX  256

#define NCCS 19
struct kernel_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
};

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

#define OPOST   0000001
#define ONLCR   0000004

#define B38400  0000017
#define CS8     0000060
#define CREAD   0000200

#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHOCTL 0001000
#define ECHOKE  0004000
#define IEXTEN  0100000

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
    char     input_buf[PTY_BUF_SIZE];
    int      input_head, input_tail;

    char     output_buf[PTY_BUF_SIZE];
    int      output_head, output_tail;

    char     line_buf[PTY_LINE_MAX];
    int      line_pos;

    struct kernel_termios termios;

    int      slave_pid;
    int      in_use;
    int      fg_pgid;

    struct pty_winsize {
        uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
    } ws;

    struct thread *blocked_reader;

    mutex_t lock;
} pty_t;

void pty_init(void);
int  pty_alloc(void);
int  pty_master_write(int id, const char *buf, int len);
int  pty_master_read(int id, char *buf, int len);
int  pty_slave_write(int id, const char *buf, int len);
int  pty_slave_read(int id, char *buf, int len);

pty_t *pty_get(int id);

#endif
