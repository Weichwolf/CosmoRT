/* CosmoRT virtio-input driver — keyboard + mouse
 *
 * virtio-input PCI: vendor 0x1AF4, device 0x1052 (modern).
 * Also matches legacy 0x1000-0x103F with subsys 18.
 * VQ 0: eventq (input events from device), VQ 1: statusq (LED control).
 *
 * IRQ-driven: events buffered in ring, read via virtio_input_read().
 * Supports multiple virtio-input devices (keyboard + mouse are separate).
 */

#include "virtio_input.h"
#include "virtio.h"
#include "cosmo.h"

/* ── Virtio-input event (from device) ──────────────── */

struct virtio_input_event_raw {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} __attribute__((packed));

/* ── Per-device state ──────────────────────────────── */

#define MAX_INPUT_DEVS 4
#define NUM_EVENT_BUFS 32
#define EVENT_SIZE     sizeof(struct virtio_input_event_raw)

struct input_dev {
    virtio_dev_t dev;
    virtqueue_t  eventq;
    uint8_t (*event_bufs)[EVENT_SIZE];   /* DMA buffers for eventq */
    uint64_t event_bufs_phys;
    int active;
};

static struct input_dev devs[MAX_INPUT_DEVS];
static int num_devs;

/* ── Event ring buffer (shared across all devices) ── */

#define EVRING_SIZE 64

static struct input_event evring[EVRING_SIZE];
static volatile int evring_head, evring_count;
static hw_spinlock_t evring_lock = HW_SPINLOCK_INIT;

static void evring_push(const struct input_event *ev) {
    if (evring_count >= EVRING_SIZE) return;  /* drop on overflow */
    int idx = (evring_head + evring_count) % EVRING_SIZE;
    evring[idx] = *ev;
    evring_count++;
}

/* ── Refill eventq with buffers ────────────────────── */

static void eventq_refill(struct input_dev *d) {
    while (d->eventq.num_free >= 1) {
        int head = virtqueue_alloc_descs(&d->eventq, 1);
        if (head < 0) break;

        d->eventq.desc[head].addr  = d->event_bufs_phys + (uint64_t)head * EVENT_SIZE;
        d->eventq.desc[head].len   = (uint32_t)EVENT_SIZE;
        d->eventq.desc[head].flags = VIRTQ_DESC_F_WRITE;
        d->eventq.desc[head].next  = 0;

        virtqueue_submit(&d->eventq, (uint16_t)head);
    }
    virtqueue_kick(&d->dev, 0);
}

/* ── IRQ handler ───────────────────────────────────── */

static void input_irq(void *ctx) {
    struct input_dev *d = (struct input_dev *)ctx;
    virtio_isr_read(&d->dev);

    uint64_t flags;
    hw_spin_lock_irq(&evring_lock, &flags);

    uint32_t len;
    int head;
    while ((head = virtqueue_get_used(&d->eventq, &len)) >= 0) {
        if (len >= EVENT_SIZE) {
            struct virtio_input_event_raw *raw =
                (struct virtio_input_event_raw *)d->event_bufs[head];
            struct input_event ev = {
                .type  = raw->type,
                .code  = raw->code,
                .value = raw->value
            };
            /* Skip EV_SYN — synchronization markers, not useful for consumers */
            if (ev.type != EV_SYN) {
                evring_push(&ev);
                /* Route to input subsystem (keyboard events → VT) */
                if (ev.type == EV_KEY) {
                    input_event_t iev = { ev.type, ev.code, ev.value };
                    input_submit_event(&iev);
                }
            }
        }
        virtqueue_free_chain(&d->eventq, (uint16_t)head);
    }

    hw_spin_unlock_irq(&evring_lock, flags);

    eventq_refill(d);
}

/* ── Init one device ───────────────────────────────── */

