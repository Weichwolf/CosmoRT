/* Kernel cmdline parser.
 *
 * Source: QEMU fw_cfg key "opt/cmdline" (set via -fw_cfg name=opt/cmdline,...).
 * Format follows the Linux kernel cmdline convention — space-separated tokens
 * each of which is `key` or `key=value`. Currently parsed:
 *
 *   console=ttyS0[,baud][,...]   enable serial console on UART0 (port 0x3F8)
 *   console=ttyS1[,baud][,...]   enable serial console on UART1 (port 0x2F8)
 *
 * Default with no cmdline (or no console= token): no serial output. The
 * dmesg ring buffer continues to record kernel printk regardless. */
#ifndef CMDLINE_H
#define CMDLINE_H

#include <stdint.h>

/* Probe fw_cfg, copy "opt/cmdline" into the kernel buffer, parse known keys.
 * Idempotent — safe to call before any subsystem that depends on results. */
void cmdline_init(void);

/* 1 if a console=ttyS<N>... token was found; serial_init/serial_putchar
 * gate UART access on this. */
int  cmdline_console_enabled(void);

/* I/O port (0x3F8 / 0x2F8) selected by console=ttyS<N>. Undefined if !enabled. */
uint16_t cmdline_console_port(void);

/* Baud rate parsed from `console=ttyS0,<baud>`, default 115200. */
int  cmdline_console_baud(void);

/* Raw cmdline (NUL-terminated) for /proc/cmdline. Empty string if unset. */
const char *cmdline_raw(void);

#endif
