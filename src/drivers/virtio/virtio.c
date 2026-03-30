/* CosmoRT Virtio PCI Transport — shared by all virtio drivers */

#include "virtio.h"
#include "cosmort.h"

#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

static int virtio_parse_caps(int bus, int d, virtio_dev_t *dev) {
    uint32_t status;
    cosmo_pci_config_read(bus, d, 0, 0x04, &status);
    status >>= 16;
    if (!(status & (1 << 4))) return -1;

    uint32_t cap_reg;
    cosmo_pci_config_read(bus, d, 0, 0x34, &cap_reg);
    uint8_t cap_off = (uint8_t)(cap_reg & 0xFC);

    volatile uint8_t *common = 0, *notify = 0, *isr = 0, *devcfg = 0;
    uint32_t notify_mult = 0;

    while (cap_off) {
        uint32_t cap_hdr;
        cosmo_pci_config_read(bus, d, 0, cap_off, &cap_hdr);
        uint8_t cap_id = (uint8_t)(cap_hdr & 0xFF);
        uint8_t cap_next = (uint8_t)((cap_hdr >> 8) & 0xFC);

        if (cap_id == 0x09) {
            uint32_t cap_w1;
            cosmo_pci_config_read(bus, d, 0, cap_off + 4, &cap_w1);
            uint8_t cfg_type = (uint8_t)((cap_hdr >> 24) & 0xFF);

            uint8_t bar_idx = (uint8_t)(cap_w1 & 0xFF);

            uint32_t bar_offset, bar_length;
            cosmo_pci_config_read(bus, d, 0, cap_off + 8, &bar_offset);
            cosmo_pci_config_read(bus, d, 0, cap_off + 12, &bar_length);

            uint32_t bar_val;
            cosmo_pci_config_read(bus, d, 0, 0x10 + bar_idx * 4, &bar_val);
            uint64_t bar_phys;
            if (bar_val & 1) {
                cap_off = cap_next;
                continue;
            }
            int is_64 = ((bar_val >> 1) & 3) == 2;
            bar_phys = bar_val & 0xFFFFFFF0ULL;
            if (is_64) {
                uint32_t bar_hi;
                cosmo_pci_config_read(bus, d, 0, 0x10 + bar_idx * 4 + 4, &bar_hi);
                bar_phys |= (uint64_t)bar_hi << 32;
            }

            void *mapped;
            size_t map_size = (size_t)(bar_offset + bar_length + 4095) & ~(size_t)4095;
            if (map_size < 4096) map_size = 4096;
            hw_allow_mmio(bar_phys, map_size);
            if (cosmo_mmio_map(bar_phys, map_size, &mapped) < 0) {
                cap_off = cap_next;
                continue;
            }
            volatile uint8_t *base = (volatile uint8_t *)mapped + bar_offset;

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                common = base;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                notify = base;
                cosmo_pci_config_read(bus, d, 0, cap_off + 16, &notify_mult);
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                isr = base;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                devcfg = base;
                break;
            }
        }

        cap_off = cap_next;
    }

    if (!common || !notify || !isr) return -1;

    dev->common_cfg = common;
    dev->notify_base = notify;
    dev->isr_cfg = isr;
    dev->device_cfg = devcfg;
    dev->notify_off_multiplier = notify_mult;
    dev->modern = 1;
    return 0;
}

