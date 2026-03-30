/* COM1 serial output for debug + dmesg ring buffer */
#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);
void serial_hex64(uint64_t v);
char serial_getchar(void);

int serial_dmesg_read(char *buf, int offset, int size);
int serial_dmesg_len(void);

void serial_putchar_raw(char c);

void serial_bridge_init(void);
void serial_bridge_poll(void);
void serial_bridge_tx(int vt_id, const char *buf, int len);
int serial_data_available(void);

#endif
