/* CosmoRT PTY — pseudo-terminal pairs with line discipline */

#include "vt/pty.h"
#include "vt/vt.h"
#include "memops.h"
#include "hw/serial.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "core/event_queue.h"

_Static_assert(sizeof(((pty_t *)0)->line_buf) == PTY_LINE_MAX,
               "PTY_LINE_MAX must match line_buf array size");

extern void sched_add(thread_t *t);

static pty_t pty_pool[PTY_MAX];

static int ring_count(int head, int tail) {
    return (tail - head + PTY_BUF_SIZE) % PTY_BUF_SIZE;
}

static int ring_space(int head, int tail) {
    return PTY_BUF_SIZE - 1 - ring_count(head, tail);
}

static void ring_put(char *buf, int *tail, char c) {
    buf[*tail] = c;
    *tail = (*tail + 1) % PTY_BUF_SIZE;
}

static char ring_get(char *buf, int *head) {
    char c = buf[*head];
    *head = (*head + 1) % PTY_BUF_SIZE;
    return c;
}

static void pty_output_char(pty_t *p, char c) {
    if (ring_space(p->output_head, p->output_tail) > 0)
        ring_put(p->output_buf, &p->output_tail, c);
}

static void pty_output_str(pty_t *p, const char *s) {
    while (*s) pty_output_char(p, *s++);
}

static void pty_output_cooked(pty_t *p, char c) {
    if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR) && c == '\n') {
        pty_output_char(p, '\r');
        pty_output_char(p, '\n');
    } else {
        pty_output_char(p, c);
    }
}

static void termios_init(struct kernel_termios *t) {
    kmemset(t, 0, sizeof(*t));
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = B38400 | CS8 | CREAD;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    t->c_line  = 0;
    t->c_cc[VINTR]    = 3;
    t->c_cc[VQUIT]    = 28;
    t->c_cc[VERASE]   = 127;
    t->c_cc[VKILL]    = 21;
    t->c_cc[VEOF]     = 4;
    t->c_cc[VTIME]    = 0;
    t->c_cc[VMIN]     = 1;
    t->c_cc[VSWTC]    = 0;
    t->c_cc[VSTART]   = 17;
    t->c_cc[VSTOP]    = 19;
    t->c_cc[VSUSP]    = 26;
    t->c_cc[VEOL]     = 0;
    t->c_cc[VREPRINT] = 18;
    t->c_cc[VDISCARD] = 15;
    t->c_cc[VWERASE]  = 23;
    t->c_cc[VLNEXT]   = 22;
    t->c_cc[VEOL2]    = 0;
}

void pty_init(void) {
    kmemset(pty_pool, 0, sizeof(pty_pool));
    for (int i = 0; i < PTY_MAX; i++) {
        termios_init(&pty_pool[i].termios);
        pty_pool[i].lock = (spinlock_t)SPINLOCK_INIT;
    }
    serial_puts("pty: ");
    serial_putchar('0' + PTY_MAX);
    serial_puts(" slots\n");
}

int pty_alloc(void) {
    for (int i = 0; i < PTY_MAX; i++) {
        uint64_t flags;
        spin_lock_irq(&pty_pool[i].lock, &flags);
        if (!pty_pool[i].in_use) {
            pty_pool[i].in_use = 1;
            pty_pool[i].input_head = pty_pool[i].input_tail = 0;
            pty_pool[i].output_head = pty_pool[i].output_tail = 0;
            pty_pool[i].line_pos = 0;
            pty_pool[i].slave_pid = 0;
            pty_pool[i].blocked_reader = 0;
            termios_init(&pty_pool[i].termios);
            pty_pool[i].ws.ws_col = (uint16_t)vt_cols();
            pty_pool[i].ws.ws_row = (uint16_t)vt_rows();
            pty_pool[i].ws.ws_xpixel = 0;
            pty_pool[i].ws.ws_ypixel = 0;
            spin_unlock_irq(&pty_pool[i].lock, flags);
            return i;
        }
        spin_unlock_irq(&pty_pool[i].lock, flags);
    }
    return -1;
}

pty_t *pty_get(int id) {
    if (id < 0 || id >= PTY_MAX) return 0;
    return &pty_pool[id];
}

static inline int pty_canon(pty_t *p)  { return (p->termios.c_lflag & ICANON) != 0; }
static inline int pty_echo(pty_t *p)   { return (p->termios.c_lflag & ECHO)   != 0; }
static inline int pty_isig(pty_t *p)   { return (p->termios.c_lflag & ISIG)   != 0; }
static inline int pty_icrnl(pty_t *p)  { return (p->termios.c_iflag & ICRNL)  != 0; }

static void send_signal_to_fg(pty_t *p, int sig) {
    int pgid = p->fg_pgid;
    if (pgid <= 0) pgid = p->slave_pid;
    if (pgid <= 0) return;

    extern process_t *proc_find(uint32_t pid);
    for (int i = 1; i < PID_TABLE_MAX; i++) {
        process_t *proc = proc_find((uint32_t)i);
        if (!proc) continue;
        if ((int)proc->pgid != pgid) continue;
        proc->sig_pending |= SIG_BIT(sig);
        thread_t *t = proc->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED)
                event_post(t, EQ_PIPE_DATA, 0);
            t = t->proc_next;
        }
    }
}

static void line_flush(pty_t *p) {
    for (int i = 0; i < p->line_pos; i++) {
        if (ring_space(p->input_head, p->input_tail) > 0)
            ring_put(p->input_buf, &p->input_tail, p->line_buf[i]);
    }
    if (ring_space(p->input_head, p->input_tail) > 0)
        ring_put(p->input_buf, &p->input_tail, '\n');
    p->line_pos = 0;
}

