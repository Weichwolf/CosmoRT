/* COM1 serial output for debug + dmesg ring buffer */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);
void serial_hex64(uint64_t v);
char serial_getchar(void); /* non-blocking, returns 0 if no data */

/* dmesg ring buffer: read `size` bytes starting at `offset` into `buf`.
 * Returns bytes copied. Used by procfs. */
int serial_dmesg_read(char *buf, int offset, int size);
int serial_dmesg_len(void);

#endif
