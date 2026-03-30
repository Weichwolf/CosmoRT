/* CosmoRT Input Subsystem — driver-agnostic event routing via IRQ thread */

#include "vt/input.h"
#include "core/irq_thread.h"
#include "hw/serial.h"
#include "spinlock.h"

#define EV_KEY 0x01

#define INPUT_MAX_DRIVERS  4
#define INPUT_RING_SIZE    64
#define INPUT_THREAD_PRIO  85

static const input_driver_t *drivers[INPUT_MAX_DRIVERS];
static int num_drivers;

static void (*vt_cb)(uint16_t scancode, int pressed);

static input_event_t input_ring[INPUT_RING_SIZE];
static volatile int ring_head, ring_count;
static spinlock_t ring_lock = SPINLOCK_INIT;

static irq_thread_t input_irq_thread;

static void input_thread_handler(void) {
    for (;;) {
        input_event_t ev;
        uint64_t flags;
        spin_lock_irq(&ring_lock, &flags);
        if (ring_count == 0) {
            spin_unlock_irq(&ring_lock, flags);
            break;
        }
        ev = input_ring[ring_head];
        ring_head = (ring_head + 1) % INPUT_RING_SIZE;
        ring_count--;
        spin_unlock_irq(&ring_lock, flags);

        if (ev.type == EV_KEY && vt_cb)
            vt_cb(ev.code, ev.value != 0);
    }
}

void input_init(void) {
    extern void vt_keyboard_event(uint16_t scancode, int pressed);
    vt_cb = vt_keyboard_event;
    irq_thread_create(&input_irq_thread, "input", input_thread_handler, INPUT_THREAD_PRIO);
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
    uint64_t flags;
    spin_lock_irq(&ring_lock, &flags);
    if (ring_count < INPUT_RING_SIZE) {
        int idx = (ring_head + ring_count) % INPUT_RING_SIZE;
        input_ring[idx] = *ev;
        ring_count++;
    }
    spin_unlock_irq(&ring_lock, flags);

    if (input_irq_thread.thread)
        irq_thread_wake(&input_irq_thread);
}
