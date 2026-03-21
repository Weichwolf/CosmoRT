/* CosmoRT PTY — pseudo-terminal pairs with line discipline */

#include "pty.h"
#include "memops.h"
#include "serial.h"
#include "process.h"
#include "thread.h"

extern void sched_add(thread_t *t);

/* Signal constants */
#define SIGINT  2
#define SIGQUIT 3

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

static void send_signal(pty_t *p, int sig) {
    if (p->slave_pid > 0) {
        process_t *target = proc_find((uint32_t)p->slave_pid);
        if (target)
            target->sig_pending |= (1ULL << sig);
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

        /* Ctrl+C → SIGINT */
        if (c == 3) {
            send_signal(p, SIGINT);
            if (p->echo) pty_output_str(p, "^C\n");
            p->line_pos = 0;
            continue;
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
        /* Ctrl+\\ → SIGQUIT */
        if (c == 28) {
            send_signal(p, SIGQUIT);
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

    /* Wake blocked reader if data available in input buffer */
    if (p->blocked_reader && ring_count(p->input_head, p->input_tail) > 0) {
        thread_t *reader = p->blocked_reader;
        p->blocked_reader = 0;
        sched_add(reader);
    }

    spin_unlock_irq(&p->lock, flags);
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
