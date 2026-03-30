/* COM1 serial — 115200 baud, 8N1, ring buffer for dmesg */

#include "hw/serial.h"
#include "spinlock.h"
#include <stdint.h>
#include "arch/arch.h"

#define COM1 0x3F8

static inline void port_out8(uint16_t port, uint8_t val) { arch_outb(port, val); }
static inline uint8_t port_in8(uint16_t port) { return arch_inb(port); }

#define DMESG_SIZE (64 * 1024)

static char dmesg_ring[DMESG_SIZE];
static int dmesg_head;
static int dmesg_len;
static spinlock_t dmesg_lock = SPINLOCK_INIT;

void serial_init(void) {
    port_out8(COM1 + 1, 0x00);
    port_out8(COM1 + 3, 0x80);
    port_out8(COM1 + 0, 0x01);
    port_out8(COM1 + 1, 0x00);
    port_out8(COM1 + 3, 0x03);
    port_out8(COM1 + 2, 0xC7);
    port_out8(COM1 + 4, 0x0B);
}

void serial_putchar(char c) {
    while (!(port_in8(COM1 + 5) & 0x20))
        ;
    port_out8(COM1, c);

    uint64_t df;
    spin_lock_irq(&dmesg_lock, &df);
    dmesg_ring[dmesg_head] = c;
    dmesg_head = (dmesg_head + 1) % DMESG_SIZE;
    if (dmesg_len < DMESG_SIZE) dmesg_len++;
    spin_unlock_irq(&dmesg_lock, df);
}

void serial_putchar_raw(char c) {
    while (!(port_in8(COM1 + 5) & 0x20))
        ;
    port_out8(COM1, c);
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
    if (!(port_in8(COM1 + 5) & 0x01))
        return 0;
    return (char)port_in8(COM1);
}

int serial_data_available(void) {
    return (port_in8(COM1 + 5) & 0x01) ? 1 : 0;
}

int serial_dmesg_read(char *buf, int offset, int size) {
    if (offset >= dmesg_len || size <= 0) return 0;
    int avail = dmesg_len - offset;
    if (size > avail) size = avail;

    int start;
    if (dmesg_len < DMESG_SIZE)
        start = 0;
    else
        start = dmesg_head;

    for (int i = 0; i < size; i++) {
        int idx = (start + offset + i) % DMESG_SIZE;
        buf[i] = dmesg_ring[idx];
    }
    return size;
}

int serial_dmesg_len(void) {
    return dmesg_len;
}
