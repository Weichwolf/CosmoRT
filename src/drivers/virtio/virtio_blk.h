/* CosmoRT virtio-blk driver — PCI legacy interface (QEMU default)
 * Uses hw.h primitives only: cosmo_pci_config_read/write, cosmo_mmio_map, cosmo_dma_alloc.
 * Block size: 4KB (CosmoFS native). Sector translation handled internally.
 */
#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

#define BLK_SIZE 4096  /* CosmoFS block size */

/* PCI probe + virtqueue init. Returns 0 on success, -1 if not found. */
int virtio_blk_init(void);

/* Read one 4KB block. block = block number (not sector). */
int blk_read(uint64_t block, void *buf);

/* Write one 4KB block. */
int blk_write(uint64_t block, const void *buf);

/* Total 4KB blocks on device. */
uint64_t blk_capacity(void);

#endif
