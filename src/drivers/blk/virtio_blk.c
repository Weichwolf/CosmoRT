/* CosmoRT virtio-blk driver — PCI legacy interface
 *
 * Adapted from llmos. Key differences:
 * - Uses hw.h primitives (cosmo_mmio_map, cosmo_dma_alloc, cosmo_pci_config_read/write)
 * - All DMA addresses via virt_to_phys()
 * - 4KB block API (translates to 512-byte sectors internally)
 * - Higher-half kernel
 */

#include "virtio_blk.h"
#include "hw.h"
#include "config.h"
#include "serial.h"
#include "timer.h"
#include "spinlock.h"
#include "memops.h"

/* ── Port I/O (virtio legacy uses I/O ports) ──────── */

static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p)  { uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

/* ── Virtio legacy PCI register offsets (from BAR0 I/O) ── */

#define VIRTIO_DEV_FEATURES   0x00
#define VIRTIO_DRV_FEATURES   0x04
#define VIRTIO_QUEUE_ADDR     0x08
#define VIRTIO_QUEUE_SIZE     0x0C
#define VIRTIO_QUEUE_SEL      0x0E
#define VIRTIO_QUEUE_NOTIFY   0x10
#define VIRTIO_DEV_STATUS     0x12
#define VIRTIO_ISR_STATUS     0x13
#define VIRTIO_BLK_CAPACITY   0x14

/* ── Virtqueue structures ─────────────────────────── */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2  /* device writes to buffer */

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

/* Virtio-blk request header */
struct virtio_blk_req {
    uint32_t type;      /* 0=read, 1=write */
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

#define VIRTQ_MAX 256
#define SECTORS_PER_BLOCK (BLK_SIZE / 512)

/* ── Driver state ─────────────────────────────────── */

static uint16_t io_base;
static uint16_t dev_qsz;
static uint64_t capacity_sectors;
static int initialized;

static struct virtq_desc  *vq_desc;
static struct virtq_avail *vq_avail;
static struct virtq_used  *vq_used;
static uint16_t vq_last_used;

/* Per-request DMA buffers */
static struct virtio_blk_req *req_hdr;
static uint8_t *data_buf;      /* 4KB data buffer */
static uint8_t *status_byte;

static spinlock_t blk_lock = SPINLOCK_INIT;

/* ── Init ─────────────────────────────────────────── */

int virtio_blk_init(void) {
    /* PCI scan: vendor 0x1AF4, device 0x1001 (legacy) or 0x1042 (modern) */
    int found_bus = -1, found_dev = -1;

    for (int bus = 0; bus < 256 && found_bus < 0; bus++) {
        for (int dev = 0; dev < 32 && found_bus < 0; dev++) {
            uint32_t id;
            if (cosmo_pci_config_read(bus, dev, 0, 0, &id) < 0) continue;
            if (id == 0xFFFFFFFF) continue;
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;
            if (vendor == 0x1AF4 && (device == 0x1001 || device == 0x1042)) {
                /* Verify subsystem: virtio-blk = subsys device ID 2 */
                uint32_t subsys;
                cosmo_pci_config_read(bus, dev, 0, 0x2C, &subsys);
                uint16_t subsys_dev = (subsys >> 16) & 0xFFFF;
                /* Legacy: device 0x1001 with subsys 2 = block.
                 * Modern: device 0x1042 is specifically block. */
                if (device == 0x1042 || subsys_dev == 2) {
                    found_bus = bus;
                    found_dev = dev;
                }
            }
        }
    }

    if (found_bus < 0) {
        serial_puts("virtio-blk: not found\n");
        return -1;
    }
    serial_puts("virtio-blk: found on PCI ");
    { char t[4]; t[0]='0'+found_bus/10; t[1]='0'+found_bus%10; t[2]=':'; t[3]=0; serial_puts(t); }
    { char t[3]; t[0]='0'+found_dev/10; t[1]='0'+found_dev%10; t[2]=0; serial_puts(t); }
    serial_putchar('\n');

    /* Get BAR0 — virtio legacy uses I/O space */
    uint32_t bar0;
    cosmo_pci_config_read(found_bus, found_dev, 0, 0x10, &bar0);
    io_base = (uint16_t)(bar0 & 0xFFFC);

    /* Enable bus mastering + I/O space */
    uint32_t cmd;
    cosmo_pci_config_read(found_bus, found_dev, 0, 0x04, &cmd);
    cmd |= (1 << 2) | (1 << 0);  /* bus master + I/O space */
    cosmo_pci_config_write(found_bus, found_dev, 0, 0x04, cmd);

    /* Reset device */
    outb(io_base + VIRTIO_DEV_STATUS, 0);
    /* Acknowledge */
    outb(io_base + VIRTIO_DEV_STATUS, 1);
    /* Driver loaded */
    outb(io_base + VIRTIO_DEV_STATUS, 1 | 2);

    /* Negotiate features — accept all (legacy: no FEATURES_OK step) */
    uint32_t features = inl(io_base + VIRTIO_DEV_FEATURES);
    outl(io_base + VIRTIO_DRV_FEATURES, features);

    /* Select and query virtqueue 0 */
    outw(io_base + VIRTIO_QUEUE_SEL, 0);
    dev_qsz = (uint16_t)(inl(io_base + VIRTIO_QUEUE_SIZE) & 0xFFFF);
    if (dev_qsz == 0) dev_qsz = 128;
    if (dev_qsz > VIRTQ_MAX) dev_qsz = VIRTQ_MAX;

    /* Allocate DMA memory for virtqueue + request buffers.
     * Layout: [descriptors][avail ring][padding to 4KB][used ring][req_hdr][data_buf][status]
     * Conservative: 64KB covers everything. */
    void *dma_virt;
    uint64_t dma_phys;
    if (cosmo_dma_alloc(65536, &dma_virt, &dma_phys) < 0) {
        serial_puts("virtio-blk: DMA alloc failed\n");
        return -1;
    }

    /* Zero everything */
    kmemset(dma_virt, 0, 65536);

    /* Layout virtqueue (page-aligned base) */
    uint8_t *p = (uint8_t *)dma_virt;
    vq_desc  = (struct virtq_desc *)p;
    vq_avail = (struct virtq_avail *)(p + dev_qsz * sizeof(struct virtq_desc));

    /* Used ring must be page-aligned (virtio spec) */
    uint64_t avail_end = (uint64_t)vq_avail + 4 + 2 * dev_qsz;
    vq_used = (struct virtq_used *)((avail_end + 4095) & ~4095ULL);

    /* Request buffers after used ring */
    uint8_t *after_used = (uint8_t *)vq_used + 4 + 8 * dev_qsz;
    after_used = (uint8_t *)(((uint64_t)after_used + 15) & ~15ULL);
    req_hdr    = (struct virtio_blk_req *)after_used;
    data_buf   = after_used + 64;  /* 64 > sizeof(virtio_blk_req), aligned */
    /* data_buf needs 4KB for a full block read */
    status_byte = data_buf + BLK_SIZE;

    /* Tell device the queue physical address (in 4096-byte pages) */
    uint32_t qpfn = (uint32_t)(virt_to_phys(vq_desc) / 4096);
    outl(io_base + VIRTIO_QUEUE_ADDR, qpfn);

    /* Sync indices */
    vq_avail->idx = 0;
    vq_avail->flags = 0;
    vq_last_used = vq_used->idx;

    /* Driver ready */
    outb(io_base + VIRTIO_DEV_STATUS, 1 | 2 | 4);

    /* Read capacity (in 512-byte sectors) */
    capacity_sectors  = inl(io_base + VIRTIO_BLK_CAPACITY);
    capacity_sectors |= ((uint64_t)inl(io_base + VIRTIO_BLK_CAPACITY + 4)) << 32;

    initialized = 1;

    serial_puts("virtio-blk: ");
    { char t[20]; int i=0; uint64_t mb = capacity_sectors/2048;
      do{t[i++]='0'+(char)(mb%10);mb/=10;}while(mb);
      while(i--) serial_putchar(t[i]); }
    serial_puts(" MB, qsz=");
    { char t[8]; int i=0; uint16_t v=dev_qsz;
      do{t[i++]='0'+(char)(v%10);v/=10;}while(v);
      while(i--) serial_putchar(t[i]); }
    serial_putchar('\n');

    return 0;
}

/* ── Single sector I/O ────────────────────────────── */

static int do_request(uint32_t type, uint64_t sector, void *buf, uint32_t len) {
    if (!initialized) return -1;

    req_hdr->type     = type;
    req_hdr->reserved = 0;
    req_hdr->sector   = sector;
    *status_byte      = 0xFF;

    /* Copy write data to DMA buffer */
    if (type == 1)
        kmemcpy(data_buf, buf, len);

    /* 3-descriptor chain: header → data → status */
    vq_desc[0].addr  = virt_to_phys(req_hdr);
    vq_desc[0].len   = sizeof(*req_hdr);
    vq_desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq_desc[0].next  = 1;

    vq_desc[1].addr  = virt_to_phys(data_buf);
    vq_desc[1].len   = len;
    vq_desc[1].flags = (type == 0 ? VIRTQ_DESC_F_WRITE : 0) | VIRTQ_DESC_F_NEXT;
    vq_desc[1].next  = 2;

    vq_desc[2].addr  = virt_to_phys(status_byte);
    vq_desc[2].len   = 1;
    vq_desc[2].flags = VIRTQ_DESC_F_WRITE;
    vq_desc[2].next  = 0;

    /* Submit to available ring */
    uint16_t avail_idx = vq_avail->idx;
    vq_avail->ring[avail_idx % dev_qsz] = 0;  /* head descriptor */
    __asm__ volatile("sfence" ::: "memory");
    vq_avail->idx = avail_idx + 1;
    __asm__ volatile("sfence" ::: "memory");

    /* Notify device */
    outw(io_base + VIRTIO_QUEUE_NOTIFY, 0);

    /* Poll for completion with timeout */
    uint64_t deadline = timer_ms() + 2000;
    while (vq_used->idx == vq_last_used) {
        if (timer_ms() > deadline) {
            serial_puts("virtio-blk: I/O timeout\n");
            return -1;
        }
        __asm__ volatile("pause");
    }
    vq_last_used = vq_used->idx;

    /* Copy read data from DMA buffer */
    if (type == 0 && *status_byte == 0)
        kmemcpy(buf, data_buf, len);

    return (*status_byte == 0) ? 0 : -1;
}

/* ── Public API (4KB block granularity) ───────────── */

int blk_read(uint64_t block, void *buf) {
    uint64_t flags;
    spin_lock_irq(&blk_lock, &flags);

    uint64_t sector = block * SECTORS_PER_BLOCK;
    int rc = do_request(0, sector, buf, BLK_SIZE);

    spin_unlock_irq(&blk_lock, flags);
    return rc;
}

int blk_write(uint64_t block, const void *buf) {
    uint64_t flags;
    spin_lock_irq(&blk_lock, &flags);

    uint64_t sector = block * SECTORS_PER_BLOCK;
    int rc = do_request(1, sector, (void *)buf, BLK_SIZE);

    spin_unlock_irq(&blk_lock, flags);
    return rc;
}

uint64_t blk_capacity(void) {
    if (!initialized) return 0;
    return capacity_sectors / SECTORS_PER_BLOCK;
}
