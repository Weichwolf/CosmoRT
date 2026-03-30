/* CosmoRT virtio-blk driver — PCI legacy interface (QEMU default) */
#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

#define BLK_SIZE 4096

int virtio_blk_init(void);

int blk_read(uint64_t block, void *buf);

int blk_write(uint64_t block, const void *buf);

uint64_t blk_capacity(void);

#endif
