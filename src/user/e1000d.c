/* CosmoRT Userspace E1000 NIC Driver
 *
 * Freestanding (no libc, no kernel headers). Uses syscalls 512-519
 * for HW access. Communicates with kernel net stack via shared-memory
 * ring buffers (net_port, SYS_COSMO_NIC_ATTACH).
 *
 * Based on src/drivers/net/e1000.c — same register init sequence,
 * but all HW access through syscalls instead of kernel functions.
 */

typedef unsigned long  uint64_t;
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef long           int64_t;
typedef unsigned long  size_t;

#define NULL ((void *)0)

/* ── Syscall wrappers ───────────────────────── */

static long sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}
static long sc5(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10")=d; register long r8 __asm__("r8")=e;
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory"); return r;
}

/* SYS_write */
static void puts(const char *s) {
    int n = 0; while (s[n]) n++;
    sc3(1, 1, (long)s, n);
}

static void put_hex(uint64_t v) {
    char buf[17]; int i = 0;
    if (v == 0) { puts("0"); return; }
    while (v) { buf[i++] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
    char out[17]; int j = 0;
    while (i--) out[j++] = buf[i];
    out[j] = 0;
    puts(out);
}

/* HW syscall wrappers */
static long cosmo_mmio_map(uint64_t phys, long len, void **virt) {
    return sc3(512, (long)phys, len, (long)virt);
}
static long cosmo_dma_alloc(long len, void **virt, uint64_t *phys) {
    return sc3(513, len, (long)virt, (long)phys);
}
static long cosmo_pci_read(int bus, int dev, int fn, int reg, uint32_t *val) {
    return sc5(516, bus, dev, fn, reg, (long)val);
}
static long cosmo_pci_write(int bus, int dev, int fn, int reg, uint32_t val) {
    return sc5(517, bus, dev, fn, reg, (long)val);
}
static long cosmo_nic_attach(void *args) {
    return sc1(519, (long)args);
}

/* SYS_sched_yield / pause */
static void yield(void) { sc1(24, 0); }

/* ── SPSC Ring Buffer (inline, freestanding) ─── */

typedef struct {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t mask;
    uint32_t capacity;
    int      owns_mem;
    uint8_t  data[];
} ring_t;

static uint32_t ring_p2(uint32_t v) {
    v--; v|=v>>1; v|=v>>2; v|=v>>4; v|=v>>8; v|=v>>16;
    return v + 1;
}

static ring_t *ring_on(void *mem, long mem_size) {
    if (!mem || mem_size < (long)(sizeof(ring_t) + 16)) return NULL;
    long data_space = mem_size - (long)sizeof(ring_t);
    uint32_t cap = ring_p2((uint32_t)data_space);
    if (cap > (uint32_t)data_space) cap >>= 1;
    if (cap < 16) return NULL;
    ring_t *r = (ring_t *)mem;
    __atomic_store_n(&r->head, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&r->tail, 0, __ATOMIC_SEQ_CST);
    r->mask = cap - 1;
    r->capacity = cap;
    r->owns_mem = 0;
    return r;
}

static size_t ring_available(const ring_t *r) {
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_SEQ_CST);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_SEQ_CST);
    return (size_t)((t - h) & r->mask);
}

static size_t ring_free(const ring_t *r) {
    return (size_t)(r->capacity - 1) - ring_available(r);
}

static void mcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static size_t ring_write(ring_t *r, const void *data, size_t len) {
    if (!r || !data || len == 0) return 0;
    size_t avail = ring_free(r);
    if (len > avail) len = avail;
    if (len == 0) return 0;
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_SEQ_CST);
    uint32_t pos = t & r->mask;
    uint32_t first = r->capacity - pos;
    if (first > len) first = (uint32_t)len;
    mcpy(r->data + pos, data, first);
    if (first < len)
        mcpy(r->data, (const uint8_t *)data + first, len - first);
    __atomic_store_n(&r->tail, t + (uint32_t)len, __ATOMIC_SEQ_CST);
    return len;
}

static size_t ring_read(ring_t *r, void *buf, size_t cap) {
    if (!r || !buf || cap == 0) return 0;
    size_t avail = ring_available(r);
    if (cap > avail) cap = avail;
    if (cap == 0) return 0;
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_SEQ_CST);
    uint32_t pos = h & r->mask;
    uint32_t first = r->capacity - pos;
    if (first > cap) first = (uint32_t)cap;
    mcpy(buf, r->data + pos, first);
    if (first < cap)
        mcpy((uint8_t *)buf + first, r->data, cap - first);
    __atomic_store_n(&r->head, h + (uint32_t)cap, __ATOMIC_SEQ_CST);
    return cap;
}

