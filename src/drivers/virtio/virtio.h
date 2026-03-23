/* CosmoRT Virtio PCI Transport — shared by all virtio drivers
 *
 * Handles PCI discovery, virtqueue setup, feature negotiation, kick/poll.
 * Supports legacy I/O port interface AND modern MMIO (PCI capabilities).
 */
#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include "cosmo_rt.h"

/* ── Virtio PCI legacy register offsets (from BAR0 I/O) ── */

#define VIRTIO_DEV_FEATURES   0x00
#define VIRTIO_DRV_FEATURES   0x04
#define VIRTIO_QUEUE_ADDR     0x08
#define VIRTIO_QUEUE_SIZE     0x0C
#define VIRTIO_QUEUE_SEL      0x0E
#define VIRTIO_QUEUE_NOTIFY   0x10
#define VIRTIO_DEV_STATUS     0x12
#define VIRTIO_ISR_STATUS     0x13
#define VIRTIO_DEV_CONFIG     0x14  /* device-specific config starts here */

/* Status bits */
#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FAILED       128

/* ── Virtqueue descriptor ──────────────────────────── */

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2  /* device writes to buffer */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

/* ── Virtqueue ─────────────────────────────────────── */

#define VIRTQ_MAX 256

typedef struct {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint64_t desc_phys;       /* physical base (for QUEUE_ADDR) */
    uint16_t size;            /* queue entry count */
    uint16_t last_used;       /* consumer index into used ring */
    uint16_t free_head;       /* head of free descriptor chain */
    uint16_t num_free;        /* free descriptors */
    hw_spinlock_t lock;
} virtqueue_t;

/* ── Virtio device ─────────────────────────────────── */

typedef struct {
    uint16_t io_base;         /* BAR0 I/O port base (legacy) */
    int pci_bus, pci_dev;
    uint16_t device_id;       /* PCI device ID */
    uint16_t subsys_id;       /* subsystem device ID */
    uint32_t features;        /* negotiated feature bits */
    int irq_line;

    /* Modern MMIO pointers (NULL if legacy) */
    volatile uint8_t *common_cfg;  /* VIRTIO_PCI_CAP_COMMON_CFG */
    volatile uint8_t *notify_base; /* VIRTIO_PCI_CAP_NOTIFY_CFG */
    volatile uint8_t *isr_cfg;     /* VIRTIO_PCI_CAP_ISR_CFG */
    volatile uint8_t *device_cfg;  /* VIRTIO_PCI_CAP_DEVICE_CFG */
    uint32_t notify_off_multiplier;
    int modern;               /* 1 = modern MMIO, 0 = legacy I/O */
} virtio_dev_t;

/* ── Port I/O helpers ──────────────────────────────── */

