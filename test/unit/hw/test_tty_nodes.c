#include "ktest.h"

static void test_tty_nodes(void) {
    puts("\n[TTY Device Nodes]\n");
    long fd, r;
    struct k_stat st;

    /* ── /dev/tty1 through /dev/tty4 (VT0-VT3) ── */

    fd = sc3(SYS_OPEN, (long)"/dev/tty1", O_RDWR, 0);
    check("open /dev/tty1", fd >= 0);
    if (fd >= 0) {
        r = sc3(SYS_WRITE, fd, (long)"tty1\n", 5);
        check_val("write /dev/tty1 -> 5", r, 5);
        sc1(SYS_CLOSE, fd);
    }

    fd = sc3(SYS_OPEN, (long)"/dev/tty2", O_RDWR, 0);
    check("open /dev/tty2", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/dev/tty3", O_RDWR, 0);
    check("open /dev/tty3", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/dev/tty4", O_RDWR, 0);
    check("open /dev/tty4", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    /* tty5-tty12: beyond VT_MAX=4 → ENOENT */
    fd = sc3(SYS_OPEN, (long)"/dev/tty5", O_RDWR, 0);
    check_val("open /dev/tty5 -> ENOENT", fd, -ENOENT);

    fd = sc3(SYS_OPEN, (long)"/dev/tty12", O_RDWR, 0);
    check_val("open /dev/tty12 -> ENOENT", fd, -ENOENT);

    /* tty13+: out of range entirely */
    fd = sc3(SYS_OPEN, (long)"/dev/tty13", O_RDWR, 0);
    check("open /dev/tty13 fails", fd < 0);

    /* ── /dev/console ── */

    fd = sc3(SYS_OPEN, (long)"/dev/console", O_RDWR, 0);
    check("open /dev/console", fd >= 0);
    if (fd >= 0) {
        r = sc3(SYS_WRITE, fd, (long)"con\n", 4);
        check_val("write /dev/console -> 4", r, 4);
        sc1(SYS_CLOSE, fd);
    }

    /* ── /dev/pts/0 through /dev/pts/3 ── */

    fd = sc3(SYS_OPEN, (long)"/dev/pts/0", O_RDWR, 0);
    check("open /dev/pts/0", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/dev/pts/3", O_RDWR, 0);
    check("open /dev/pts/3", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/dev/pts/4", O_RDWR, 0);
    check_val("open /dev/pts/4 -> ENOENT", fd, -ENOENT);

    /* ── stat on tty nodes ── */

    r = sc2(SYS_STAT, (long)"/dev/tty1", (long)&st);
    check("stat /dev/tty1", r == 0);
    if (r == 0) {
        check("stat /dev/tty1 S_IFCHR", (st.st_mode & S_IFMT) == S_IFCHR);
        check_val("stat /dev/tty1 rdev", (long)st.st_rdev, 0x0401);
    }

    r = sc2(SYS_STAT, (long)"/dev/tty4", (long)&st);
    check("stat /dev/tty4", r == 0);
    if (r == 0)
        check_val("stat /dev/tty4 rdev", (long)st.st_rdev, 0x0404);

    r = sc2(SYS_STAT, (long)"/dev/console", (long)&st);
    check("stat /dev/console", r == 0);
    if (r == 0) {
        check("stat /dev/console S_IFCHR", (st.st_mode & S_IFMT) == S_IFCHR);
        check_val("stat /dev/console rdev", (long)st.st_rdev, 0x0501);
    }

    r = sc2(SYS_STAT, (long)"/dev/pts/0", (long)&st);
    check("stat /dev/pts/0", r == 0);
    if (r == 0)
        check_val("stat /dev/pts/0 rdev", (long)st.st_rdev, 0x8800);

    /* ── termios on tty1 ── */

    fd = sc3(SYS_OPEN, (long)"/dev/tty1", O_RDWR, 0);
    check("open /dev/tty1 for ioctl", fd >= 0);
    if (fd >= 0) {
        struct {
            unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
            unsigned char c_line;
            unsigned char c_cc[19];
        } tios;
        r = sc3(SYS_IOCTL, fd, 0x5401 /*TCGETS*/, (long)&tios);
        check("TCGETS on /dev/tty1", r == 0);
        if (r == 0) {
            check("c_iflag has ICRNL", (tios.c_iflag & 0x0100) != 0);
            check("c_oflag has OPOST|ONLCR", (tios.c_oflag & 0x0005) == 0x0005);
            check("c_lflag has ISIG|ICANON|ECHO",
                  (tios.c_lflag & 0x000B) == 0x000B);
            check_val("c_cc[VINTR]=3", (long)tios.c_cc[0], 3);
            check_val("c_cc[VEOF]=4", (long)tios.c_cc[4], 4);
            check_val("c_cc[VMIN]=1", (long)tios.c_cc[6], 1);
            check_val("c_cc[VSUSP]=26", (long)tios.c_cc[10], 26);
        }

        /* TCSETS: disable echo, verify TCGETS reflects it */
        if (r == 0) {
            tios.c_lflag &= ~0x08u; /* clear ECHO */
            r = sc3(SYS_IOCTL, fd, 0x5402 /*TCSETS*/, (long)&tios);
            check("TCSETS disable echo", r == 0);
            r = sc3(SYS_IOCTL, fd, 0x5401 /*TCGETS*/, (long)&tios);
            check("TCGETS after TCSETS", r == 0);
            if (r == 0)
                check("echo cleared", (tios.c_lflag & 0x08) == 0);
            /* Restore echo */
            tios.c_lflag |= 0x08u;
            sc3(SYS_IOCTL, fd, 0x5402, (long)&tios);
        }

        sc1(SYS_CLOSE, fd);
    }
}

TEST("tty_nodes", test_tty_nodes);