/* ── E1000 Registers ────────────────────────── */

#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_ICR     0x00C0
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

#define E1000_CTRL_RST  (1 << 26)
#define E1000_CTRL_SLU  (1 << 6)
#define E1000_CTRL_ASDE (1 << 5)

#define E1000_RCTL_EN   (1 << 1)
#define E1000_RCTL_UPE  (1 << 3)
#define E1000_RCTL_MPE  (1 << 4)
#define E1000_RCTL_BAM  (1 << 15)
#define E1000_RCTL_BSIZE_2048 (0 << 16)
#define E1000_RCTL_SECRC (1 << 26)

#define E1000_TCTL_EN   (1 << 1)
#define E1000_TCTL_PSP  (1 << 3)

#define E1000_TXD_CMD_EOP  (1 << 0)
#define E1000_TXD_CMD_IFCS (1 << 1)
#define E1000_TXD_CMD_RS   (1 << 3)
#define E1000_TXD_STAT_DD  (1 << 0)
#define E1000_RXD_STAT_DD  (1 << 0)
#define E1000_RXD_STAT_EOP (1 << 1)

/* ── Descriptors ────────────────────────────── */

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

/* ── DMA state ──────────────────────────────── */

#define NUM_TX_DESC 8
#define NUM_RX_DESC 8
#define BUF_SIZE    2048

static struct e1000_tx_desc *tx_descs;
static struct e1000_rx_desc *rx_descs;
static uint8_t (*tx_bufs)[BUF_SIZE];
static uint8_t (*rx_bufs)[BUF_SIZE];
static uint64_t dma_phys_base;

static volatile uint32_t *mmio;
static uint8_t mac_addr[6];
static int tx_cur, rx_cur;

/* ── MMIO Access ────────────────────────────── */

static uint32_t e1000_read(uint32_t reg)                { return mmio[reg / 4]; }
static void     e1000_write(uint32_t reg, uint32_t val) { mmio[reg / 4] = val; }

/* ── MAC Address ────────────────────────────── */

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

/* virt_to_phys for DMA memory: compute offset from dma_virt base */
static void *dma_virt;
static uint64_t vtop(void *v) {
    return dma_phys_base + (uint64_t)((uint8_t *)v - (uint8_t *)dma_virt);
}

/* ── Net port ring buffers ──────────────────── */

#define NET_PORT_RING_SIZE (64 * 1024)

static ring_t *tx_ring;  /* kernel writes (TX requests), driver reads */
static ring_t *rx_ring;  /* driver writes (RX packets), kernel reads */

/* ── Init ───────────────────────────────────── */

