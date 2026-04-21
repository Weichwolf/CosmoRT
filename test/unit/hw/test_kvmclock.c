/* KVM pvclock clocksource self-tests.
 *
 * Subcases 60-63 via SYS_COSMO_RT_QUERY. Auf QEMU-TCG (kein KVM) ist
 * kvmclock_available()==0 — Register/Monotonic degradieren zu no-op
 * success (analog Hyper-V). Subcase 62 prüft die Read-Formel und das
 * Sequence-Lock-Bit unabhängig von der Host-Präsenz. */
#include "ktest.h"
#include "cosmort.h"

#define KVMCS_SUB_INIT_NO_KVM      60
#define KVMCS_SUB_REGISTER_PRESENT 61
#define KVMCS_SUB_SEQLOCK_RETRY    62
#define KVMCS_SUB_MONOTONIC        63

static void test_kvmcs_init_no_kvm(void) {
    puts("\n[KVM-CLOCK]\n");
    check_val("kvmclock_init no-op when KVM absent",
              sc2(SYS_COSMO_RT_QUERY, KVMCS_SUB_INIT_NO_KVM, 0), 0);
}

static void test_kvmcs_register_present(void) {
    check_val("clocksource kvm-clock registered rating 400 under KVM",
              sc2(SYS_COSMO_RT_QUERY, KVMCS_SUB_REGISTER_PRESENT, 0), 0);
}

static void test_kvmcs_seqlock_retry(void) {
    check_val("pvclock compose + odd-version seqlock bit",
              sc2(SYS_COSMO_RT_QUERY, KVMCS_SUB_SEQLOCK_RETRY, 0), 0);
}

static void test_kvmcs_monotonic(void) {
    check_val("kvmclock_read_ns monoton (skip ohne KVM)",
              sc2(SYS_COSMO_RT_QUERY, KVMCS_SUB_MONOTONIC, 0), 0);
}

TEST("kvm_clock/init_no_kvm",      test_kvmcs_init_no_kvm);
TEST("kvm_clock/register_present", test_kvmcs_register_present);
TEST("kvm_clock/seqlock_retry",    test_kvmcs_seqlock_retry);
TEST("kvm_clock/monotonic",        test_kvmcs_monotonic);