static int init_one(struct input_dev *d) {
    virtio_dev_init(&d->dev, 0);

    if (virtqueue_setup(&d->dev, 0, &d->eventq) < 0)
        return -1;

    /* Allocate DMA for event buffers */
    void *dma_virt;
    uint64_t dma_phys;
    uint32_t alloc = NUM_EVENT_BUFS * (uint32_t)EVENT_SIZE;
    alloc = (alloc + 4095) & ~4095U;
    if (cosmo_dma_alloc(alloc, &dma_virt, &dma_phys) < 0)
        return -1;
    hw_memset(dma_virt, 0, alloc);
    d->event_bufs      = (uint8_t (*)[EVENT_SIZE])dma_virt;
    d->event_bufs_phys = dma_phys;

    virtio_set_driver_ok(&d->dev);

    eventq_refill(d);

    if (d->dev.irq_line >= 0)
        cosmo_irq_register(d->dev.irq_line, input_irq, d);

    d->active = 1;
    return 0;
}

/* ── Input subsystem registration ──────────────────── */

static const input_driver_t virtio_input_driver = {
    .name = "virtio-input",
};

/* ── Public API ────────────────────────────────────── */

int virtio_input_init(void) {
    num_devs = 0;

    /* Scan PCI for all virtio-input devices */
    for (int bus = 0; bus < 256 && num_devs < MAX_INPUT_DEVS; bus++) {
        for (int slot = 0; slot < 32 && num_devs < MAX_INPUT_DEVS; slot++) {
            uint32_t id;
            if (cosmo_pci_config_read(bus, slot, 0, 0, &id) < 0) continue;
            if (id == 0xFFFFFFFF) continue;

            uint16_t vendor = id & 0xFFFF;
            uint16_t pci_dev = (id >> 16) & 0xFFFF;
            if (vendor != 0x1AF4) continue;

            /* virtio-input: device 0x1052 (modern) or legacy with subsys 18 */
            uint32_t subsys;
            cosmo_pci_config_read(bus, slot, 0, 0x2C, &subsys);
            uint16_t subsys_dev = (subsys >> 16) & 0xFFFF;

            int is_input = (pci_dev == 0x1052) ||
                           (pci_dev >= 0x1000 && pci_dev <= 0x103F && subsys_dev == 18);
            if (!is_input) continue;

            /* Use shared virtio transport (handles legacy I/O + modern MMIO) */
            struct input_dev *d = &devs[num_devs];
            if (virtio_pci_init_at(bus, slot, &d->dev) < 0)
                continue;

            if (init_one(d) == 0) {
                serial_puts("input: registered virtio-input dev ");
                serial_putchar('0' + num_devs);
                serial_puts(d->dev.modern ? " (modern)" : " (legacy)");
                serial_puts(" PCI ");
                { char t[4]; t[0]='0'+d->dev.pci_bus/10; t[1]='0'+d->dev.pci_bus%10; t[2]=':'; t[3]=0; serial_puts(t); }
                { char t[3]; t[0]='0'+d->dev.pci_dev/10; t[1]='0'+d->dev.pci_dev%10; t[2]=0; serial_puts(t); }
                serial_puts(" IRQ ");
                { char t[4]; int j=0; int v=d->dev.irq_line;
                  if (v < 0) { serial_putchar('-'); } else {
                  do{t[j++]='0'+(v%10);v/=10;}while(v);
                  while(j--) serial_putchar(t[j]); } }
                serial_putchar('\n');
                num_devs++;
            }
        }
    }

    if (num_devs == 0) {
        serial_puts("virtio-input: not found\n");
        return -1;
    }

    input_register(&virtio_input_driver);
    return 0;
}

int virtio_input_read(struct input_event *ev) {
    uint64_t flags;
    hw_spin_lock_irq(&evring_lock, &flags);

    if (evring_count == 0) {
        hw_spin_unlock_irq(&evring_lock, flags);
        return 0;
    }

    *ev = evring[evring_head];
    evring_head = (evring_head + 1) % EVRING_SIZE;
    evring_count--;

    hw_spin_unlock_irq(&evring_lock, flags);
    return 1;
}
