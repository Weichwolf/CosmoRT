/* CosmoRT Virtio PCI Transport — shared by all virtio drivers
 *
 * Legacy I/O port interface. PCI scan, virtqueue alloc, kick/poll.
 * Uses hw.h primitives only.
 */

#include "virtio.h"
#include "hw.h"
#include "config.h"
#include "serial.h"
#include "memops.h"

/* ── PCI Discovery ─────────────────────────────────── */

int virtio_pci_find(uint16_t device_id, uint16_t subsys_id, virtio_dev_t *dev) {
    for (int bus = 0; bus < 256; bus++) {
        for (int d = 0; d < 32; d++) {
            uint32_t id;
            if (cosmo_pci_config_read(bus, d, 0, 0, &id) < 0) continue;
            if (id == 0xFFFFFFFF) continue;

            uint16_t vendor = id & 0xFFFF;
            uint16_t pci_dev_id = (id >> 16) & 0xFFFF;
            if (vendor != 0x1AF4) continue;

            /* Legacy transitional: device 0x1000-0x103F, check subsys_id.
             * Modern: device 0x1040+, specific per type. */
            int match = 0;
            uint32_t subsys;
            cosmo_pci_config_read(bus, d, 0, 0x2C, &subsys);
            uint16_t actual_subsys = (subsys >> 16) & 0xFFFF;

            if (pci_dev_id == device_id) {
                /* Exact device ID match (legacy or modern) */
                if (subsys_id == 0 || actual_subsys == subsys_id)
                    match = 1;
            }
            /* Legacy transitional: device 0x1000 with subsys_id check */
            if (!match && pci_dev_id >= 0x1000 && pci_dev_id <= 0x103F &&
                actual_subsys == subsys_id)
                match = 1;

            if (!match) continue;

            /* Found — get BAR0 (I/O port) */
            uint32_t bar0;
            cosmo_pci_config_read(bus, d, 0, 0x10, &bar0);
            if (!(bar0 & 1)) continue;  /* not I/O space — skip */

            dev->io_base   = (uint16_t)(bar0 & 0xFFFC);
            dev->pci_bus   = bus;
            dev->pci_dev   = d;
            dev->device_id = pci_dev_id;
            dev->subsys_id = actual_subsys;
            dev->features  = 0;
            dev->irq_line  = -1;

            /* Read IRQ line */
            uint32_t irq_reg;
            cosmo_pci_config_read(bus, d, 0, 0x3C, &irq_reg);
            dev->irq_line = (int)(irq_reg & 0xFF);

            /* Enable bus mastering + I/O space */
            uint32_t cmd;
            cosmo_pci_config_read(bus, d, 0, 0x04, &cmd);
            cmd |= (1 << 2) | (1 << 0);
            cosmo_pci_config_write(bus, d, 0, 0x04, cmd);

            return 0;
        }
    }
    return -1;
}

/* ── Device Init (reset + feature negotiation) ─────── */