int virtio_pci_init_at(int bus, int d, virtio_dev_t *dev) {
    uint32_t id;
    if (cosmo_pci_config_read(bus, d, 0, 0, &id) < 0) return -1;
    if (id == 0xFFFFFFFF) return -1;

    uint16_t pci_dev_id = (id >> 16) & 0xFFFF;
    uint32_t subsys;
    cosmo_pci_config_read(bus, d, 0, 0x2C, &subsys);

    dev->pci_bus   = bus;
    dev->pci_dev   = d;
    dev->device_id = pci_dev_id;
    dev->subsys_id = (subsys >> 16) & 0xFFFF;
    dev->features  = 0;
    dev->irq_line  = -1;
    dev->common_cfg = 0;
    dev->notify_base = 0;
    dev->isr_cfg = 0;
    dev->device_cfg = 0;
    dev->notify_off_multiplier = 0;
    dev->modern = 0;
    dev->io_base = 0;

    if (virtio_parse_caps(bus, d, dev) == 0) {
    } else {
        uint32_t bar0;
        cosmo_pci_config_read(bus, d, 0, 0x10, &bar0);
        if (!(bar0 & 1)) return -1;
        dev->io_base = (uint16_t)(bar0 & 0xFFFC);
    }

    uint32_t irq_reg;
    cosmo_pci_config_read(bus, d, 0, 0x3C, &irq_reg);
    dev->irq_line = (int)(irq_reg & 0xFF);

    uint32_t cmd;
    cosmo_pci_config_read(bus, d, 0, 0x04, &cmd);
    cmd |= (1 << 2) | (1 << 1) | (1 << 0);
    cosmo_pci_config_write(bus, d, 0, 0x04, cmd);

    return 0;
}

int virtio_pci_find(uint16_t device_id, uint16_t subsys_id, virtio_dev_t *dev) {
    for (int bus = 0; bus < 256; bus++) {
        for (int d = 0; d < 32; d++) {
            uint32_t id;
            if (cosmo_pci_config_read(bus, d, 0, 0, &id) < 0) continue;
            if (id == 0xFFFFFFFF) continue;

            uint16_t vendor = id & 0xFFFF;
            uint16_t pci_dev_id = (id >> 16) & 0xFFFF;
            if (vendor != 0x1AF4) continue;

            int match = 0;
            uint32_t subsys;
            cosmo_pci_config_read(bus, d, 0, 0x2C, &subsys);
            uint16_t actual_subsys = (subsys >> 16) & 0xFFFF;

            if (pci_dev_id == device_id) {
                if (subsys_id == 0 || actual_subsys == subsys_id)
                    match = 1;
            }
            if (!match && pci_dev_id >= 0x1000 && pci_dev_id <= 0x103F &&
                actual_subsys == subsys_id)
                match = 1;

            if (!match) continue;

            dev->pci_bus   = bus;
            dev->pci_dev   = d;
            dev->device_id = pci_dev_id;
            dev->subsys_id = actual_subsys;
            dev->features  = 0;
            dev->irq_line  = -1;
            dev->common_cfg = 0;
            dev->notify_base = 0;
            dev->isr_cfg = 0;
            dev->device_cfg = 0;
            dev->notify_off_multiplier = 0;
            dev->modern = 0;
            dev->io_base = 0;

            if (pci_dev_id >= 0x1040 || virtio_parse_caps(bus, d, dev) == 0) {
                if (!dev->modern && pci_dev_id >= 0x1040) {
                    if (virtio_parse_caps(bus, d, dev) < 0)
                        continue;
                }
            }

            if (!dev->modern) {
                uint32_t bar0;
                cosmo_pci_config_read(bus, d, 0, 0x10, &bar0);
                if (!(bar0 & 1)) continue;
                dev->io_base = (uint16_t)(bar0 & 0xFFFC);
            }

            uint32_t irq_reg;
            cosmo_pci_config_read(bus, d, 0, 0x3C, &irq_reg);
            dev->irq_line = (int)(irq_reg & 0xFF);

            uint32_t cmd;
            cosmo_pci_config_read(bus, d, 0, 0x04, &cmd);
            cmd |= (1 << 2) | (1 << 1) | (1 << 0);
            cosmo_pci_config_write(bus, d, 0, 0x04, cmd);

            return 0;
        }
    }
    return -1;
}

