/* CosmoRT Input Subsystem — driver-agnostic event routing */

#include "vt/input.h"
#include "hw/serial.h"

#define EV_KEY 0x01

#define INPUT_MAX_DRIVERS 4

static const input_driver_t *drivers[INPUT_MAX_DRIVERS];
static int num_drivers;

static void (*vt_cb)(uint16_t scancode, int pressed);

void input_init(void) {
    extern void vt_keyboard_event(uint16_t scancode, int pressed);
    vt_cb = vt_keyboard_event;
}

void input_register(const input_driver_t *drv) {
    if (num_drivers >= INPUT_MAX_DRIVERS) return;
    drivers[num_drivers++] = drv;
    serial_puts("input: registered ");
    serial_puts(drv->name);
    serial_putchar('\n');
}

int input_has_keyboard(void) {
    return num_drivers > 0;
}

void input_submit_event(const input_event_t *ev) {
    if (ev->type == EV_KEY && vt_cb)
        vt_cb(ev->code, ev->value != 0);
}
