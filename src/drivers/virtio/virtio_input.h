/* CosmoRT virtio-input driver — keyboard + mouse for QEMU/KVM */
#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include <stdint.h>

#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define EV_ABS   0x03

#define REL_X    0x00
#define REL_Y    0x01

struct input_event {
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

int virtio_input_init(void);

int virtio_input_read(struct input_event *ev);

#endif
