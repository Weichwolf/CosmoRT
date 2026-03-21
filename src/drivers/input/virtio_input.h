/* CosmoRT virtio-input driver — keyboard + mouse for QEMU/KVM */
#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include <stdint.h>

/* Linux input_event types */
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define EV_ABS   0x03

/* Common relative axes */
#define REL_X    0x00
#define REL_Y    0x01

struct input_event {
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

/* Initialize virtio-input. Probes all virtio-input devices on PCI.
 * Returns 0 on success, -1 if none found. */
int virtio_input_init(void);

/* Non-blocking read. Returns 1 if event available, 0 if none. */
int virtio_input_read(struct input_event *ev);

/* Set callback for input events (called from IRQ context). */
void virtio_input_set_callback(void (*cb)(uint16_t type, uint16_t code, int32_t value));

#endif
