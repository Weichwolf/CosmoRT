/* COM1 serial output for debug */
#ifndef SERIAL_H
#define SERIAL_H

void serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);
char serial_getchar(void); /* non-blocking, returns 0 if no data */

#endif