int pty_master_write(int id, const char *buf, int len) {
    if (id < 0 || id >= PTY_MAX) return -1;
    pty_t *p = &pty_pool[id];
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    for (int i = 0; i < len; i++) {
        char c = buf[i];

        if (pty_icrnl(p) && c == '\r')
            c = '\n';

        if (pty_isig(p)) {
            if (c == (char)p->termios.c_cc[VINTR]) {
                send_signal_to_fg(p, SIGINT);
                if (pty_echo(p)) pty_output_str(p, "^C\n");
                p->line_pos = 0;
                continue;
            }
            if (c == (char)p->termios.c_cc[VQUIT]) {
                send_signal_to_fg(p, SIGQUIT);
                if (pty_echo(p)) pty_output_str(p, "^\\\n");
                p->line_pos = 0;
                continue;
            }
            if (c == (char)p->termios.c_cc[VSUSP]) {
                send_signal_to_fg(p, SIGTSTP);
                if (pty_echo(p)) pty_output_str(p, "^Z\n");
                p->line_pos = 0;
                continue;
            }
        }

        if (pty_canon(p) && c == (char)p->termios.c_cc[VEOF]) {
            if (p->line_pos > 0)
                line_flush(p);
            continue;
        }

        if (pty_canon(p)) {
            if (c == '\b' || c == (char)p->termios.c_cc[VERASE]) {
                if (p->line_pos > 0) {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                continue;
            }
            if (c == (char)p->termios.c_cc[VKILL]) {
                while (p->line_pos > 0) {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                continue;
            }
            if (c == (char)p->termios.c_cc[VWERASE] && p->termios.c_cc[VWERASE]) {
                while (p->line_pos > 0 && p->line_buf[p->line_pos - 1] == ' ') {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                while (p->line_pos > 0 && p->line_buf[p->line_pos - 1] != ' ') {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                continue;
            }
            if (c == '\n') {
                if (pty_echo(p)) pty_output_cooked(p, '\n');
                line_flush(p);
                continue;
            }
            if (p->line_pos < PTY_LINE_MAX - 1) {
                p->line_buf[p->line_pos++] = c;
                if (pty_echo(p)) {
                    if ((p->termios.c_lflag & ECHOCTL) &&
                        (unsigned char)c < 32 && c != '\t' && c != '\n')
                    {
                        pty_output_char(p, '^');
                        pty_output_char(p, (char)(c + '@'));
                    } else {
                        pty_output_cooked(p, c);
                    }
                }
            }
        } else {
            if (ring_space(p->input_head, p->input_tail) > 0)
                ring_put(p->input_buf, &p->input_tail, c);
            if (pty_echo(p)) pty_output_cooked(p, c);
        }
    }

    if (p->blocked_reader &&
        (ring_count(p->input_head, p->input_tail) > 0 ||
         ring_count(p->output_head, p->output_tail) > 0)) {
        thread_t *reader = p->blocked_reader;
        p->blocked_reader = 0;
        event_post(reader, EQ_PIPE_DATA, 0);
    }

    spin_unlock_irq(&p->lock, flags);

    if (ring_count(p->input_head, p->input_tail) > 0)
        epoll_wake_all();

    return len;
}

int pty_master_read(int id, char *buf, int len) {
    if (id < 0 || id >= PTY_MAX) return 0;
    pty_t *p = &pty_pool[id];
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    int n = ring_count(p->output_head, p->output_tail);
    if (n > len) n = len;
    for (int i = 0; i < n; i++)
        buf[i] = ring_get(p->output_buf, &p->output_head);

    spin_unlock_irq(&p->lock, flags);
    return n;
}

int pty_slave_write(int id, const char *buf, int len) {
    if (id < 0 || id >= PTY_MAX) return -1;
    pty_t *p = &pty_pool[id];
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    int wrote = 0;
    for (int i = 0; i < len; i++) {
        if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR) && buf[i] == '\n') {
            if (ring_space(p->output_head, p->output_tail) < 2) break;
            ring_put(p->output_buf, &p->output_tail, '\r');
            ring_put(p->output_buf, &p->output_tail, '\n');
        } else {
            if (ring_space(p->output_head, p->output_tail) <= 0) break;
            ring_put(p->output_buf, &p->output_tail, buf[i]);
        }
        wrote++;
    }

    spin_unlock_irq(&p->lock, flags);
    return wrote;
}

int pty_input_direct(int id, const char *buf, int len) {
    if (id < 0 || id >= PTY_MAX) return 0;
    pty_t *p = &pty_pool[id];
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);
    int written = 0;
    for (int i = 0; i < len; i++) {
        if (((p->input_tail + 1) % PTY_BUF_SIZE) == p->input_head) break;
        p->input_buf[p->input_tail] = buf[i];
        p->input_tail = (p->input_tail + 1) % PTY_BUF_SIZE;
        written++;
    }
    if (written > 0 && p->blocked_reader) {
        thread_t *reader = p->blocked_reader;
        p->blocked_reader = 0;
        event_post(reader, EQ_PIPE_DATA, 0);
    }
    spin_unlock_irq(&p->lock, flags);
    if (written > 0)
        epoll_wake_all();
    return written;
}

int pty_slave_read(int id, char *buf, int len) {
    if (id < 0 || id >= PTY_MAX) return 0;
    pty_t *p = &pty_pool[id];
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    int n = ring_count(p->input_head, p->input_tail);
    if (n > len) n = len;
    for (int i = 0; i < n; i++)
        buf[i] = ring_get(p->input_buf, &p->input_head);

    spin_unlock_irq(&p->lock, flags);
    return n;
}
