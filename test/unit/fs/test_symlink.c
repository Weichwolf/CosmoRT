#include "ktest.h"

/* ── Symlink: create, read, chain, lstat, unlink ── */

static void test_symlink(void) {
    puts("\n[SYMLINK]\n");

    /* ── basic: create file, symlink to it, read through symlink ── */
    long fd = sc3(SYS_OPEN, (long)"/tmp/sym_target", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create /tmp/sym_target", fd >= 0);
    if (fd >= 0) {
        sc3(SYS_WRITE, fd, (long)"SYMDATA", 7);
        sc1(SYS_CLOSE, fd);
    }

    long r = sc2(SYS_SYMLINK, (long)"/tmp/sym_target", (long)"/tmp/sym_link");
    check_val("symlink /tmp/sym_link -> /tmp/sym_target", r, 0);

    fd = sc3(SYS_OPEN, (long)"/tmp/sym_link", O_RDONLY, 0);
    check("open via symlink", fd >= 0);
    if (fd >= 0) {
        char buf[16] = {0};
        r = sc3(SYS_READ, fd, (long)buf, 7);
        check_val("read 7 bytes via symlink", r, 7);
        check("data matches", buf[0]=='S' && buf[1]=='Y' && buf[2]=='M');
        sc1(SYS_CLOSE, fd);
    }

    /* ── readlink: returns correct target ── */
    char rlbuf[128] = {0};
    r = sc3(SYS_READLINK, (long)"/tmp/sym_link", (long)rlbuf, 127);
    check("readlink length", r > 0);
    check("readlink target", rlbuf[0]=='/' && rlbuf[1]=='t' && rlbuf[2]=='m' &&
          rlbuf[3]=='p' && rlbuf[4]=='/' && rlbuf[5]=='s' && rlbuf[6]=='y' &&
          rlbuf[7]=='m' && rlbuf[8]=='_' && rlbuf[9]=='t');

    /* ── chain: c -> b -> a -> file ── */
    fd = sc3(SYS_OPEN, (long)"/tmp/chain_file", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    check("create chain_file", fd >= 0);
    if (fd >= 0) {
        sc3(SYS_WRITE, fd, (long)"CHAIN", 5);
        sc1(SYS_CLOSE, fd);
    }

    r = sc2(SYS_SYMLINK, (long)"/tmp/chain_file", (long)"/tmp/chain_a");
    check_val("symlink chain_a -> chain_file", r, 0);

    r = sc2(SYS_SYMLINK, (long)"/tmp/chain_a", (long)"/tmp/chain_b");
    check_val("symlink chain_b -> chain_a", r, 0);

    r = sc2(SYS_SYMLINK, (long)"/tmp/chain_b", (long)"/tmp/chain_c");
    check_val("symlink chain_c -> chain_b", r, 0);

    fd = sc3(SYS_OPEN, (long)"/tmp/chain_c", O_RDONLY, 0);
    check("open through 3-level chain", fd >= 0);
    if (fd >= 0) {
        char buf[16] = {0};
        r = sc3(SYS_READ, fd, (long)buf, 5);
        check_val("read 5 bytes through chain", r, 5);
        check("chain data matches", buf[0]=='C' && buf[1]=='H' && buf[2]=='A');
        sc1(SYS_CLOSE, fd);
    }

    /* ── eloop: circular a <-> b ── */
    r = sc2(SYS_SYMLINK, (long)"/tmp/cloop_b", (long)"/tmp/cloop_a");
    check_val("symlink cloop_a -> cloop_b", r, 0);

    r = sc2(SYS_SYMLINK, (long)"/tmp/cloop_a", (long)"/tmp/cloop_b");
    check_val("symlink cloop_b -> cloop_a", r, 0);

    fd = sc3(SYS_OPEN, (long)"/tmp/cloop_a", O_RDONLY, 0);
    check("open circular -> error", fd < 0);

    /* ── lstat: returns S_IFLNK ── */
    struct k_stat st;
    r = sc2(SYS_LSTAT, (long)"/tmp/sym_link", (long)&st);
    check_val("lstat symlink ok", r, 0);
    check("lstat mode S_IFLNK", (st.st_mode & S_IFMT) == S_IFLNK);

    /* stat on same path follows the link */
    r = sc2(SYS_STAT, (long)"/tmp/sym_link", (long)&st);
    check_val("stat symlink ok", r, 0);
    check("stat mode S_IFREG (followed)", (st.st_mode & S_IFMT) == S_IFREG);

    /* ── unlink: remove symlink, target survives ── */
    r = sc1(SYS_UNLINK, (long)"/tmp/sym_link");
    check_val("unlink symlink", r, 0);

    fd = sc3(SYS_OPEN, (long)"/tmp/sym_target", O_RDONLY, 0);
    check("target still exists after unlink symlink", fd >= 0);
    if (fd >= 0) sc1(SYS_CLOSE, fd);

    fd = sc3(SYS_OPEN, (long)"/tmp/sym_link", O_RDONLY, 0);
    check("symlink gone after unlink", fd < 0);

    /* ── O_NOFOLLOW on symlink -> ELOOP ── */
    r = sc2(SYS_SYMLINK, (long)"/tmp/sym_target", (long)"/tmp/sym_nf");
    check_val("symlink for O_NOFOLLOW test", r, 0);

    fd = sc3(SYS_OPEN, (long)"/tmp/sym_nf", O_RDONLY | O_NOFOLLOW, 0);
    check("O_NOFOLLOW on symlink -> ELOOP", fd == -ELOOP);

    /* ── cleanup ── */
    sc1(SYS_UNLINK, (long)"/tmp/sym_target");
    sc1(SYS_UNLINK, (long)"/tmp/sym_nf");
    sc1(SYS_UNLINK, (long)"/tmp/chain_file");
    sc1(SYS_UNLINK, (long)"/tmp/chain_a");
    sc1(SYS_UNLINK, (long)"/tmp/chain_b");
    sc1(SYS_UNLINK, (long)"/tmp/chain_c");
    sc1(SYS_UNLINK, (long)"/tmp/cloop_a");
    sc1(SYS_UNLINK, (long)"/tmp/cloop_b");
}

TEST("SYMLINK", test_symlink);
