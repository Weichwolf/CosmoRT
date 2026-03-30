/* CosmoRT Virtio PCI Transport — shared by all virtio drivers */
#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include "cosmort.h"

#define VIRTIO_DEV_FEATURES   0x00
#define VIRTIO_DRV_FEATURES   0x04
#define VIRTIO_QUEUE_ADDR     0x08
#define VIRTIO_QUEUE_SIZE     0x0C
#define VIRTIO_QUEUE_SEL      0x0E
#define VIRTIO_QUEUE_NOTIFY   0x10
#define VIRTIO_DEV_STATUS     0x12
#define VIRTIO_ISR_STATUS     0x13
#define VIRTIO_DEV_CONFIG     0x14

#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4
#define VIRTIO_STATUS_FAILED       128

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

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

#define VIRTQ_MAX 256

typedef struct {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint64_t desc_phys;
    uint16_t size;
    uint16_t last_used;
    uint16_t free_head;
    uint16_t num_free;
    hw_spinlock_t lock;
} virtqueue_t;

typedef struct {
    uint16_t io_base;
    int pci_bus, pci_dev;
    uint16_t device_id;
    uint16_t subsys_id;
    uint32_t features;
    int irq_line;

    volatile uint8_t *common_cfg;
    volatile uint8_t *notify_base;
    volatile uint8_t *isr_cfg;
    volatile uint8_t *device_cfg;
    uint32_t notify_off_multiplier;
    int modern;
} virtio_dev_t;

static inline void vio_outb(uint16_t p, uint8_t v)   { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void vio_outw(uint16_t p, uint16_t v)  { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void vio_outl(uint16_t p, uint32_t v)  { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  vio_inb(uint16_t p) { uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t vio_inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t vio_inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

static inline void     mmio_w8(volatile uint8_t *p, uint8_t v)   { *p = v; __asm__ volatile("sfence":::"memory"); }
static inline void     mmio_w16(volatile uint8_t *p, uint16_t v) { *(volatile uint16_t *)p = v; __asm__ volatile("sfence":::"memory"); }
static inline void     mmio_w32(volatile uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; __asm__ volatile("sfence":::"memory"); }
static inline void     mmio_w64(volatile uint8_t *p, uint64_t v) { *(volatile uint64_t *)p = v; __asm__ volatile("sfence":::"memory"); }
static inline uint8_t  mmio_r8(volatile uint8_t *p)  { __asm__ volatile("lfence":::"memory"); return *p; }
static inline uint16_t mmio_r16(volatile uint8_t *p) { __asm__ volatile("lfence":::"memory"); return *(volatile uint16_t *)p; }
static inline uint32_t mmio_r32(volatile uint8_t *p) { __asm__ volatile("lfence":::"memory"); return *(volatile uint32_t *)p; }
static inline uint64_t mmio_r64(volatile uint8_t *p) { __asm__ volatile("lfence":::"memory"); return *(volatile uint64_t *)p; }

#define VIRTIO_COMMON_DFSELECT     0x00
#define VIRTIO_COMMON_DF           0x04
#define VIRTIO_COMMON_GFSELECT     0x08
#define VIRTIO_COMMON_GF           0x0C
#define VIRTIO_COMMON_MSIX_CFG     0x10
#define VIRTIO_COMMON_NUMQ         0x12
#define VIRTIO_COMMON_STATUS       0x14
#define VIRTIO_COMMON_CFGGEN       0x15
#define VIRTIO_COMMON_Q_SELECT     0x16
#define VIRTIO_COMMON_Q_SIZE       0x18
#define VIRTIO_COMMON_Q_MSIX_VEC   0x1A
#define VIRTIO_COMMON_Q_ENABLE     0x1C
#define VIRTIO_COMMON_Q_NOTIFY_OFF 0x1E
#define VIRTIO_COMMON_Q_DESC_LO    0x20
#define VIRTIO_COMMON_Q_DESC_HI    0x24
#define VIRTIO_COMMON_Q_AVAIL_LO   0x28
#define VIRTIO_COMMON_Q_AVAIL_HI   0x2C
#define VIRTIO_COMMON_Q_USED_LO    0x30
#define VIRTIO_COMMON_Q_USED_HI    0x34

int virtio_pci_find(uint16_t device_id, uint16_t subsys_id, virtio_dev_t *dev);

int virtio_pci_init_at(int bus, int slot, virtio_dev_t *dev);

int virtio_dev_init(virtio_dev_t *dev, uint32_t wanted_features);

int virtqueue_setup(virtio_dev_t *dev, int queue_idx, virtqueue_t *vq);

int virtqueue_alloc_descs(virtqueue_t *vq, int n);

void virtqueue_free_chain(virtqueue_t *vq, uint16_t head);

void virtqueue_submit(virtqueue_t *vq, uint16_t head);

void virtqueue_kick(virtio_dev_t *dev, int queue_idx);

int virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out);

void virtio_set_driver_ok(virtio_dev_t *dev);

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

static inline uint8_t virtio_isr_read(virtio_dev_t *dev) {
    if (dev->modern) return mmio_r8(dev->isr_cfg);
    return vio_inb(dev->io_base + VIRTIO_ISR_STATUS);
}

#endif
