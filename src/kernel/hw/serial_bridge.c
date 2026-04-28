/* Serial ↔ VT bridge — routes serial RX → PTY input, PTY output → serial TX
 *
 * Escape prefix: Ctrl-A (0x01)
 *   Ctrl-A 0-3    switch serial to VT N
 *   Ctrl-A d       detach (serial shows only kernel dmesg)
 *   Ctrl-A Ctrl-A  send literal Ctrl-A to PTY
 */

#include "hw/serial.h"
#include "vt/pty.h"
#include "vt/vt.h"
#include "core/tick.h"

/* Which VT is attached to serial. -1 = detached (dmesg only). */
static int serial_vt = 0;
static int esc_pending;  /* saw Ctrl-A, waiting for next byte */

static struct tick_callback serial_bridge_cb;

void serial_bridge_init(void) {
    serial_vt = 0;
    esc_pending = 0;
    tick_register(&serial_bridge_cb, serial_bridge_poll, TICK_EVERY);
    serial_puts("serial: bridge attached to VT0\n");
}

/* ── RX: serial input → PTY master write ─────────────── */

static void bridge_rx_byte(char c) {
    if (esc_pending) {
        esc_pending = 0;
        if (c >= '0' && c <= '3') {
            serial_vt = c - '0';
            vt_switch(serial_vt);
            serial_puts("\r\nserial: attached to VT");
            serial_putchar(c);
            serial_putchar('\n');
            return;
        }
        if (c == 'd') {
            serial_puts("\r\nserial: detached\n");
            serial_vt = -1;
            return;
        }
        if (c == 1) {
            /* Ctrl-A Ctrl-A → send literal Ctrl-A */
            if (serial_vt >= 0) {
                char a = 1;
                pty_master_write(vt_pty_id(serial_vt), &a, 1);
            }
            return;
        }
        /* Unknown escape — drop silently */
        return;
    }

    if (c == 1) { /* Ctrl-A */
        esc_pending = 1;
        return;
    }

    if (serial_vt >= 0)
        pty_master_write(vt_pty_id(serial_vt), &c, 1);
}

/* Poll COM1 for incoming bytes — called from timer tick */
void serial_bridge_poll(void) {
    for (int i = 0; i < 16; i++) {  /* drain up to 16 chars per tick */
        char c = serial_getchar();
        if (!c) break;
        bridge_rx_byte(c);
    }
}

/* ── TX: PTY output → serial ───────────────────────────
 *
 * Userspace stdout geht NICHT in den dmesg-Ring (Linux-Semantik: dmesg
 * enthaelt nur Kernel-printk, kein User-IO). Sonst ueberschreibt langer
 * Test-Output (oder ein chatty Programm) den Boot-Banner und macht
 * /proc/dmesg fuer Diagnose unbrauchbar. */

void serial_bridge_tx(int vt_id, const char *buf, int len) {
    if (vt_id != serial_vt) return;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n')
            serial_putchar_raw('\r');
        serial_putchar_raw(buf[i]);
    }
}
