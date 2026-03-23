#include "ktest.h"

#define AF_UNIX 1
#define SOCK_STREAM 1

/* msghdr for sendmsg/recvmsg */
struct k_iovec { const void *iov_base; size_t iov_len; };
struct k_msghdr {
    void       *msg_name;
    uint32_t    msg_namelen;
    uint32_t    _pad0;
    struct k_iovec *msg_iov;
    uint64_t    msg_iovlen;
    void       *msg_control;
    uint64_t    msg_controllen;
    int         msg_flags;
    int         _pad1;
};

static void test_unix_socket(void) {
    puts("\n[AF_UNIX]\n");

    /* ── socketpair ─────────────────────────────── */
    int sv[2] = { -1, -1 };
    long r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv);
    check_val("socketpair returns 0", r, 0);
    check("socketpair sv[0] >= 0", sv[0] >= 0);
    check("socketpair sv[1] >= 0", sv[1] >= 0);
    check("socketpair sv[0] != sv[1]", sv[0] != sv[1]);

    /* ── write sv[0] → read sv[1] ──────────────── */
    const char *msg = "Hello Unix!";
    long w = sc3(SYS_WRITE, sv[0], (long)msg, 11);
    check_val("write 11 bytes to sv[0]", w, 11);

    char rbuf[32] = {0};
    long rd = sc3(SYS_READ, sv[1], (long)rbuf, 32);
    check_val("read from sv[1] returns 11", rd, 11);
    check("read data matches 'H'", rbuf[0] == 'H');
    check("read data matches 'e'", rbuf[1] == 'e');
    check("read data matches '!'", rbuf[10] == '!');

    /* ── write sv[1] → read sv[0] (bidirectional) ─ */
    const char *reply = "OK";
    w = sc3(SYS_WRITE, sv[1], (long)reply, 2);
    check_val("write 2 bytes to sv[1]", w, 2);

    char rbuf2[8] = {0};
    rd = sc3(SYS_READ, sv[0], (long)rbuf2, 8);
    check_val("read from sv[0] returns 2", rd, 2);
    check("bidirectional data 'O'", rbuf2[0] == 'O');
    check("bidirectional data 'K'", rbuf2[1] == 'K');

    /* ── sendmsg/recvmsg ────────────────────────── */
    const char *smsg = "sendmsg!";
    struct k_iovec siov = { smsg, 8 };
    struct k_msghdr shdr = { NULL, 0, 0, &siov, 1, NULL, 0, 0, 0 };
    long sm = sc3(SYS_SENDMSG, sv[0], (long)&shdr, 0);
    check_val("sendmsg returns 8", sm, 8);

    char rmbuf[16] = {0};
    struct k_iovec riov = { rmbuf, 16 };
    struct k_msghdr rhdr = { NULL, 0, 0, &riov, 1, NULL, 0, 0, 0 };
    long rm = sc3(SYS_RECVMSG, sv[1], (long)&rhdr, 0);
    check_val("recvmsg returns 8", rm, 8);
    check("recvmsg data 's'", rmbuf[0] == 's');
    check("recvmsg data '!'", rmbuf[7] == '!');

    /* ── close ──────────────────────────────────── */
    sc1(SYS_CLOSE, sv[0]);
    sc1(SYS_CLOSE, sv[1]);

    /* ── EOF after peer close ───────────────────── */
    int sv2[2] = { -1, -1 };
    r = sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0, (long)sv2);
    check_val("socketpair #2 returns 0", r, 0);

    sc1(SYS_CLOSE, sv2[0]); /* close writer */
    char eof_buf[4];
    rd = sc3(SYS_READ, sv2[1], (long)eof_buf, 4);
    check_val("read after peer close = 0 (EOF)", rd, 0);
    sc1(SYS_CLOSE, sv2[1]);

    /* ── socket(AF_UNIX) alone ──────────────────── */
    long sfd = sc3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    check("socket(AF_UNIX) >= 0", sfd >= 0);
    if (sfd >= 0) sc1(SYS_CLOSE, sfd);
}

TEST("af_unix", test_unix_socket);