int virtio_dev_init(virtio_dev_t *dev, uint32_t wanted_features) {
    if (dev->modern) {
        volatile uint8_t *c = dev->common_cfg;

        mmio_w8(c + VIRTIO_COMMON_STATUS, 0);
        while (mmio_r8(c + VIRTIO_COMMON_STATUS) != 0)
            ;
        mmio_w8(c + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACK);
        mmio_w8(c + VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

        mmio_w32(c + VIRTIO_COMMON_DFSELECT, 0);
        uint32_t offered = mmio_r32(c + VIRTIO_COMMON_DF);
        dev->features = offered & wanted_features;
        mmio_w32(c + VIRTIO_COMMON_GFSELECT, 0);
        mmio_w32(c + VIRTIO_COMMON_GF, dev->features);

        mmio_w8(c + VIRTIO_COMMON_STATUS,
                VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
        if (!(mmio_r8(c + VIRTIO_COMMON_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
            serial_puts("virtio: features_ok failed\n");
            return -1;
        }
        return 0;
    }

    uint16_t base = dev->io_base;

    vio_outb(base + VIRTIO_DEV_STATUS, 0);
    vio_outb(base + VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK);
    vio_outb(base + VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    uint32_t offered = vio_inl(base + VIRTIO_DEV_FEATURES);
    dev->features = offered & wanted_features;
    vio_outl(base + VIRTIO_DRV_FEATURES, dev->features);

    return 0;
}

int virtqueue_setup(virtio_dev_t *dev, int queue_idx, virtqueue_t *vq) {
    if (dev->modern) {
        volatile uint8_t *c = dev->common_cfg;

        mmio_w16(c + VIRTIO_COMMON_Q_SELECT, (uint16_t)queue_idx);

        uint16_t qsz = mmio_r16(c + VIRTIO_COMMON_Q_SIZE);
        if (qsz == 0) qsz = 128;
        if (qsz > VIRTQ_MAX) qsz = VIRTQ_MAX;

        void *dma_virt;
        uint64_t dma_phys;
        if (cosmo_dma_alloc(32768, &dma_virt, &dma_phys) < 0) return -1;
        hw_memset(dma_virt, 0, 32768);

        uint8_t *p = (uint8_t *)dma_virt;
        vq->desc  = (struct virtq_desc *)p;
        vq->avail = (struct virtq_avail *)(p + qsz * sizeof(struct virtq_desc));

        uint64_t avail_end = (uint64_t)(uintptr_t)vq->avail + 4 + 2 * qsz;
        vq->used = (struct virtq_used *)((avail_end + 4095) & ~4095ULL);

        uint64_t desc_phys  = dma_phys;
        uint64_t avail_phys = dma_phys + (uint64_t)((uint8_t *)vq->avail - p);
        uint64_t used_phys  = dma_phys + (uint64_t)((uint8_t *)vq->used - p);

        vq->desc_phys  = desc_phys;
        vq->size       = qsz;
        vq->last_used  = 0;
        vq->free_head  = 0;
        vq->num_free   = qsz;
        vq->lock       = (hw_spinlock_t)HW_SPINLOCK_INIT;

        for (uint16_t i = 0; i < qsz; i++) {
            vq->desc[i].next  = i + 1;
            vq->desc[i].flags = 0;
        }
        vq->desc[qsz - 1].next = 0xFFFF;

        vq->avail->idx   = 0;
        vq->avail->flags = 0;

        mmio_w32(c + VIRTIO_COMMON_Q_DESC_LO,  (uint32_t)(desc_phys & 0xFFFFFFFF));
        mmio_w32(c + VIRTIO_COMMON_Q_DESC_HI,  (uint32_t)(desc_phys >> 32));
        mmio_w32(c + VIRTIO_COMMON_Q_AVAIL_LO, (uint32_t)(avail_phys & 0xFFFFFFFF));
        mmio_w32(c + VIRTIO_COMMON_Q_AVAIL_HI, (uint32_t)(avail_phys >> 32));
        mmio_w32(c + VIRTIO_COMMON_Q_USED_LO,  (uint32_t)(used_phys & 0xFFFFFFFF));
        mmio_w32(c + VIRTIO_COMMON_Q_USED_HI,  (uint32_t)(used_phys >> 32));

        mmio_w16(c + VIRTIO_COMMON_Q_ENABLE, 1);

        return 0;
    }

    uint16_t base = dev->io_base;

    vio_outw(base + VIRTIO_QUEUE_SEL, (uint16_t)queue_idx);

    uint16_t qsz = (uint16_t)(vio_inl(base + VIRTIO_QUEUE_SIZE) & 0xFFFF);
    if (qsz == 0) qsz = 128;
    if (qsz > VIRTQ_MAX) qsz = VIRTQ_MAX;

    void *dma_virt;
    uint64_t dma_phys;
    if (cosmo_dma_alloc(32768, &dma_virt, &dma_phys) < 0) return -1;
    hw_memset(dma_virt, 0, 32768);

    uint8_t *p = (uint8_t *)dma_virt;
    vq->desc  = (struct virtq_desc *)p;
    vq->avail = (struct virtq_avail *)(p + qsz * sizeof(struct virtq_desc));

    uint64_t avail_end = (uint64_t)(uintptr_t)vq->avail + 4 + 2 * qsz;
    vq->used = (struct virtq_used *)((avail_end + 4095) & ~4095ULL);

    vq->desc_phys  = dma_phys;
    vq->size       = qsz;
    vq->last_used  = 0;
    vq->free_head  = 0;
    vq->num_free   = qsz;
    vq->lock       = (hw_spinlock_t)HW_SPINLOCK_INIT;

    for (uint16_t i = 0; i < qsz; i++) {
        vq->desc[i].next  = i + 1;
        vq->desc[i].flags = 0;
    }
    vq->desc[qsz - 1].next = 0xFFFF;

    vq->avail->idx   = 0;
    vq->avail->flags = 0;

    vio_outl(base + VIRTIO_QUEUE_ADDR, (uint32_t)(vq->desc_phys / 4096));

    return 0;
}

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

void virtqueue_submit(virtqueue_t *vq, uint16_t head) {
    uint16_t avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % vq->size] = head;
    __asm__ volatile("sfence" ::: "memory");
    vq->avail->idx = avail_idx + 1;
    __asm__ volatile("sfence" ::: "memory");
}

void virtqueue_kick(virtio_dev_t *dev, int queue_idx) {
    if (dev->modern) {
        mmio_w16(dev->common_cfg + VIRTIO_COMMON_Q_SELECT, (uint16_t)queue_idx);
        uint16_t notify_off = mmio_r16(dev->common_cfg + VIRTIO_COMMON_Q_NOTIFY_OFF);
        volatile uint8_t *notify_addr = dev->notify_base +
            (uint32_t)notify_off * dev->notify_off_multiplier;
        mmio_w16(notify_addr, (uint16_t)queue_idx);
        return;
    }
    vio_outw(dev->io_base + VIRTIO_QUEUE_NOTIFY, (uint16_t)queue_idx);
}

int virtqueue_get_used(virtqueue_t *vq, uint32_t *len_out) {
    __asm__ volatile("lfence" ::: "memory");
    if (vq->used->idx == vq->last_used) return -1;

    uint16_t idx = vq->last_used % vq->size;
    uint32_t id  = vq->used->ring[idx].id;
    if (len_out) *len_out = vq->used->ring[idx].len;
    vq->last_used++;
    return (int)id;
}

void virtio_set_driver_ok(virtio_dev_t *dev) {
    if (dev->modern) {
        uint8_t s = mmio_r8(dev->common_cfg + VIRTIO_COMMON_STATUS);
        mmio_w8(dev->common_cfg + VIRTIO_COMMON_STATUS, s | VIRTIO_STATUS_DRIVER_OK);
        return;
    }
    vio_outb(dev->io_base + VIRTIO_DEV_STATUS,
             VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
}
