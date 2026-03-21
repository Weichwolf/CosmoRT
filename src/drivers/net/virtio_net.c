/* CosmoRT virtio-net driver — IRQ-driven NIC for QEMU/KVM
 *
 * virtio-net PCI: vendor 0x1AF4, device 0x1000 (legacy), subsys 1.
 * VQ 0: RX, VQ 1: TX. Registers with net stack via net_nic_register().
 *
 * Uses shared virtio transport (virtio.h).
 */

#include "virtio_net.h"
#include "virtio.h"
#include "hw.h"
#include "config.h"
#include "serial.h"
#include "net.h"
#include "memops.h"
#include "spinlock.h"

/* ── Virtio-net header (prepended to every packet) ── */

struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

#define VIRTIO_NET_HDR_SIZE  sizeof(struct virtio_net_hdr)

/* Feature bits */
#define VIRTIO_NET_F_MAC        (1 << 5)
#define VIRTIO_NET_F_STATUS     (1 << 16)
#define VIRTIO_NET_F_MRG_RXBUF (1 << 15)

/* ── Configuration ─────────────────────────────────── */

#define NUM_RX_BUFS 16
#define NUM_TX_BUFS 8
#define BUF_SIZE    2048  /* MTU + headers + virtio-net header */

/* ── Driver state ──────────────────────────────────── */

static virtio_dev_t net_dev;
static virtqueue_t  rxq, txq;
static uint8_t mac_addr[6];
static int initialized;

/* DMA buffers — one per RX/TX descriptor */
static uint8_t (*rx_bufs)[BUF_SIZE];
static uint8_t (*tx_bufs)[BUF_SIZE];
static uint64_t rx_bufs_phys;
static uint64_t tx_bufs_phys;

/* Track which descriptor is used per TX slot */
static spinlock_t tx_lock = SPINLOCK_INIT;

/* ── RX buffer management ──────────────────────────── */

static void rx_refill(void) {
    while (rxq.num_free >= 1) {
        int head = virtqueue_alloc_descs(&rxq, 1);
        if (head < 0) break;

        /* Point descriptor at RX buffer (device writes entire frame) */
        rxq.desc[head].addr  = rx_bufs_phys + (uint64_t)head * BUF_SIZE;
        rxq.desc[head].len   = BUF_SIZE;
        rxq.desc[head].flags = VIRTQ_DESC_F_WRITE;
        rxq.desc[head].next  = 0;

        virtqueue_submit(&rxq, (uint16_t)head);
    }
    virtqueue_kick(&net_dev, 0);
}

/* ── IRQ handler ───────────────────────────────────── */

static void virtio_net_irq(void *ctx) {
    (void)ctx;
    virtio_isr_read(&net_dev);  /* ACK interrupt */
    extern void net_poll(void);
    net_poll();
}

/* ── NIC driver interface ──────────────────────────── */

static void vnet_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = mac_addr[i];
}

static int vnet_send(const void *data, uint16_t len) {
    if (!initialized || len == 0 || len > BUF_SIZE - VIRTIO_NET_HDR_SIZE)
        return -1;

    uint64_t flags;
    spin_lock_irq(&tx_lock, &flags);

    int head = virtqueue_alloc_descs(&txq, 1);
    if (head < 0) {
        /* Try to reclaim used TX descriptors */
        uint32_t used_len;
        int used_head;
        while ((used_head = virtqueue_get_used(&txq, &used_len)) >= 0)
            virtqueue_free_chain(&txq, (uint16_t)used_head);
        head = virtqueue_alloc_descs(&txq, 1);
    }
    if (head < 0) {
        spin_unlock_irq(&tx_lock, flags);
        return -1;
    }

    /* Build packet: virtio-net header + ethernet frame */
    uint8_t *buf = tx_bufs[head];
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)buf;
    kmemset(hdr, 0, VIRTIO_NET_HDR_SIZE);
    kmemcpy(buf + VIRTIO_NET_HDR_SIZE, data, len);

    txq.desc[head].addr  = tx_bufs_phys + (uint64_t)head * BUF_SIZE;
    txq.desc[head].len   = (uint32_t)(VIRTIO_NET_HDR_SIZE + len);
    txq.desc[head].flags = 0;  /* device reads */
    txq.desc[head].next  = 0;

    virtqueue_submit(&txq, (uint16_t)head);
    virtqueue_kick(&net_dev, 1);

    spin_unlock_irq(&tx_lock, flags);
    return 0;
}

