/* Intel E1000 (82540EM) NIC Driver for CosmoRT
 * Higher-half kernel: all DMA addrs via virt_to_phys(), MMIO via cosmo_mmio_map().
 * Adapted from llmos/src/drivers/net/e1000.c
 */

#include "e1000.h"
#include "cosmo_rt.h"

/* ── E1000 Registers ───────────────────────────────── */

#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_EERD    0x0014
#define E1000_ICR     0x00C0
#define E1000_ICS     0x00C8
#define E1000_IMS     0x00D0
#define E1000_IMC     0x00D8

#define E1000_RCTL    0x0100
#define E1000_TCTL    0x0400
#define E1000_RDBAL   0x2800
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810
#define E1000_RDT     0x2818
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_RAL     0x5400
#define E1000_RAH     0x5404
#define E1000_MTA     0x5200

/* CTRL bits */
#define E1000_CTRL_RST  (1 << 26)
#define E1000_CTRL_SLU  (1 << 6)
#define E1000_CTRL_ASDE (1 << 5)

/* RCTL bits */
#define E1000_RCTL_EN   (1 << 1)
#define E1000_RCTL_UPE  (1 << 3)
#define E1000_RCTL_MPE  (1 << 4)
#define E1000_RCTL_BAM  (1 << 15)
#define E1000_RCTL_BSIZE_2048 (0 << 16)
#define E1000_RCTL_SECRC (1 << 26)

/* TCTL bits */
#define E1000_TCTL_EN   (1 << 1)
#define E1000_TCTL_PSP  (1 << 3)

/* TX descriptor command/status bits */
#define E1000_TXD_CMD_EOP  (1 << 0)
#define E1000_TXD_CMD_IFCS (1 << 1)
#define E1000_TXD_CMD_RS   (1 << 3)
#define E1000_TXD_STAT_DD  (1 << 0)

/* RX descriptor status bits */
#define E1000_RXD_STAT_DD  (1 << 0)
#define E1000_RXD_STAT_EOP (1 << 1)

/* ── Descriptors ───────────────────────────────────── */

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* ── DMA Buffers ───────────────────────────────────── */

#define NUM_TX_DESC 8
#define NUM_RX_DESC 8
#define BUF_SIZE    2048

/* Virtual pointers for kernel access */
static volatile struct e1000_tx_desc *tx_descs;
static volatile struct e1000_rx_desc *rx_descs;
static uint8_t (*tx_bufs)[BUF_SIZE];
static uint8_t (*rx_bufs)[BUF_SIZE];

/* Physical base of DMA region (for descriptor addr fields) */
static uint64_t dma_phys_base;

static volatile uint32_t *mmio;
static uint8_t mac_addr[6];
static int tx_cur, rx_cur;

/* ── MMIO Access ───────────────────────────────────── */

static uint32_t e1000_read(uint32_t reg) { return mmio[reg / 4]; }
static void e1000_write(uint32_t reg, uint32_t val) { mmio[reg / 4] = val; }

/* ── MAC Address ───────────────────────────────────── */

static void read_mac(void) {
    uint32_t lo = e1000_read(E1000_RAL);
    uint32_t hi = e1000_read(E1000_RAH);
    mac_addr[0] = lo & 0xFF;
    mac_addr[1] = (lo >> 8) & 0xFF;
    mac_addr[2] = (lo >> 16) & 0xFF;
    mac_addr[3] = (lo >> 24) & 0xFF;
    mac_addr[4] = hi & 0xFF;
    mac_addr[5] = (hi >> 8) & 0xFF;
}

/* ── IRQ Handler (called on packet receive) ────────── */

static void e1000_irq_handler(void *ctx) {
    (void)ctx;
    e1000_read(E1000_ICR); /* read + auto-clear interrupt cause */
    extern void net_poll(void);
    /* Drain all pending packets (not just one) */
    for (int i = 0; i < 16; i++) {
        net_poll();
        if (!(rx_descs[rx_cur].status & E1000_RXD_STAT_DD)) break;
    }
}

/* ── Init ──────────────────────────────────────────── */

