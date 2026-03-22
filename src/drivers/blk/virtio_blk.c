/* CosmoRT virtio-blk driver — uses shared virtio transport
 *
 * PCI legacy interface (QEMU default).
 * 4KB block API — translates to 512-byte sectors internally.
 */

#include "virtio_blk.h"
#include "virtio.h"
#include "hw.h"
#include "config.h"
#include "serial.h"

/* Virtio-blk request header */
struct virtio_blk_req {
    uint32_t type;      /* 0=read, 1=write */
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

#define SECTORS_PER_BLOCK (BLK_SIZE / 512)

/* ── Driver state ─────────────────────────────────── */

static virtio_dev_t blk_dev;
static virtqueue_t  blk_vq;
static uint64_t capacity_sectors;
static int initialized;

/* Per-request DMA buffers */
static struct virtio_blk_req *req_hdr;
static uint8_t *data_buf;      /* 4KB data buffer */
static uint8_t *status_byte;

static hw_spinlock_t blk_lock = HW_SPINLOCK_INIT;

static void blk_irq_handler(void *ctx);

/* ── Init ─────────────────────────────────────────── */

int virtio_blk_init(void) {
    /* PCI scan: virtio-blk = device 0x1001 (legacy) or subsys 2 */
    if (virtio_pci_find(0x1001, 2, &blk_dev) < 0) {
        serial_puts("virtio-blk: not found\n");
        return -1;
    }
    serial_puts("virtio-blk: found on PCI ");
    { char t[4]; t[0]='0'+blk_dev.pci_bus/10; t[1]='0'+blk_dev.pci_bus%10; t[2]=':'; t[3]=0; serial_puts(t); }
    { char t[3]; t[0]='0'+blk_dev.pci_dev/10; t[1]='0'+blk_dev.pci_dev%10; t[2]=0; serial_puts(t); }
    serial_putchar('\n');

    /* Init device, accept all features */
    virtio_dev_init(&blk_dev, 0xFFFFFFFF);

    /* Setup virtqueue 0 (requestq) */
    if (virtqueue_setup(&blk_dev, 0, &blk_vq) < 0) {
        serial_puts("virtio-blk: VQ setup failed\n");
        return -1;
    }

    /* Allocate DMA buffers for request header + data + status */
    void *dma_virt;
    uint64_t dma_phys;
    if (cosmo_dma_alloc(8192, &dma_virt, &dma_phys) < 0) {
        serial_puts("virtio-blk: DMA alloc failed\n");
        return -1;
    }
    hw_memset(dma_virt, 0, 8192);
    uint8_t *p = (uint8_t *)dma_virt;
    req_hdr     = (struct virtio_blk_req *)p;
    data_buf    = p + 64;  /* aligned, 4KB for block data */
    status_byte = data_buf + BLK_SIZE;

    /* Device ready */
    virtio_set_driver_ok(&blk_dev);

    /* Read capacity (in 512-byte sectors) from device config */
    capacity_sectors  = virtio_cfg_read32(&blk_dev, 0);
    capacity_sectors |= ((uint64_t)virtio_cfg_read32(&blk_dev, 4)) << 32;

    initialized = 1;

    serial_puts("virtio-blk: ");
    { char t[20]; int i=0; uint64_t mb = capacity_sectors/2048;
      do{t[i++]='0'+(char)(mb%10);mb/=10;}while(mb);
      while(i--) serial_putchar(t[i]); }
    serial_puts(" MB, qsz=");
    { char t[8]; int i=0; uint16_t v=blk_vq.size;
      do{t[i++]='0'+(char)(v%10);v/=10;}while(v);
      while(i--) serial_putchar(t[i]); }
    serial_putchar('\n');

    /* Register IRQ so hlt wakes on I/O completion */
    cosmo_irq_register(blk_dev.irq_line, blk_irq_handler, 0);

    return 0;
}

/* IRQ handler — acknowledge interrupt, hlt in blk_io wakes */
static void blk_irq_handler(void *ctx) {
    (void)ctx;
    /* Read ISR status register to acknowledge (legacy: io_base + 19) */
    if (blk_dev.io_base)
        __asm__ volatile("inb %w1, %0" : "=a"((uint8_t){0}) : "Nd"((uint16_t)(blk_dev.io_base + 19)));
}

/* ── Single block I/O ─────────────────────────────── */

static int do_request(uint32_t type, uint64_t sector, void *buf, uint32_t len) {
    if (!initialized) return -1;

    req_hdr->type     = type;
    req_hdr->reserved = 0;
    req_hdr->sector   = sector;
    *status_byte      = 0xFF;

    if (type == 1)
        hw_memcpy(data_buf, buf, len);

    /* Allocate 3 descriptors for chain: header -> data -> status */
    int head = virtqueue_alloc_descs(&blk_vq, 3);
    if (head < 0) return -1;

    /* Header (device reads) */
    uint16_t d0 = (uint16_t)head;
    blk_vq.desc[d0].addr  = virt_to_phys(req_hdr);
    blk_vq.desc[d0].len   = sizeof(*req_hdr);
    blk_vq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    uint16_t d1 = blk_vq.desc[d0].next;

    /* Data (device reads for write, writes for read) */
    blk_vq.desc[d1].addr  = virt_to_phys(data_buf);
    blk_vq.desc[d1].len   = len;
    blk_vq.desc[d1].flags = (type == 0 ? VIRTQ_DESC_F_WRITE : 0) | VIRTQ_DESC_F_NEXT;
    uint16_t d2 = blk_vq.desc[d1].next;

    /* Status (device writes) */
    blk_vq.desc[d2].addr  = virt_to_phys(status_byte);
    blk_vq.desc[d2].len   = 1;
    blk_vq.desc[d2].flags = VIRTQ_DESC_F_WRITE;

    virtqueue_submit(&blk_vq, (uint16_t)head);
    virtqueue_kick(&blk_dev, 0);

    /* Wait for completion — busy-poll with pause (safe under spinlock) */
    uint64_t deadline = hw_ms() + 2000;
    while (virtqueue_get_used(&blk_vq, 0) < 0) {
        if (hw_ms() > deadline) {
            serial_puts("virtio-blk: I/O timeout\n");
            virtqueue_free_chain(&blk_vq, (uint16_t)head);
            return -1;
        }
        __asm__ volatile("pause");
    }
    virtqueue_free_chain(&blk_vq, (uint16_t)head);

    if (type == 0 && *status_byte == 0)
        hw_memcpy(buf, data_buf, len);

    return (*status_byte == 0) ? 0 : -1;
}

/* ── Public API (4KB block granularity) ───────────── */

int blk_read(uint64_t block, void *buf) {
    uint64_t flags;
    hw_spin_lock_irq(&blk_lock, &flags);
    int rc = do_request(0, block * SECTORS_PER_BLOCK, buf, BLK_SIZE);
    hw_spin_unlock_irq(&blk_lock, flags);
    return rc;
}

int blk_write(uint64_t block, const void *buf) {
    uint64_t flags;
    hw_spin_lock_irq(&blk_lock, &flags);
    int rc = do_request(1, block * SECTORS_PER_BLOCK, (void *)buf, BLK_SIZE);
    hw_spin_unlock_irq(&blk_lock, flags);
    return rc;
}

uint64_t blk_capacity(void) {
    if (!initialized) return 0;
    return capacity_sectors / SECTORS_PER_BLOCK;
}
