/* CosmoRT virtio-gpu driver — 2D framebuffer for QEMU/KVM */
#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

int virtio_gpu_init(void);

void *virtio_gpu_framebuffer(uint32_t *width, uint32_t *height, uint32_t *pitch);

void virtio_gpu_flush(int x, int y, int w, int h);

#endif
