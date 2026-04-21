/* virtio-rtc probe selftests.
 *
 * Subcases 90-93 via SYS_COSMO_RT_QUERY. Phase 7.7.8 ships only the PCI
 * probe; full virtqueue-backed clock reads are deferred (spec 1.3 §5.20
 * round-trip is ill-suited as hot-path clocksource). Tests therefore
 * verify spec constants, probe idempotency, location consistency, and
 * that the driver deliberately does NOT appear in the clocksource chain.
 *
 * Under standard QEMU no virtio-rtc device is exposed → available()==0
 * and location-case expects (-1,-1). On a hypervisor that does expose
 * the device the same tests pass with (bus,slot) filled in. */
#include "ktest.h"
#include "cosmort.h"

#define VRTC_SUB_SPEC_CONSTANTS  90
#define VRTC_SUB_PROBE_IDEMPOTENT 91
#define VRTC_SUB_LOCATION        92
#define VRTC_SUB_NOT_CLOCKSOURCE 93

static void test_vrtc_spec_constants(void) {
    puts("\n[VIRTIO-RTC]\n");
    check_val("PCI device-id = 0x1040+29, opcodes distinct, clock-ids distinct",
              sc2(SYS_COSMO_RT_QUERY, VRTC_SUB_SPEC_CONSTANTS, 0), 0);
}

static void test_vrtc_probe_idempotent(void) {
    check_val("virtio_rtc_init idempotent + available() matches return",
              sc2(SYS_COSMO_RT_QUERY, VRTC_SUB_PROBE_IDEMPOTENT, 0), 0);
}

static void test_vrtc_location(void) {
    check_val("virtio_rtc_location consistent with available()",
              sc2(SYS_COSMO_RT_QUERY, VRTC_SUB_LOCATION, 0), 0);
}

static void test_vrtc_not_clocksource(void) {
    check_val("virtio-rtc deliberately not registered as clocksource",
              sc2(SYS_COSMO_RT_QUERY, VRTC_SUB_NOT_CLOCKSOURCE, 0), 0);
}

TEST("virtio_rtc/spec_constants",    test_vrtc_spec_constants);
TEST("virtio_rtc/probe_idempotent",  test_vrtc_probe_idempotent);
TEST("virtio_rtc/location",          test_vrtc_location);
TEST("virtio_rtc/not_clocksource",   test_vrtc_not_clocksource);