static int vnet_recv(void *buf, uint16_t bufsize) {
    uint32_t used_len;
    int head = virtqueue_get_used(&rxq, &used_len);
    if (head < 0) return 0;

    /* Strip virtio-net header */
    uint8_t *pkt = rx_bufs[head];
    if (used_len <= VIRTIO_NET_HDR_SIZE) {
        virtqueue_free_chain(&rxq, (uint16_t)head);
        rx_refill();
        return 0;
    }

    uint32_t pkt_len = used_len - (uint32_t)VIRTIO_NET_HDR_SIZE;
    if (pkt_len > bufsize) pkt_len = bufsize;
    kmemcpy(buf, pkt + VIRTIO_NET_HDR_SIZE, pkt_len);

    virtqueue_free_chain(&rxq, (uint16_t)head);
    rx_refill();

    return (int)pkt_len;
}

/* ── Init ──────────────────────────────────────────── */

int virtio_net_init(void) {
    /* PCI scan: virtio-net = device 0x1000 (legacy), subsys 1 */
    if (virtio_pci_find(0x1000, 1, &net_dev) < 0) {
        serial_puts("virtio-net: not found\n");
        return -1;
    }
    serial_puts("virtio-net: found on PCI\n");

    /* Init device, request MAC feature */
    virtio_dev_init(&net_dev, VIRTIO_NET_F_MAC);

    /* Setup virtqueues: 0=RX, 1=TX */
    if (virtqueue_setup(&net_dev, 0, &rxq) < 0 ||
        virtqueue_setup(&net_dev, 1, &txq) < 0) {
        serial_puts("virtio-net: VQ setup failed\n");
        return -1;
    }

    /* Allocate DMA buffers for RX and TX */
    void *rx_virt, *tx_virt;
    uint64_t rx_phys, tx_phys;
    if (cosmo_dma_alloc(NUM_RX_BUFS * BUF_SIZE, &rx_virt, &rx_phys) < 0 ||
        cosmo_dma_alloc(NUM_TX_BUFS * BUF_SIZE, &tx_virt, &tx_phys) < 0) {
        serial_puts("virtio-net: DMA alloc failed\n");
        return -1;
    }
    kmemset(rx_virt, 0, NUM_RX_BUFS * BUF_SIZE);
    kmemset(tx_virt, 0, NUM_TX_BUFS * BUF_SIZE);
    rx_bufs = (uint8_t (*)[BUF_SIZE])rx_virt;
    tx_bufs = (uint8_t (*)[BUF_SIZE])tx_virt;
    rx_bufs_phys = rx_phys;
    tx_bufs_phys = tx_phys;

    /* Read MAC from device config */
    if (net_dev.features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++)
            mac_addr[i] = virtio_cfg_read8(&net_dev, i);
    } else {
        /* Fallback: generate local MAC */
        mac_addr[0] = 0x52; mac_addr[1] = 0x54; mac_addr[2] = 0x00;
        mac_addr[3] = 0xCE; mac_addr[4] = 0x01; mac_addr[5] = 0x02;
    }

    /* Device ready */
    virtio_set_driver_ok(&net_dev);
    initialized = 1;

    /* Fill RX queue with buffers */
    rx_refill();

    /* Register IRQ */
    if (net_dev.irq_line >= 0)
        cosmo_irq_register(net_dev.irq_line, virtio_net_irq, 0);

    serial_puts("virtio-net: MAC=");
    for (int i = 0; i < 6; i++) {
        char hex[3];
        hex[0] = "0123456789abcdef"[mac_addr[i] >> 4];
        hex[1] = "0123456789abcdef"[mac_addr[i] & 0xF];
        hex[2] = 0;
        serial_puts(hex);
        if (i < 5) serial_putchar(':');
    }
    serial_puts(" IRQ ");
    { char t[4]; int i=0; int v=net_dev.irq_line;
      if (v < 0) { serial_putchar('-'); } else {
      do{t[i++]='0'+(v%10);v/=10;}while(v);
      while(i--) serial_putchar(t[i]); } }
    serial_putchar('\n');

    /* Register with network stack */
    static const nic_driver_t virtio_net_driver = {
        .send    = vnet_send,
        .recv    = vnet_recv,
        .get_mac = vnet_get_mac,
        .name    = "virtio-net"
    };
    net_nic_register(&virtio_net_driver);

    return 0;
}
