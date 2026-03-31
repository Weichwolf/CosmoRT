/* CosmoRT virtio-gpu driver — 2D framebuffer for QEMU/KVM
 * Minimal: create resource, attach backing, set scanout, transfer+flush.
 */
#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>

/* Initialize virtio-gpu. Returns 0 on success, -1 if not found. */
int virtio_gpu_init(void);

/* Get framebuffer pointer. Returns NULL if no GPU.
 * Caller can write pixels directly (BGRX 32-bit). */
void *virtio_gpu_framebuffer(uint32_t *width, uint32_t *height, uint32_t *pitch);

/* Flush dirty region to display. */
void virtio_gpu_flush(int x, int y, int w, int h);

#endif