static int e1000d_init(void) {
    /* PCI scan for Intel E1000 (8086:100e and variants) */
    int found_bus = -1, found_dev = -1;

    for (int bus = 0; bus < 256 && found_bus < 0; bus++) {
        for (int dev = 0; dev < 32 && found_bus < 0; dev++) {
            uint32_t id;
            if (cosmo_pci_read(bus, dev, 0, 0, &id) < 0) continue;
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
        puts("e1000d: not found\n");
        return -1;
    }
    puts("e1000d: found on PCI ");
    put_hex((uint64_t)found_bus);
    puts(":");
    put_hex((uint64_t)found_dev);
    puts("\n");

    /* Get BAR0 (MMIO base) */
    uint32_t bar0;
    cosmo_pci_read(found_bus, found_dev, 0, 0x10, &bar0);
    uint64_t mmio_phys = bar0 & 0xFFFFFFF0ULL;

    /* Check if 64-bit BAR */
    if ((bar0 & 0x06) == 0x04) {
        uint32_t bar1;
        cosmo_pci_read(found_bus, found_dev, 0, 0x14, &bar1);
        mmio_phys |= ((uint64_t)bar1 << 32);
    }

    /* Map MMIO via syscall */
    puts("e1000d: mapping MMIO at 0x");
    put_hex(mmio_phys);
    puts("\n");
    void *mmio_virt;
    long mmio_ret = cosmo_mmio_map(mmio_phys, 0x20000, &mmio_virt);
    puts("e1000d: MMIO map ret=");
    put_hex((uint64_t)mmio_ret);
    puts("\n");
    if (mmio_ret < 0) {
        puts("e1000d: MMIO map failed\n");
        return -1;
    }
    mmio = (volatile uint32_t *)mmio_virt;

    /* Enable PCI bus mastering + memory space */
    uint32_t cmd;
    cosmo_pci_read(found_bus, found_dev, 0, 0x04, &cmd);
    cmd |= (1 << 2) | (1 << 1);
    cosmo_pci_write(found_bus, found_dev, 0, 0x04, cmd);

    /* Allocate DMA region: descriptors + buffers (~64KB) */
    size_t desc_size = (NUM_TX_DESC + NUM_RX_DESC) * 16;
    size_t buf_size = (NUM_TX_DESC + NUM_RX_DESC) * BUF_SIZE;
    size_t total = desc_size + 256 + buf_size;

    puts("e1000d: DMA alloc...\n");
    if (cosmo_dma_alloc((long)total, &dma_virt, &dma_phys_base) < 0) {
        puts("e1000d: DMA alloc failed\n");
        return -1;
    }

    uint8_t *p = (uint8_t *)dma_virt;
    p = (uint8_t *)(((uint64_t)p + 15) & ~15ULL);
    tx_descs = (struct e1000_tx_desc *)p; p += NUM_TX_DESC * sizeof(struct e1000_tx_desc);
    rx_descs = (struct e1000_rx_desc *)p; p += NUM_RX_DESC * sizeof(struct e1000_rx_desc);
    p = (uint8_t *)(((uint64_t)p + 15) & ~15ULL);
    tx_bufs = (uint8_t (*)[BUF_SIZE])p; p += NUM_TX_DESC * BUF_SIZE;
    rx_bufs = (uint8_t (*)[BUF_SIZE])p;

    puts("e1000d: reset...\n");
    /* Reset */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    /* Short delay after reset — QEMU E1000 resets instantly */
    for (volatile int i = 0; i < 10000; i++);

    /* Re-enable bus mastering after reset */
    cosmo_pci_read(found_bus, found_dev, 0, 0x04, &cmd);
    cmd |= (1 << 2) | (1 << 1);
    cosmo_pci_write(found_bus, found_dev, 0, 0x04, cmd);

    /* Disable interrupts */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);

    /* Set link up */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    puts("e1000d: reading MAC...\n");
    /* Read MAC */
    read_mac();

    /* Clear multicast table */
    for (int i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);

    /* Setup RX descriptors */
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_descs[i].addr = vtop(&rx_bufs[i]);
        rx_descs[i].status = 0;
    }
    uint64_t rx_desc_phys = vtop(rx_descs);
    e1000_write(E1000_RDBAL, (uint32_t)rx_desc_phys);
    e1000_write(E1000_RDBAH, (uint32_t)(rx_desc_phys >> 32));
    e1000_write(E1000_RDLEN, NUM_RX_DESC * sizeof(struct e1000_rx_desc));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, NUM_RX_DESC - 1);
    rx_cur = 0;

    /* Enable RX */
    e1000_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_UPE |
                             E1000_RCTL_MPE | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

    /* Setup TX descriptors */
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = vtop(&tx_bufs[i]);
        tx_descs[i].status = E1000_TXD_STAT_DD;
        tx_descs[i].cmd = 0;
    }
    uint64_t tx_desc_phys = vtop(tx_descs);
    e1000_write(E1000_TDBAL, (uint32_t)tx_desc_phys);
    e1000_write(E1000_TDBAH, (uint32_t)(tx_desc_phys >> 32));
    e1000_write(E1000_TDLEN, NUM_TX_DESC * sizeof(struct e1000_tx_desc));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    tx_cur = 0;

    /* Enable TX */
    e1000_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                            (15 << 4) | (64 << 12));

    /* Disable interrupts — polling */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);

    puts("e1000d: MAC=");
    for (int i = 0; i < 6; i++) {
        char hex[3];
        hex[0] = "0123456789abcdef"[mac_addr[i] >> 4];
        hex[1] = "0123456789abcdef"[mac_addr[i] & 0xF];
        hex[2] = 0;
        puts(hex);
        if (i < 5) puts(":");
    }
    puts("\n");

    return 0;
}

/* ── TX: send packet from ring to hardware ──── */

