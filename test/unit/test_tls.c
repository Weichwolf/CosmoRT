#include "ktest.h"

void test_tls(void) {
    puts("\n[TLS]\n");

    uint64_t test_val = 0xDEADBEEF12345678ULL;
    long r = sc2(SYS_arch_prctl, ARCH_SET_FS, (long)&test_val);
    check_val("arch_prctl SET_FS", r, 0);

    uint64_t readback = 0;
    r = sc2(SYS_arch_prctl, ARCH_GET_FS, (long)&readback);
    check_val("arch_prctl GET_FS", r, 0);
    check("FS base roundtrip", readback == (uint64_t)(long)&test_val);
}
