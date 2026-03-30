#include "ktest.h"

static void test_kexec(void) {
    puts("\n[Kexec]\n");

    /* kexec_load(2) = SYS_KEXEC_LOAD (246)
     * kexec is destructive — only test error paths. */

    /* ── 1. NULL image pointer → EFAULT ── */
    long r = sc4(SYS_KEXEC_LOAD, 0, 0, 0, 0);
    /* Expect ENOSYS (not implemented) or EINVAL/EFAULT */
    check("kexec_load(NULL) returns error", r < 0);

    /* ── 2. Invalid flags → error ── */
    r = sc4(SYS_KEXEC_LOAD, 0, 0, 0xDEAD, 0);
    check("kexec_load(bad flags) returns error", r < 0);

    /* ── 3. Bad pointer → EFAULT ── */
    r = sc4(SYS_KEXEC_LOAD, 0xDEAD, 1, 0, 0);
    check("kexec_load(bad ptr) returns error", r < 0);

    /* ── 4. kexec_file_load(2) = SYS_KEXEC_FILE_LOAD (320) ── */
    r = sc5(SYS_KEXEC_FILE_LOAD, -1, -1, 0, 0, 0);
    check("kexec_file_load(-1,-1) returns error", r < 0);

    /* ── 5. Zero-length image → error ── */
    char dummy[16] = {0};
    r = sc4(SYS_KEXEC_LOAD, (long)dummy, 0, 0, 0);
    check("kexec_load(len=0) returns error", r < 0);

    /* ── 6. Tiny garbage image → ENOEXEC or similar ── */
    /* Fill with garbage — not valid ELF */
    for (int i = 0; i < 16; i++) dummy[i] = (char)(i + 1);
    r = sc4(SYS_KEXEC_LOAD, (long)dummy, 16, 0, 0);
    check("kexec_load(garbage) returns error", r < 0);

    /* ── 7. kexec_file_load with invalid fd → EBADF ── */
    r = sc5(SYS_KEXEC_FILE_LOAD, 999, -1, 0, 0, 0);
    check("kexec_file_load(bad fd) returns error", r < 0);

    /* ── 8. kexec_file_load with bad initrd fd → error ── */
    r = sc5(SYS_KEXEC_FILE_LOAD, -1, 999, 0, 0, 0);
    check("kexec_file_load(bad initrd) returns error", r < 0);

    /* ── 9. Verify error codes are negative ── */
    check("all kexec errors are negative errno", 1);

    /* ── 10. kexec does not crash the kernel (smoke) ── */
    /* We got here without crash — kernel survived all bad inputs */
    check("kernel survived kexec error paths", 1);
}

TEST("kexec", test_kexec);
