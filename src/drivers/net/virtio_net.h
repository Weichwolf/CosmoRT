/* CosmoRT virtio-net driver — alternative NIC for QEMU/KVM
 * Registers with net stack via net_nic_register().
 */
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

/* PCI probe + virtqueue init + IRQ setup.
 * Returns 0 on success, -1 if not found. */
int virtio_net_init(void);

#endif
