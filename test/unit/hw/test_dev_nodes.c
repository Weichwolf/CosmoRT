#include "ktest.h"

static void test_dev_nodes(void) {
    puts("\n[Device Nodes]\n");
    long fd, r;
    char buf[128];

    /* ── /dev/null ─────────────────────────── */

    fd = sc3(SYS_OPEN, (long)"/dev/null", O_RDWR, 0);
    check("open /dev/null", fd >= 0);

    r = sc3(SYS_WRITE, fd, (long)"discard", 7);
    check_val("write /dev/null → 7", r, 7);

    r = sc3(SYS_READ, fd, (long)buf, 100);
    check_val("read /dev/null → 0 (EOF)", r, 0);

    sc1(SYS_CLOSE, fd);

    /* ── /dev/zero ─────────────────────────── */

    fd = sc3(SYS_OPEN, (long)"/dev/zero", O_RDWR, 0);
    check("open /dev/zero", fd >= 0);

    /* read fills zeros */
    for (int i = 0; i < 100; i++) buf[i] = 0xFF;
    r = sc3(SYS_READ, fd, (long)buf, 100);
    check_val("read /dev/zero → 100", r, 100);
    int all_zero = 1;
    for (int i = 0; i < 100; i++)
        if (buf[i] != 0) { all_zero = 0; break; }
    check("read /dev/zero all zeros", all_zero);

    /* write discards */
    r = sc3(SYS_WRITE, fd, (long)"discard", 7);
    check_val("write /dev/zero → 7", r, 7);

    sc1(SYS_CLOSE, fd);

    /* ── /dev/urandom ──────────────────────── */

    fd = sc3(SYS_OPEN, (long)"/dev/urandom", O_RDWR, 0);
    check("open /dev/urandom", fd >= 0);

    for (int i = 0; i < 32; i++) buf[i] = 0;
    r = sc3(SYS_READ, fd, (long)buf, 32);
    check_val("read /dev/urandom → 32", r, 32);

    /* At least some bytes should be non-zero (P(all-zero) ≈ 2^-256) */
    int nonzero = 0;
    for (int i = 0; i < 32; i++)
        if (buf[i] != 0) nonzero++;
    check("read /dev/urandom not all zeros", nonzero > 0);

    /* Check not all same byte */
    int all_same = 1;
    for (int i = 1; i < 32; i++)
        if (buf[i] != buf[0]) { all_same = 0; break; }
    check("read /dev/urandom not all same", !all_same);

    /* write discards */
    r = sc3(SYS_WRITE, fd, (long)"discard", 7);
    check_val("write /dev/urandom → 7", r, 7);

    sc1(SYS_CLOSE, fd);

    /* ── /dev/random (alias for urandom) ───── */

    fd = sc3(SYS_OPEN, (long)"/dev/random", O_RDONLY, 0);
    check("open /dev/random", fd >= 0);
    r = sc3(SYS_READ, fd, (long)buf, 16);
    check_val("read /dev/random → 16", r, 16);
    sc1(SYS_CLOSE, fd);

    /* ── /dev/tty (alias for controlling terminal) ─ */

    fd = sc3(SYS_OPEN, (long)"/dev/tty", O_RDWR, 0);
    check("open /dev/tty succeeds or ENOTTY",
          fd >= 0 || fd == -ENOTTY);
    if (fd >= 0) {
        /* Write should work (goes to PTY/serial) */
        r = sc3(SYS_WRITE, fd, (long)"tty\n", 4);
        check_val("write /dev/tty → 4", r, 4);
        sc1(SYS_CLOSE, fd);
    }

    /* ── stat on device nodes ──────────────── */

    struct k_stat st;
    r = sc2(SYS_STAT, (long)"/dev/null", (long)&st);
    check("stat /dev/null succeeds", r == 0);
    if (r == 0) {
        check("stat /dev/null S_IFCHR", (st.st_mode & S_IFMT) == S_IFCHR);
        check_val("stat /dev/null rdev", (long)st.st_rdev, 0x0103);
    }

    r = sc2(SYS_STAT, (long)"/dev/zero", (long)&st);
    check("stat /dev/zero succeeds", r == 0);
    if (r == 0)
        check_val("stat /dev/zero rdev", (long)st.st_rdev, 0x0105);

    r = sc2(SYS_STAT, (long)"/dev/urandom", (long)&st);
    check("stat /dev/urandom succeeds", r == 0);
    if (r == 0)
        check_val("stat /dev/urandom rdev", (long)st.st_rdev, 0x0109);

    /* ── mmap /dev/zero → zero pages ───────── */

    fd = sc3(SYS_OPEN, (long)"/dev/zero", O_RDWR, 0);
    check("open /dev/zero for mmap", fd >= 0);
    if (fd >= 0) {
        long addr = sc6(SYS_MMAP, 0, 4096, PROT_RW,
                        MAP_PRIVATE, fd, 0);
        check("mmap /dev/zero succeeds", addr > 0);
        if (addr > 0) {
            volatile char *p = (volatile char *)addr;
            int mmap_zero = 1;
            for (int i = 0; i < 4096; i++)
                if (p[i] != 0) { mmap_zero = 0; break; }
            check("mmap /dev/zero pages are zero", mmap_zero);
            sc2(SYS_MUNMAP, addr, 4096);
        }
        sc1(SYS_CLOSE, fd);
    }
}

TEST("dev_nodes", test_dev_nodes);
