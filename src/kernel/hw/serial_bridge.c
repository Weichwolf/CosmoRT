/* Serial-VT bridge — routes serial RX to PTY input, PTY output to serial TX */

#include "hw/serial.h"
#include "vt/pty.h"
#include "vt/vt.h"
#include "core/irq_thread.h"

#define SERIAL_BRIDGE_MAX_RX 16
#define SERIAL_THREAD_PRIO   80

static int serial_vt = 0;
static int esc_pending;

static irq_thread_t serial_irq_thread;

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
            if (serial_vt >= 0) {
                char a = 1;
                pty_master_write(vt_pty_id(serial_vt), &a, 1);
            }
            return;
        }
        return;
    }

    if (c == 1) {
        esc_pending = 1;
        return;
    }

    if (serial_vt >= 0)
        pty_master_write(vt_pty_id(serial_vt), &c, 1);
}

static void serial_thread_handler(void) {
    for (int i = 0; i < SERIAL_BRIDGE_MAX_RX; i++) {
        char c = serial_getchar();
        if (!c) break;
        bridge_rx_byte(c);
    }
}

void serial_bridge_init(void) {
    serial_vt = 0;
    esc_pending = 0;
    irq_thread_create(&serial_irq_thread, "serial", serial_thread_handler, SERIAL_THREAD_PRIO);
    serial_puts("serial: bridge attached to VT0\n");
}

void serial_bridge_poll(void) {
    if (serial_data_available() && serial_irq_thread.thread)
        irq_thread_wake(&serial_irq_thread);
}

void serial_bridge_tx(int vt_id, const char *buf, int len) {
    if (vt_id != serial_vt) return;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n')
            serial_putchar('\r');
        serial_putchar(buf[i]);
    }
}
