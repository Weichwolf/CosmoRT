/* 16550 UART output, gated on `console=ttyS<N>` cmdline (Linux semantics).
 *
 * Default with no cmdline: no UART traffic. The 64KB dmesg ring continues
 * to capture printk so /proc/kmsg / dmesg(1) works for postmortem.
 *
 * Port + baud are picked at boot from cmdline_init():
 *   console=ttyS0,115200  → 0x3F8, 115200
 *   console=ttyS1,9600    → 0x2F8, 9600
 * Default baud (no comma) is 115200, matching Linux 8250 driver. */

#include "hw/serial.h"
#include "core/cmdline.h"
#include "spinlock.h"
#include <stdint.h>
#include "hal/hal.h"

static inline void port_out8(uint16_t port, uint8_t val) { hal_io_outb(port, val); }
static inline uint8_t port_in8(uint16_t port) { return hal_io_inb(port); }

/* ── dmesg ring buffer ─────────────────────────────── */

#define DMESG_SIZE (64 * 1024)

static char dmesg_ring[DMESG_SIZE];
static int dmesg_head;  /* next write position (wraps) */
static int dmesg_len;   /* total bytes stored (capped at DMESG_SIZE) */
static spinlock_t dmesg_lock = SPINLOCK_INIT;

static uint16_t s_port;       /* 0 if console disabled */
static int      s_inited;

static void dmesg_push(char c) {
    uint64_t df;
    spin_lock_irq(&dmesg_lock, &df);
    dmesg_ring[dmesg_head] = c;
    dmesg_head = (dmesg_head + 1) % DMESG_SIZE;
    if (dmesg_len < DMESG_SIZE) dmesg_len++;
    spin_unlock_irq(&dmesg_lock, df);
}

/* ── Public API ────────────────────────────────────── */

void serial_init(void) {
    if (s_inited) return;
    s_inited = 1;
    if (!cmdline_console_enabled()) return;

    s_port = cmdline_console_port();
    int baud = cmdline_console_baud();
    /* 16550 divisor latch: divisor = 115200 / baud */
    int divisor = 115200 / (baud > 0 ? baud : 115200);
    if (divisor <= 0) divisor = 1;

    port_out8(s_port + 1, 0x00);
    port_out8(s_port + 3, 0x80);
    port_out8(s_port + 0, (uint8_t)(divisor & 0xff));
    port_out8(s_port + 1, (uint8_t)((divisor >> 8) & 0xff));
    port_out8(s_port + 3, 0x03);  /* 8N1 */
    port_out8(s_port + 2, 0xC7);
    port_out8(s_port + 4, 0x0B);
}

void serial_putchar(char c) {
    dmesg_push(c);
    if (!s_port) return;
    while (!(port_in8(s_port + 5) & 0x20))
        hal_cpu_relax();
    port_out8(s_port, c);
}

void serial_putchar_raw(char c) {
    if (!s_port) return;
    while (!(port_in8(s_port + 5) & 0x20))
        hal_cpu_relax();
    port_out8(s_port, c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putchar('\r');
        serial_putchar(*s++);
    }
}

void serial_hex64(uint64_t v) {
    for (int i = 60; i >= 0; i -= 4)
        serial_putchar("0123456789abcdef"[(v >> i) & 0xf]);
}

char serial_getchar(void) {
    if (!s_port) return 0;
    if (!(port_in8(s_port + 5) & 0x01))
        return 0; /* no data available */
    return (char)port_in8(s_port);
}

int serial_data_available(void) {
    if (!s_port) return 0;
    return (port_in8(s_port + 5) & 0x01) ? 1 : 0;
}

/* ── dmesg read (for procfs) ─────────────────────── */

int serial_dmesg_read(char *buf, int offset, int size) {
    if (offset >= dmesg_len || size <= 0) return 0;
    int avail = dmesg_len - offset;
    if (size > avail) size = avail;

    /* Ring start: oldest byte position */
    int start;
    if (dmesg_len < DMESG_SIZE)
        start = 0;
    else
        start = dmesg_head; /* oldest byte is at write cursor when full */

    for (int i = 0; i < size; i++) {
        int idx = (start + offset + i) % DMESG_SIZE;
        buf[i] = dmesg_ring[idx];
    }
    return size;
}

int serial_dmesg_len(void) {
    return dmesg_len;
}