int e1000_init(void) {
    /* PCI scan for Intel E1000 (8086:100e and variants) */
    int found_bus = -1, found_dev = -1;

    for (int bus = 0; bus < 256 && found_bus < 0; bus++) {
        for (int dev = 0; dev < 32 && found_bus < 0; dev++) {
            uint32_t id;
            if (cosmo_pci_config_read(bus, dev, 0, 0, &id) < 0) continue;
            if (id == 0xFFFFFFFF) continue;
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;
            if (vendor == 0x8086 && (device == 0x100E || device == 0x100F ||
                                     device == 0x153A || device == 0x10D3)) {
                found_bus = bus;
                found_dev = dev;
            }
        }
    }

    if (found_bus < 0) {
        serial_puts("e1000: not found\n");
        return -1;
    }
    serial_puts("e1000: found on PCI\n");

    /* Get BAR0 (MMIO base) */
    uint32_t bar0;
    cosmo_pci_config_read(found_bus, found_dev, 0, 0x10, &bar0);
    uint64_t mmio_phys = bar0 & 0xFFFFFFF0ULL;

    /* Check if 64-bit BAR */
    if ((bar0 & 0x06) == 0x04) {
        uint32_t bar1;
        cosmo_pci_config_read(found_bus, found_dev, 0, 0x14, &bar1);
        mmio_phys |= ((uint64_t)bar1 << 32);
    }

    /* Register BAR range and map MMIO into kernel address space */
    hw_allow_mmio(mmio_phys, 0x20000);
    void *mmio_virt;
    if (cosmo_mmio_map(mmio_phys, 0x20000, &mmio_virt) < 0) {
        serial_puts("e1000: MMIO map failed\n");
        return -1;
    }
    mmio = (volatile uint32_t *)mmio_virt;

    /* Enable PCI bus mastering + memory space */
    uint32_t cmd;
    cosmo_pci_config_read(found_bus, found_dev, 0, 0x04, &cmd);
    cmd |= (1 << 2) | (1 << 1);
    cosmo_pci_config_write(found_bus, found_dev, 0, 0x04, cmd);

    /* Allocate DMA region: descriptors + buffers.
     * Layout: [tx_descs | rx_descs | padding | tx_bufs | rx_bufs]
     * ~64KB total */
    size_t desc_size = (NUM_TX_DESC + NUM_RX_DESC) * 16;  /* 16 bytes per desc */
    size_t buf_size = (NUM_TX_DESC + NUM_RX_DESC) * BUF_SIZE;
    size_t total = desc_size + 256 + buf_size;  /* 256 alignment slack */

    void *dma_virt;
    if (cosmo_dma_alloc(total, &dma_virt, &dma_phys_base) < 0) {
        serial_puts("e1000: DMA alloc failed\n");
        return -1;
    }

    uint8_t *p = (uint8_t *)dma_virt;
    /* Align to 16 bytes */
    p = (uint8_t *)(((uint64_t)p + 15) & ~15ULL);
    tx_descs = (struct e1000_tx_desc *)p; p += NUM_TX_DESC * sizeof(struct e1000_tx_desc);
    rx_descs = (struct e1000_rx_desc *)p; p += NUM_RX_DESC * sizeof(struct e1000_rx_desc);
    p = (uint8_t *)(((uint64_t)p + 15) & ~15ULL);
    tx_bufs = (uint8_t (*)[BUF_SIZE])p; p += NUM_TX_DESC * BUF_SIZE;
    rx_bufs = (uint8_t (*)[BUF_SIZE])p;

    /* Reset */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    for (volatile int i = 0; i < 1000000; i++);

    /* Re-enable bus mastering after reset */
    cosmo_pci_config_read(found_bus, found_dev, 0, 0x04, &cmd);
    cmd |= (1 << 2) | (1 << 1);
    cosmo_pci_config_write(found_bus, found_dev, 0, 0x04, cmd);

    /* Disable interrupts */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);

    /* Set link up */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    /* Read MAC */
    read_mac();

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);

    /* Setup RX descriptors — device needs PHYSICAL addresses */
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr = virt_to_phys(&rx_bufs[i]);
        rx_descs[i].status = 0;
    }
    uint64_t rx_desc_phys = virt_to_phys(rx_descs);
    e1000_write(E1000_RDBAL, (uint32_t)rx_desc_phys);
    e1000_write(E1000_RDBAH, (uint32_t)(rx_desc_phys >> 32));
    e1000_write(E1000_RDLEN, NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, NUM_RX_DESC - 1);
    rx_cur = 0;

    /* Enable RX */
    e1000_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_UPE |
                             E1000_RCTL_MPE | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

    /* Setup TX descriptors — device needs PHYSICAL addresses */
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = virt_to_phys(&tx_bufs[i]);
        tx_descs[i].status = E1000_TXD_STAT_DD;
        tx_descs[i].cmd = 0;
    }
    uint64_t tx_desc_phys = virt_to_phys(tx_descs);
    e1000_write(E1000_TDBAL, (uint32_t)tx_desc_phys);
    e1000_write(E1000_TDBAH, (uint32_t)(tx_desc_phys >> 32));
    e1000_write(E1000_TDLEN, NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    tx_cur = 0;

    /* Enable TX */
    e1000_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                            (15 << 4) | (64 << 12));

    /* Clear pending interrupts */
    e1000_read(E1000_ICR);

    serial_puts("e1000: MAC=");
    for (int i = 0; i < 6; i++) {
        char hex[3];
        hex[0] = "0123456789abcdef"[mac_addr[i] >> 4];
        hex[1] = "0123456789abcdef"[mac_addr[i] & 0xF];
        hex[2] = 0;
        serial_puts(hex);
        if (i < 5) serial_putchar(':');
    }
    serial_putchar('\n');

    /* Register with network stack */
    static const nic_driver_t e1000_driver = {
        .send    = e1000_send,
        .recv    = e1000_recv,
        .get_mac = e1000_get_mac,
        .name    = "e1000"
    };
    net_nic_register(&e1000_driver);

    /* Enable RX interrupts (IRQ-driven, no polling) */
    uint32_t irq_reg;
    cosmo_pci_config_read(found_bus, found_dev, 0, 0x3C, &irq_reg);
    int irq_line = (int)(irq_reg & 0xFF);
    cosmo_irq_register(irq_line, e1000_irq_handler, 0);
    /* Set RX interrupt delay to 0 for immediate notification */
    e1000_write(0x2820, 0);  /* RDTR = 0 (receive delay timer) */
    e1000_write(0x282C, 0);  /* RADV = 0 (receive absolute delay) */
    /* Enable: RXT0 (bit 7) + RXO (bit 6) + RXDMT0 (bit 4) + LSC (bit 2) */
    e1000_write(E1000_IMS, 0xD4);
    e1000_read(E1000_ICR); /* clear pending */

    serial_puts("e1000: IRQ ");
    serial_putchar('0' + (irq_line / 10));
    serial_putchar('0' + (irq_line % 10));
    serial_puts(" (interrupt-driven)\n");

    return 0;
}

