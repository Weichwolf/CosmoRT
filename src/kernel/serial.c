/* COM1 serial — 115200 baud, 8N1 */

#include "serial.h"
#include <stdint.h>

#define COM1 0x3F8

static inline void port_out8(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t port_in8(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void) {
    port_out8(COM1 + 1, 0x00);
    port_out8(COM1 + 3, 0x80);
    port_out8(COM1 + 0, 0x01);  /* 115200 baud */
    port_out8(COM1 + 1, 0x00);
    port_out8(COM1 + 3, 0x03);  /* 8N1 */
    port_out8(COM1 + 2, 0xC7);
    port_out8(COM1 + 4, 0x0B);
}

void serial_putchar(char c) {
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

char serial_getchar(void) {
    if (!(port_in8(COM1 + 5) & 0x01))
        return 0; /* no data available */
    return (char)port_in8(COM1);
}
