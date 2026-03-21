/* CosmoRT Network Port — shared-memory ring bridge for userspace NIC drivers.
 *
 * Kernel side of the ring pair. Implements nic_driver_t so the existing
 * net stack (IP/TCP/DHCP/DNS) works unchanged whether the NIC driver
 * is in-kernel (e1000) or userspace (via this port).
 */

#include "net_port.h"
#include "net.h"
#include "config.h"
#include "serial.h"
#include "process.h"
#include "percpu.h"

static struct net_port port;

/* ── nic_driver_t implementation over ring buffers ── */

static int port_send(const void *data, uint16_t len) {
    if (!port.active || !port.tx_ring) return -1;
    if (ring_free(port.tx_ring) < (size_t)(len + 2)) return -1;
    /* Length-prefix: [uint16_t len LE][payload] */
    uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    ring_write(port.tx_ring, hdr, 2);
    ring_write(port.tx_ring, data, len);
    return 0;
}

static int port_recv(void *buf, uint16_t bufsize) {
    if (!port.active || !port.rx_ring) return 0;
    if (ring_available(port.rx_ring) < 2) return 0;
    /* Peek length prefix */
    uint8_t hdr[2];
    ring_read(port.rx_ring, hdr, 2);
    uint16_t pkt_len = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    if (pkt_len > bufsize) {
        /* Oversized — drain and discard */
        uint8_t skip[64];
        size_t rem = pkt_len;
        while (rem > 0) {
            size_t chunk = rem > 64 ? 64 : rem;
            ring_read(port.rx_ring, skip, chunk);
            rem -= chunk;
        }
        return 0;
    }
    ring_read(port.rx_ring, buf, pkt_len);
    return pkt_len;
}

static void port_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = port.mac[i];
}

static const nic_driver_t port_driver = {
    .send    = port_send,
    .recv    = port_recv,
    .get_mac = port_get_mac,
    .name    = "net_port"
};

/* ── Public API ── */

void net_port_init(void) {
    port.active = 0;
    port.tx_ring = 0;
    port.rx_ring = 0;
    port.shm_virt = 0;
    port.driver_pid = -1;
}

int net_port_attach(uint64_t shm_phys, size_t shm_size, const uint8_t mac[6]) {
    if (shm_size < 2 * NET_PORT_RING_SIZE) return -1;

    /* Map via direct physical map */
    port.shm_virt = (void *)(shm_phys + PHYS_OFFSET);
    port.shm_phys = shm_phys;
    port.shm_size = shm_size;

    /* First half = TX (kernel writes), second half = RX (driver writes) */
    port.tx_ring = ring_on(port.shm_virt, NET_PORT_RING_SIZE);
    port.rx_ring = ring_on((uint8_t *)port.shm_virt + NET_PORT_RING_SIZE,
                           NET_PORT_RING_SIZE);

    if (!port.tx_ring || !port.rx_ring) {
        serial_puts("net_port: ring init failed\n");
        return -1;
    }

    for (int i = 0; i < 6; i++) port.mac[i] = mac[i];

    process_t *p = proc_current();
    port.driver_pid = p ? (int)p->pid : -1;
    port.active = 1;

    net_nic_register(&port_driver);

    serial_puts("net_port: attached pid=");
    serial_hex64((uint64_t)port.driver_pid);
    serial_puts(" shm=");
    serial_hex64(shm_phys);
    serial_putchar('\n');

    return 0;
}

void net_port_detach(void) {
    if (!port.active) return;
    port.active = 0;
    port.tx_ring = 0;
    port.rx_ring = 0;
    net_nic_register(0);  /* deregister NIC — net.c is now NULL-safe */
    serial_puts("net_port: detached\n");
}

int net_port_active(void) {
    return port.active;
}

void net_port_check_driver(int pid) {
    if (port.active && port.driver_pid == pid) {
        serial_puts("net_port: driver exited, detaching\n");
        net_port_detach();
    }
}
