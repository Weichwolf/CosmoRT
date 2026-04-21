/* virtio-rtc: probe-only driver (see virtio_rtc.h for scope rationale). */

#include "virtio_rtc.h"
#include "virtio.h"
#include "cosmort.h"

#define VIRTIO_VENDOR_ID            0x1AF4u
#define PCI_REG_VENDOR_DEVICE       0x00
#define PCI_REG_INVALID             0xFFFFFFFFu
#define PCI_VENDOR_MASK             0xFFFFu
#define PCI_DEVICE_SHIFT            16

#define RTC_NOT_PROBED              0
#define RTC_PROBED_FOUND            1
#define RTC_PROBED_MISSING         -1

static int rtc_state = RTC_NOT_PROBED;
static int rtc_bus   = -1;
static int rtc_slot  = -1;

static int rtc_match(uint16_t pci_device_id) {
    return pci_device_id == VIRTIO_RTC_PCI_DEVICE_ID;
}

int virtio_rtc_init(void) {
    if (rtc_state != RTC_NOT_PROBED)
        return rtc_state == RTC_PROBED_FOUND ? 0 : -1;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id;
            if (cosmo_pci_config_read(bus, slot, 0, PCI_REG_VENDOR_DEVICE, &id) < 0)
                continue;
            if (id == PCI_REG_INVALID)
                continue;

            uint16_t vendor = (uint16_t)(id & PCI_VENDOR_MASK);
            if (vendor != VIRTIO_VENDOR_ID)
                continue;

            uint16_t pci_device_id = (uint16_t)((id >> PCI_DEVICE_SHIFT) & PCI_VENDOR_MASK);
            if (!rtc_match(pci_device_id))
                continue;

            rtc_state = RTC_PROBED_FOUND;
            rtc_bus   = bus;
            rtc_slot  = slot;

            serial_puts("virtio-rtc: found at PCI ");
            { char t[4]; t[0]='0'+bus/10; t[1]='0'+bus%10; t[2]=':'; t[3]=0; serial_puts(t); }
            { char t[3]; t[0]='0'+slot/10; t[1]='0'+slot%10; t[2]=0; serial_puts(t); }
            serial_puts(" (probe only)\n");
            return 0;
        }
    }

    rtc_state = RTC_PROBED_MISSING;
    serial_puts("virtio-rtc: not found\n");
    return -1;
}

int virtio_rtc_available(void) {
    return rtc_state == RTC_PROBED_FOUND ? 1 : 0;
}

void virtio_rtc_location(int *bus_out, int *slot_out) {
    if (rtc_state == RTC_PROBED_FOUND) {
        if (bus_out)  *bus_out  = rtc_bus;
        if (slot_out) *slot_out = rtc_slot;
    } else {
        if (bus_out)  *bus_out  = -1;
        if (slot_out) *slot_out = -1;
    }
}