int virtio_dev_init(virtio_dev_t *dev, uint32_t wanted_features) {
    uint16_t base = dev->io_base;

    /* Reset */
    vio_outb(base + VIRTIO_DEV_STATUS, 0);
    /* Acknowledge */
    vio_outb(base + VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK);
    /* Driver */
    vio_outb(base + VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation */
    uint32_t offered = vio_inl(base + VIRTIO_DEV_FEATURES);
    dev->features = offered & wanted_features;
    vio_outl(base + VIRTIO_DRV_FEATURES, dev->features);

    return 0;
}

/* ── Virtqueue Setup ───────────────────────────────── */

int virtqueue_setup(virtio_dev_t *dev, int queue_idx, virtqueue_t *vq) {
    uint16_t base = dev->io_base;

    /* Select queue */
    vio_outw(base + VIRTIO_QUEUE_SEL, (uint16_t)queue_idx);

    /* Read queue size */
    uint16_t qsz = (uint16_t)(vio_inl(base + VIRTIO_QUEUE_SIZE) & 0xFFFF);
    if (qsz == 0) qsz = 128;
    if (qsz > VIRTQ_MAX) qsz = VIRTQ_MAX;

    /* Allocate DMA: descriptors + avail ring + padding + used ring.
     * Virtio spec: used ring must be page-aligned.
     * Conservative: 32KB covers VIRTQ_MAX=256 comfortably. */
    void *dma_virt;
    uint64_t dma_phys;
    if (cosmo_dma_alloc(32768, &dma_virt, &dma_phys) < 0) return -1;
    kmemset(dma_virt, 0, 32768);

    uint8_t *p = (uint8_t *)dma_virt;
    vq->desc  = (struct virtq_desc *)p;
    vq->avail = (struct virtq_avail *)(p + qsz * sizeof(struct virtq_desc));

    /* Used ring: page-aligned */
    uint64_t avail_end = (uint64_t)vq->avail + 4 + 2 * qsz;
    vq->used = (struct virtq_used *)((avail_end + 4095) & ~4095ULL);

    vq->desc_phys  = virt_to_phys(vq->desc);
    vq->size       = qsz;
    vq->last_used  = 0;
    vq->free_head  = 0;
    vq->num_free   = qsz;
    vq->lock       = (spinlock_t)SPINLOCK_INIT;

    /* Build free descriptor chain */
    for (uint16_t i = 0; i < qsz; i++) {
        vq->desc[i].next  = i + 1;
        vq->desc[i].flags = 0;
    }
    vq->desc[qsz - 1].next = 0xFFFF;  /* end sentinel */

    vq->avail->idx   = 0;
    vq->avail->flags = 0;

    /* Tell device the queue physical address (in 4096-byte pages) */
    vio_outl(base + VIRTIO_QUEUE_ADDR, (uint32_t)(vq->desc_phys / 4096));

    return 0;
}

/* ── Descriptor Management ─────────────────────────── */

int virtqueue_alloc_descs(virtqueue_t *vq, int n) {
    if (vq->num_free < (uint16_t)n) return -1;

    uint16_t head = vq->free_head;
    uint16_t idx = head;
    for (int i = 0; i < n; i++) {
        vq->num_free--;
        if (i == n - 1) {
            uint16_t next = vq->desc[idx].next;
            vq->desc[idx].next = 0;
            vq->desc[idx].flags = 0;
            vq->free_head = next;
        } else {
            vq->desc[idx].flags = VIRTQ_DESC_F_NEXT;
            idx = vq->desc[idx].next;
        }
    }
    return (int)head;
}

void virtqueue_free_chain(virtqueue_t *vq, uint16_t head) {
    uint16_t idx = head;
    for (;;) {
        vq->num_free++;
        if (!(vq->desc[idx].flags & VIRTQ_DESC_F_NEXT)) {
            vq->desc[idx].next = vq->free_head;
            vq->free_head = head;
            return;
        }
        idx = vq->desc[idx].next;
    }
}

/* ── Submit + Kick ─────────────────────────────────── */

void virtqueue_submit(virtqueue_t *vq, uint16_t head) {
    uint16_t avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % vq->size] = head;
    __asm__ volatile("sfence" ::: "memory");
    vq->avail->idx = avail_idx + 1;
    __asm__ volatile("sfence" ::: "memory");
}

void virtqueue_kick(virtio_dev_t *dev, int queue_idx) {
    vio_outw(dev->io_base + VIRTIO_QUEUE_NOTIFY, (uint16_t)queue_idx);
}

/* ── Poll Used Ring ────────────────────────────────── */

int virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out) {
    __asm__ volatile("lfence" ::: "memory");
    if (vq->used->idx == vq->last_used) return -1;

    uint16_t idx = vq->last_used % vq->size;
    uint32_t id  = vq->used->ring[idx].id;
    if (len_out) *len_out = vq->used->ring[idx].len;
    vq->last_used++;
    return (int)id;
}

/* ── Status ────────────────────────────────────────── */

void virtio_set_driver_ok(virtio_dev_t *dev) {
    vio_outb(dev->io_base + VIRTIO_DEV_STATUS,
             VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
}
