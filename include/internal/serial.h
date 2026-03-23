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

/* Raw serial TX — bypasses dmesg ring buffer. For VT bridge output. */
void serial_putchar_raw(char c);

/* Serial ↔ VT bridge: routes serial RX to PTY input, PTY output to serial TX.
 * Call serial_bridge_init() after vt_init(). */
void serial_bridge_init(void);
void serial_bridge_poll(void);           /* call from timer tick */
void serial_bridge_tx(int vt_id, const char *buf, int len); /* PTY output → serial */
int serial_data_available(void); /* 1 if RX byte ready on COM1 */

#endif