static void hw_send(const void *data, uint16_t len) {
    if (len > BUF_SIZE) return;

    while (!(tx_descs[tx_cur].status & E1000_TXD_STAT_DD))
        __asm__ volatile("pause");

    const uint8_t *src = data;
    uint8_t *dst = tx_bufs[tx_cur];
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    tx_descs[tx_cur].length = len;
    tx_descs[tx_cur].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    tx_descs[tx_cur].status = 0;

    int old = tx_cur;
    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    e1000_write(E1000_TDT, (uint32_t)tx_cur);

    while (!(tx_descs[old].status & E1000_TXD_STAT_DD))
        __asm__ volatile("pause");
}

/* ── RX: receive packet from hardware to ring ── */

static int hw_recv(void *buf, uint16_t bufsize) {
    if (!(rx_descs[rx_cur].status & E1000_RXD_STAT_DD))
        return 0;

    uint16_t len = rx_descs[rx_cur].length;
    if (len > bufsize) len = bufsize;

    uint8_t *src = rx_bufs[rx_cur];
    uint8_t *dst = buf;
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    rx_descs[rx_cur].status = 0;
    int old = rx_cur;
    rx_cur = (rx_cur + 1) % NUM_RX_DESC;
    e1000_write(E1000_RDT, (uint32_t)old);

    return len;
}

/* ── Main ───────────────────────────────────── */

void _start(void) {
    puts("e1000d: starting userspace E1000 driver\n");

    if (e1000d_init() < 0) {
        puts("e1000d: init failed, exiting\n");
        sc1(231, 1);  /* exit_group(1) */
        __builtin_unreachable();
    }

    /* Allocate shared memory for net_port ring buffers */
    void *shm_virt;
    uint64_t shm_phys;
    long shm_size = 2 * NET_PORT_RING_SIZE;
    if (cosmo_dma_alloc(shm_size, &shm_virt, &shm_phys) < 0) {
        puts("e1000d: SHM alloc failed\n");
        sc1(231, 1);
        __builtin_unreachable();
    }

    /* Init ring buffers on shared memory.
     * Layout matches kernel net_port.c expectations:
     *   [0, RING_SIZE)            TX ring (kernel->driver: packets to send)
     *   [RING_SIZE, 2*RING_SIZE)  RX ring (driver->kernel: received packets)
     *
     * From the DRIVER's perspective:
     *   tx_ring = first half  — we READ from it (kernel wrote TX requests)
     *   rx_ring = second half — we WRITE to it (we received packets) */
    tx_ring = ring_on(shm_virt, NET_PORT_RING_SIZE);
    rx_ring = ring_on((uint8_t *)shm_virt + NET_PORT_RING_SIZE, NET_PORT_RING_SIZE);

    if (!tx_ring || !rx_ring) {
        puts("e1000d: ring init failed\n");
        sc1(231, 1);
        __builtin_unreachable();
    }

    /* Attach to kernel net stack */
    struct {
        uint64_t shm_phys;
        uint64_t shm_size;
        uint8_t  mac[6];
    } __attribute__((packed)) attach_args;
    attach_args.shm_phys = shm_phys;
    attach_args.shm_size = (uint64_t)shm_size;
    for (int i = 0; i < 6; i++) attach_args.mac[i] = mac_addr[i];

    if (cosmo_nic_attach(&attach_args) < 0) {
        puts("e1000d: NIC attach failed\n");
        sc1(231, 1);
        __builtin_unreachable();
    }

    puts("e1000d: attached to kernel net stack\n");

    /* Polling loop: bridge between hardware and kernel ring buffers */
    uint8_t pkt[2048];
    for (;;) {
        int did_work = 0;

        /* TX: kernel→driver — read packets from tx_ring, send to hardware */
        while (ring_available(tx_ring) >= 2) {
            uint8_t hdr[2];
            ring_read(tx_ring, hdr, 2);
            uint16_t pkt_len = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
            if (pkt_len > BUF_SIZE || pkt_len == 0) break;
            ring_read(tx_ring, pkt, pkt_len);
            hw_send(pkt, pkt_len);
            did_work = 1;
        }

        /* RX: hardware→kernel — receive from hardware, write to rx_ring */
        int len;
        while ((len = hw_recv(pkt, sizeof(pkt))) > 0) {
            if (ring_free(rx_ring) >= (size_t)(len + 2)) {
                uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
                ring_write(rx_ring, hdr, 2);
                ring_write(rx_ring, pkt, (size_t)len);
            }
            did_work = 1;
        }

        if (!did_work) yield();
    }
}
