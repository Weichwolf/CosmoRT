/* CosmoRT PTY — pseudo-terminal pairs with line discipline */

#include "vt/pty.h"
#include "vt/vt.h"
#include "memops.h"
#include "hw/serial.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "mm/slab.h"
#include "core/waitqueue.h"

_Static_assert(sizeof(((pty_t *)0)->line_buf) == PTY_LINE_MAX,
               "PTY_LINE_MAX must match line_buf array size");

extern void sched_add(thread_t *t);

/* SIGINT, SIGQUIT — from linux.h (via process.h chain) */

static slab_t      pty_slab;
static int         pty_slab_inited;
static pty_t      *pty_list_head;
static int         pty_next_id;
static spinlock_t  pty_list_lock = SPINLOCK_INIT;

static void pty_slab_ensure_init(void) {
    if (__builtin_expect(pty_slab_inited, 1)) return;
    slab_init_dynamic(&pty_slab, (int)sizeof(pty_t), 1);
    pty_slab_inited = 1;
}

/* ── Ring buffer helpers ───────────────────────────── */

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

/* ── Output helper (echo to output buffer) ─────────── */

static void pty_output_char(pty_t *p, char c) {
    if (ring_space(p->output_head, p->output_tail) > 0)
        ring_put(p->output_buf, &p->output_tail, c);
}

static void pty_output_str(pty_t *p, const char *s) {
    while (*s) pty_output_char(p, *s++);
}

/* Output with OPOST/ONLCR processing */
static void pty_output_cooked(pty_t *p, char c) {
    if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR) && c == '\n') {
        pty_output_char(p, '\r');
        pty_output_char(p, '\n');
    } else {
        pty_output_char(p, c);
    }
}

/* ── Default termios (matches Linux defaults) ──────── */

static void termios_init(struct kernel_termios *t) {
    kmemset(t, 0, sizeof(*t));
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = B38400 | CS8 | CREAD;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
    t->c_line  = 0;
    /* Control characters — Linux defaults */
    t->c_cc[VINTR]    = 3;    /* ^C */
    t->c_cc[VQUIT]    = 28;   /* ^\ */
    t->c_cc[VERASE]   = 127;  /* DEL */
    t->c_cc[VKILL]    = 21;   /* ^U */
    t->c_cc[VEOF]     = 4;    /* ^D */
    t->c_cc[VTIME]    = 0;
    t->c_cc[VMIN]     = 1;
    t->c_cc[VSWTC]    = 0;
    t->c_cc[VSTART]   = 17;   /* ^Q */
    t->c_cc[VSTOP]    = 19;   /* ^S */
    t->c_cc[VSUSP]    = 26;   /* ^Z */
    t->c_cc[VEOL]     = 0;
    t->c_cc[VREPRINT] = 18;   /* ^R */
    t->c_cc[VDISCARD] = 15;   /* ^O */
    t->c_cc[VWERASE]  = 23;   /* ^W */
    t->c_cc[VLNEXT]   = 22;   /* ^V */
    t->c_cc[VEOL2]    = 0;
}

/* ── Init ──────────────────────────────────────────── */

void pty_init(void) {
    pty_slab_ensure_init();
    serial_puts("pty: slab-backed, on-demand\n");
}

int pty_alloc(void) {
    pty_slab_ensure_init();

    pty_t *p = (pty_t *)slab_alloc(&pty_slab);
    if (!p) return -1;

    kmemset(p, 0, sizeof(*p));
    p->lock = (spinlock_t)SPINLOCK_INIT;
    p->in_use = 1;
    init_waitqueue_head(&p->m2s_wq);
    init_waitqueue_head(&p->s2m_wq);
    termios_init(&p->termios);
    p->ws.ws_col = (uint16_t)vt_cols();
    p->ws.ws_row = (uint16_t)vt_rows();

    uint64_t flags;
    spin_lock_irq(&pty_list_lock, &flags);
    p->id = pty_next_id++;
    p->list_next = pty_list_head;
    pty_list_head = p;
    spin_unlock_irq(&pty_list_lock, flags);

    return p->id;
}

pty_t *pty_get(int id) {
    if (id < 0 || id >= PTY_DEV_ID_MAX) return 0;
    uint64_t flags;
    spin_lock_irq(&pty_list_lock, &flags);
    pty_t *p = pty_list_head;
    while (p && p->id != id) p = p->list_next;
    spin_unlock_irq(&pty_list_lock, flags);
    return p;
}

/* ── Convenience accessors for termios fields ──────── */

static inline int pty_canon(pty_t *p)  { return (p->termios.c_lflag & ICANON) != 0; }
static inline int pty_echo(pty_t *p)   { return (p->termios.c_lflag & ECHO)   != 0; }
static inline int pty_isig(pty_t *p)   { return (p->termios.c_lflag & ISIG)   != 0; }
static inline int pty_icrnl(pty_t *p)  { return (p->termios.c_iflag & ICRNL)  != 0; }

/* ── Master write: keyboard → line discipline → input buffer ── */

/* Send signal to foreground process group (or slave_pid fallback).
 * IRQ-safe: only sets sig_pending + signal_wake_up, no do_kill. */
static void send_signal_to_fg(pty_t *p, int sig) {
    int pgid = p->fg_pgid;
    if (pgid <= 0) pgid = p->slave_pid;
    if (pgid <= 0) return;

    extern process_t *proc_find(uint32_t pid);
    extern void signal_wake_up(thread_t *t);
    int cap = pid_table_capacity();
    for (int i = 1; i < cap; i++) {
        process_t *proc = proc_find((uint32_t)i);
        if (!proc) continue;
        if ((int)proc->pgid != pgid) continue;
        __sync_fetch_and_or(&proc->sig_pending, SIG_BIT(sig));
        thread_t *t = proc->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED) signal_wake_up(t);
            t = t->proc_next;
        }
    }
}