static inline void vio_outb(uint16_t p, uint8_t v)   { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void vio_outw(uint16_t p, uint16_t v)  { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void vio_outl(uint16_t p, uint32_t v)  { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  vio_inb(uint16_t p) { uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t vio_inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t vio_inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

/* ── MMIO helpers ──────────────────────────────────── */

static inline void     mmio_w8(volatile uint8_t *p, uint8_t v)   { *p = v; __asm__ volatile("sfence":::"memory"); }
static inline void     mmio_w16(volatile uint8_t *p, uint16_t v) { *(volatile uint16_t *)p = v; __asm__ volatile("sfence":::"memory"); }
static inline void     mmio_w32(volatile uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; __asm__ volatile("sfence":::"memory"); }
static inline void     mmio_w64(volatile uint8_t *p, uint64_t v) { *(volatile uint64_t *)p = v; __asm__ volatile("sfence":::"memory"); }
static inline uint8_t  mmio_r8(volatile uint8_t *p)  { __asm__ volatile("lfence":::"memory"); return *p; }
static inline uint16_t mmio_r16(volatile uint8_t *p) { __asm__ volatile("lfence":::"memory"); return *(volatile uint16_t *)p; }
static inline uint32_t mmio_r32(volatile uint8_t *p) { __asm__ volatile("lfence":::"memory"); return *(volatile uint32_t *)p; }
static inline uint64_t mmio_r64(volatile uint8_t *p) { __asm__ volatile("lfence":::"memory"); return *(volatile uint64_t *)p; }

/* ── Modern virtio common_cfg register offsets ─────── */

#define VIRTIO_COMMON_DFSELECT     0x00  /* uint32 */
#define VIRTIO_COMMON_DF           0x04  /* uint32 */
#define VIRTIO_COMMON_GFSELECT     0x08  /* uint32 */
#define VIRTIO_COMMON_GF           0x0C  /* uint32 */
#define VIRTIO_COMMON_MSIX_CFG     0x10  /* uint16 */
#define VIRTIO_COMMON_NUMQ         0x12  /* uint16 */
#define VIRTIO_COMMON_STATUS       0x14  /* uint8  */
#define VIRTIO_COMMON_CFGGEN       0x15  /* uint8  */
#define VIRTIO_COMMON_Q_SELECT     0x16  /* uint16 */
#define VIRTIO_COMMON_Q_SIZE       0x18  /* uint16 */
#define VIRTIO_COMMON_Q_MSIX_VEC   0x1A  /* uint16 */
#define VIRTIO_COMMON_Q_ENABLE     0x1C  /* uint16 */
#define VIRTIO_COMMON_Q_NOTIFY_OFF 0x1E  /* uint16 */
#define VIRTIO_COMMON_Q_DESC_LO    0x20  /* uint32 */
#define VIRTIO_COMMON_Q_DESC_HI    0x24  /* uint32 */
#define VIRTIO_COMMON_Q_AVAIL_LO   0x28  /* uint32 */
#define VIRTIO_COMMON_Q_AVAIL_HI   0x2C  /* uint32 */
#define VIRTIO_COMMON_Q_USED_LO    0x30  /* uint32 */
#define VIRTIO_COMMON_Q_USED_HI    0x34  /* uint32 */

/* ── API ───────────────────────────────────────────── */

/* Find virtio device by PCI device ID (e.g., 0x1000 for net, 0x1001 for blk).
 * Populates dev struct. Returns 0 on success, -1 if not found.
 * For legacy transitional devices, also checks subsys_id.
 * For modern devices (0x1040+), matches directly and uses MMIO. */
int virtio_pci_find(uint16_t device_id, uint16_t subsys_id, virtio_dev_t *dev);

/* Initialize a virtio device at a known PCI bus:dev.
 * Probes for modern (PCI capabilities) or legacy (I/O BAR).
 * Returns 0 on success, -1 on failure. */
int virtio_pci_init_at(int bus, int slot, virtio_dev_t *dev);

/* Reset device, set ACK+DRIVER status, negotiate features.
 * Call after virtio_pci_find. Returns 0 on success. */
int virtio_dev_init(virtio_dev_t *dev, uint32_t wanted_features);

/* Setup virtqueue: allocate DMA memory, tell device.
 * queue_idx: 0, 1, ... Returns 0 on success. */
int virtqueue_setup(virtio_dev_t *dev, int queue_idx, virtqueue_t *vq);

/* Allocate N contiguous descriptors from free list. Returns head index, -1 if full. */
int virtqueue_alloc_descs(virtqueue_t *vq, int n);

/* Free descriptor chain starting at head back to free list. */
void virtqueue_free_chain(virtqueue_t *vq, uint16_t head);

/* Submit descriptor chain (head index) to available ring. */
void virtqueue_submit(virtqueue_t *vq, uint16_t head);

/* Notify device about queue_idx. */
void virtqueue_kick(virtio_dev_t *dev, int queue_idx);

/* Check used ring for completed buffer. Returns head desc index, -1 if none.
 * *len_out receives bytes written by device. */
int virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out);

/* Set DRIVER_OK status — device is live. */
void virtio_set_driver_ok(virtio_dev_t *dev);

/* Read device-specific config byte/word/dword at offset. */
static inline uint8_t virtio_cfg_read8(virtio_dev_t *dev, int off) {
    if (dev->modern) return mmio_r8(dev->device_cfg + off);
    return vio_inb(dev->io_base + VIRTIO_DEV_CONFIG + off);
}
static inline uint16_t virtio_cfg_read16(virtio_dev_t *dev, int off) {
    if (dev->modern) return mmio_r16(dev->device_cfg + off);
    return vio_inw(dev->io_base + VIRTIO_DEV_CONFIG + off);
}
static inline uint32_t virtio_cfg_read32(virtio_dev_t *dev, int off) {
    if (dev->modern) return mmio_r32(dev->device_cfg + off);
    return vio_inl(dev->io_base + VIRTIO_DEV_CONFIG + off);
}

/* Read ISR status (auto-clears). */
static inline uint8_t virtio_isr_read(virtio_dev_t *dev) {
    if (dev->modern) return mmio_r8(dev->isr_cfg);
    return vio_inb(dev->io_base + VIRTIO_ISR_STATUS);
}

#endif
