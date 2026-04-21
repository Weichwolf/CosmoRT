/* CosmoRT virtio-rtc driver — probe only.
 *
 * virtio-1.3 Device ID 29 (section 5.20). Host exposes a paravirtual clock
 * with request/event virtqueues (READ / READ_CROSS / CFG_ALARM / alarm
 * events). See
 *   https://docs.oasis-open.org/virtio/virtio/v1.3/csprd01/
 *
 * Scope in CosmoRT Phase 7.7.8 is deliberately limited to detection:
 *
 *   - virtqueue round-trip per clock read is orders of magnitude slower
 *     than rdtsc / HPET MMIO and therefore unsuitable as the hot-path
 *     clocksource (the single reason to have virtio-rtc).
 *   - kvmclock (rating 400) and HPET (rating 250) already cover QEMU/KVM
 *     and bare-metal. virtio-rtc only matters on virtio-only hypervisors
 *     where neither is present — extremely rare today.
 *   - Standard QEMU (<=8.1) does not expose the device, so a full
 *     virtqueue driver would be dead code for the current test matrix.
 *
 * The probe stays so a future Phase-Audio task can graft the full read
 * path on top without reshuffling the tree. */
#ifndef VIRTIO_RTC_H
#define VIRTIO_RTC_H

#include <stdint.h>

/* Modern virtio-rtc PCI device ID: 0x1040 + virtio device ID 29. */
#define VIRTIO_RTC_PCI_DEVICE_ID    0x105Du
#define VIRTIO_RTC_VIRTIO_DEV_ID    29u

/* virtio-rtc v1.3 request opcodes (spec section 5.20.6). Kept here so a
 * later full implementation does not reintroduce magic numbers. */
#define VIRTIO_RTC_REQ_READ             0x0001u
#define VIRTIO_RTC_REQ_READ_CROSS       0x0002u
#define VIRTIO_RTC_REQ_CFG              0x1000u
#define VIRTIO_RTC_REQ_CLOCK_CAP        0x1001u
#define VIRTIO_RTC_REQ_CROSS_CAP        0x1002u

/* Clock identifiers (spec section 5.20.7). */
#define VIRTIO_RTC_CLOCK_UTC            0u
#define VIRTIO_RTC_CLOCK_TAI            1u
#define VIRTIO_RTC_CLOCK_MONOTONIC      2u

/* PCI scan + detection only. Returns 0 when a virtio-rtc device was found
 * and probed, -1 otherwise. Idempotent; calling twice returns the cached
 * result of the first scan. No virtqueue setup, no clocksource register. */
int virtio_rtc_init(void);

/* 1 iff a virtio-rtc device was located on PCI during init. */
int virtio_rtc_available(void);

/* PCI bus / slot of the detected device (valid only when available()==1).
 * Returns -1 in each out when no device was found. */
void virtio_rtc_location(int *bus_out, int *slot_out);

#endif
