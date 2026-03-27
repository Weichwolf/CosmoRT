/* CosmoRT PTY — pseudo-terminal pairs with line discipline */

#include "vt/pty.h"
#include "vt/vt.h"
#include "memops.h"
#include "hw/serial.h"
#include "proc/process.h"
#include "proc/thread.h"

_Static_assert(sizeof(((pty_t *)0)->line_buf) == PTY_LINE_MAX,
               "PTY_LINE_MAX must match line_buf array size");

extern void sched_add(thread_t *t);

/* SIGINT, SIGQUIT — from linux.h (via process.h chain) */

static pty_t pty_pool[PTY_MAX];

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

/* ── Init ──────────────────────────────────────────── */

void pty_init(void) {
    kmemset(pty_pool, 0, sizeof(pty_pool));
    for (int i = 0; i < PTY_MAX; i++) {
        pty_pool[i].echo = 1;
        pty_pool[i].canon = 1;
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
            pty_pool[i].echo = 1;
            pty_pool[i].canon = 1;
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

/* ── Master write: keyboard → line discipline → input buffer ── */

/* Send signal to foreground process group (or slave_pid fallback).
 * IRQ-safe: only sets sig_pending + event_post, no do_kill. */
static void send_signal_to_fg(pty_t *p, int sig) {
    int pgid = p->fg_pgid;
    if (pgid <= 0) pgid = p->slave_pid;
    if (pgid <= 0) return;

    extern process_t *proc_find(uint32_t pid);
    extern void event_post(thread_t *target, uint32_t type, uint64_t data);
    for (int i = 1; i < PID_TABLE_MAX; i++) {
        process_t *proc = proc_find((uint32_t)i);
        if (!proc) continue;
        if ((int)proc->pgid != pgid) continue;
        proc->sig_pending |= (1ULL << sig);
        /* Wake blocked threads for signal delivery */
        thread_t *t = proc->threads;
        while (t) {
            if (t->state == THREAD_BLOCKED)
                event_post(t, 4 /* EQ_PIPE_DATA */, 0);
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
    if (id < 0 || id >= PTY_MAX) return -1;
    pty_t *p = &pty_pool[id];
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    for (int i = 0; i < len; i++) {
        char c = buf[i];

        /* Canonical mode signal chars — only in cooked mode */
        if (p->canon) {
            /* Ctrl+C → SIGINT */
            if (c == 3) {
                send_signal_to_fg(p, SIGINT);
                if (p->echo) pty_output_str(p, "^C\n");
                p->line_pos = 0;
                continue;
            }
            /* Ctrl+Z → SIGTSTP */
            if (c == 26) {
                send_signal_to_fg(p, SIGTSTP);
                if (p->echo) pty_output_str(p, "^Z\n");
                p->line_pos = 0;
                continue;
            }
        }
        /* Ctrl+D → EOF (flush line or signal EOF) */
        if (c == 4) {
            if (p->canon) {
                if (p->line_pos > 0)
                    line_flush(p);
                /* else: EOF marker — leave input empty so read returns 0 */
            }
            continue;
        }
        /* Ctrl+L → clear screen */
        if (c == 12) {
            pty_output_str(p, "\033[2J\033[H");
            continue;
        }
        /* Ctrl+\\ → SIGQUIT (only in canonical mode) */
        if (c == 28 && p->canon) {
            send_signal_to_fg(p, SIGQUIT);
            if (p->echo) pty_output_str(p, "^\\\n");
            p->line_pos = 0;
            continue;
        }

        if (p->canon) {
            /* Backspace */
            if (c == '\b' || c == 127) {
                if (p->line_pos > 0) {
                    p->line_pos--;
                    if (p->echo) pty_output_str(p, "\b \b");
                }
                continue;
            }
            /* Enter → flush line */
            if (c == '\n' || c == '\r') {
                if (p->echo) pty_output_char(p, '\n');
                line_flush(p);
                continue;
            }
            /* Buffer printable chars */
            if (p->line_pos < PTY_LINE_MAX - 1) {
                p->line_buf[p->line_pos++] = c;
                if (p->echo) pty_output_char(p, c);
            }
        } else {
            /* Raw mode: pass directly to input */
            if (ring_space(p->input_head, p->input_tail) > 0)
                ring_put(p->input_buf, &p->input_tail, c);
            if (p->echo) pty_output_char(p, c);
        }
    }

    /* Wake blocked reader if input data OR echo output pending */
    if (p->blocked_reader &&
        (ring_count(p->input_head, p->input_tail) > 0 ||
         ring_count(p->output_head, p->output_tail) > 0)) {
        thread_t *reader = p->blocked_reader;
        p->blocked_reader = 0;
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        event_post(reader, 4 /* EQ_PIPE_DATA */, 0);
    }

    spin_unlock_irq(&p->lock, flags);

    /* Wake poll/epoll sleepers — they check fd_poll_readiness on re-scan */
    if (ring_count(p->input_head, p->input_tail) > 0) {
        extern void epoll_wake_all(void);
        epoll_wake_all();
    }

    return len;
}

/* ── Master read: drain output buffer (process → VT) ── */

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

/* ── Slave write: process stdout → output buffer ───── */

int pty_slave_write(int id, const char *buf, int len) {
    if (id < 0 || id >= PTY_MAX) return -1;
    pty_t *p = &pty_pool[id];
    uint64_t flags;
    spin_lock_irq(&p->lock, &flags);

    int wrote = 0;
    for (int i = 0; i < len; i++) {
        if (ring_space(p->output_head, p->output_tail) <= 0) break;
        ring_put(p->output_buf, &p->output_tail, buf[i]);
        wrote++;
    }

    spin_unlock_irq(&p->lock, flags);
    return wrote;
}

/* ── Slave read: process stdin ← input buffer ──────── */

/* Write directly to input ring, bypassing canonical mode line buffer.
 * Used for terminal responses (DSR, cursor position) that must reach
 * the process regardless of canonical/raw mode state. */
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
    /* Wake blocked reader */
    if (written > 0 && p->blocked_reader) {
        thread_t *reader = p->blocked_reader;
        p->blocked_reader = 0;
        extern void event_post(thread_t *target, uint32_t type, uint64_t data);
        event_post(reader, 4, 0);
    }
    spin_unlock_irq(&p->lock, flags);
    if (written > 0) {
        extern void epoll_wake_all(void);
        epoll_wake_all();
    }
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