/* ── Public API ───────────────────────────────────── */

void e1000_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = mac_addr[i];
}

int e1000_send(const void *data, uint16_t len) {
    if (len > BUF_SIZE) return -1;

    /* Wait for previous TX to complete (timeout after ~1000 iterations) */
    for (int w = 0; w < 1000; w++) {
        if (tx_descs[tx_cur].status & E1000_TXD_STAT_DD) break;
        __asm__ volatile("pause");
        if (w == 999) { serial_puts("e1000: TX timeout\n"); return -1; }
    }

    /* Copy data to TX buffer */
    const uint8_t *src = data;
    uint8_t *dst = tx_bufs[tx_cur];
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    /* Setup descriptor */
    tx_descs[tx_cur].length = len;
    tx_descs[tx_cur].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    tx_descs[tx_cur].status = 0;

    /* Advance tail */
    int old = tx_cur;
    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    e1000_write(E1000_TDT, tx_cur);

    /* Wait for completion (timeout after ~1000 iterations) */
    for (int w = 0; w < 1000; w++) {
        if (tx_descs[old].status & E1000_TXD_STAT_DD) break;
        __asm__ volatile("pause");
        if (w == 999) { serial_puts("e1000: TX completion timeout\n"); return -1; }
    }

    return 0;
}

int e1000_recv(void *buf, uint16_t bufsize) {
    if (!(rx_descs[rx_cur].status & E1000_RXD_STAT_DD))
        return 0;

    uint16_t len = rx_descs[rx_cur].length;
    if (len > bufsize) len = bufsize;

    /* Copy from RX buffer (kernel virtual) to caller */
    uint8_t *src = rx_bufs[rx_cur];
    uint8_t *dst = buf;
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    /* Return descriptor to hardware */
    rx_descs[rx_cur].status = 0;
    int old = rx_cur;
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    e1000_write(E1000_RDT, old);

    return len;
}