static void line_flush(pty_t *p) {
    /* Flush line buffer into input ring */
    for (int i = 0; i < p->line_pos; i++) {
        if (ring_space(p->input_head, p->input_tail) > 0)
            ring_put(p->input_buf, &p->input_tail, p->line_buf[i]);
    }
    /* Add newline */
    if (ring_space(p->input_head, p->input_tail) > 0)
        ring_put(p->input_buf, &p->input_tail, '\n');
    p->line_pos = 0;
}

int pty_master_write(int id, const char *buf, int len) {
    pty_t *p = pty_get(id);
    if (!p) return -1;
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    for (int i = 0; i < len; i++) {
        char c = buf[i];

        /* ICRNL: CR → NL on input */
        if (pty_icrnl(p) && c == '\r')
            c = '\n';

        /* Signal characters — only when ISIG is set */
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

        /* EOF (^D) — only in canonical mode */
        if (pty_canon(p) && c == (char)p->termios.c_cc[VEOF]) {
            if (p->line_pos > 0)
                line_flush(p);
            /* else: EOF marker — leave input empty so read returns 0 */
            continue;
        }

        if (pty_canon(p)) {
            /* Backspace (VERASE) */
            if (c == '\b' || c == (char)p->termios.c_cc[VERASE]) {
                if (p->line_pos > 0) {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                continue;
            }
            /* Kill line (VKILL = ^U) */
            if (c == (char)p->termios.c_cc[VKILL]) {
                while (p->line_pos > 0) {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                continue;
            }
            /* Word erase (VWERASE = ^W) */
            if (c == (char)p->termios.c_cc[VWERASE] && p->termios.c_cc[VWERASE]) {
                /* Skip trailing spaces */
                while (p->line_pos > 0 && p->line_buf[p->line_pos - 1] == ' ') {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                /* Delete word */
                while (p->line_pos > 0 && p->line_buf[p->line_pos - 1] != ' ') {
                    p->line_pos--;
                    if (pty_echo(p)) pty_output_str(p, "\b \b");
                }
                continue;
            }
            /* Enter → flush line */
            if (c == '\n') {
                if (pty_echo(p)) pty_output_cooked(p, '\n');
                line_flush(p);
                continue;
            }
            /* Buffer printable chars */
            if (p->line_pos < PTY_LINE_MAX - 1) {
                p->line_buf[p->line_pos++] = c;
                if (pty_echo(p)) {
                    /* ECHOCTL: echo control chars as ^X */
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
            /* Raw mode: pass directly to input */
            if (ring_space(p->input_head, p->input_tail) > 0)
                ring_put(p->input_buf, &p->input_tail, c);
            if (pty_echo(p)) pty_output_cooked(p, c);
        }
    }

    int input_avail  = ring_count(p->input_head,  p->input_tail)  > 0;
    int output_avail = ring_count(p->output_head, p->output_tail) > 0;

    spin_unlock_irq(&p->lock, flags);

    /* Wake direction-specific waiters. m2s parks slave readers on input_buf;
     * s2m parks master readers on echo'd output. Multi-waiter safe via wq. */
    if (input_avail)  wake_up(&p->m2s_wq);
    if (output_avail) wake_up(&p->s2m_wq);
    return len;
}

/* ── Master read: drain output buffer (process → VT) ── */

int pty_master_read(int id, char *buf, int len) {
    pty_t *p = pty_get(id);
    if (!p) return 0;
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    int n = ring_count(p->output_head, p->output_tail);
    if (n > len) n = len;
    for (int i = 0; i < n; i++)
        buf[i] = ring_get(p->output_buf, &p->output_head);

    spin_unlock_irq(&p->lock, flags);
    return n;
}

/* ── Slave write: process stdout → output buffer ───── */

int pty_slave_write(int id, const char *buf, int len) {
    pty_t *p = pty_get(id);
    if (!p) return -1;
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    int wrote = 0;
    for (int i = 0; i < len; i++) {
        /* OPOST + ONLCR: NL → CR+NL on output */
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
    if (wrote > 0) wake_up(&p->s2m_wq);
    return wrote;
}

/* ── Slave read: process stdin ← input buffer ──────── */

/* Write directly to input ring, bypassing canonical mode line buffer.
 * Used for terminal responses (DSR, cursor position) that must reach
 * the process regardless of canonical/raw mode state. */
int pty_input_direct(int id, const char *buf, int len) {
    pty_t *p = pty_get(id);
    if (!p) return 0;
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);
    int written = 0;
    for (int i = 0; i < len; i++) {
        if (((p->input_tail + 1) % PTY_BUF_SIZE) == p->input_head) break;
        p->input_buf[p->input_tail] = buf[i];
        p->input_tail = (p->input_tail + 1) % PTY_BUF_SIZE;
        written++;
    }
    spin_unlock_irq(&p->lock, flags);
    if (written > 0) wake_up(&p->m2s_wq);
    return written;
}

int pty_slave_read(int id, char *buf, int len) {
    pty_t *p = pty_get(id);
    if (!p) return 0;
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    int n = ring_count(p->input_head, p->input_tail);
    if (n > len) n = len;
    for (int i = 0; i < n; i++)
        buf[i] = ring_get(p->input_buf, &p->input_head);

    spin_unlock_irq(&p->lock, flags);
    return n;
}
