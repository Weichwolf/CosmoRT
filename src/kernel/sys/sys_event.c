/* CosmoRT — Event syscalls: pselect6, select, ppoll */

#include "internal.h"

long do_pselect6(int nfds, uint64_t *readfds, long a3, long a4, long a5, long num) {
    uint64_t *writefds = (uint64_t *)a3;
    uint64_t *exceptfds = (uint64_t *)a4;
    (void)exceptfds;

    if (nfds <= 0) return 0;
    if (nfds > 256) nfds = 256;

    int nwords = (nfds + 63) / 64;
    if (nwords > 4) nwords = 4;

    uint64_t kread[4] = {0}, kwrite[4] = {0};
    if (readfds && user_ok((uint64_t)readfds, (uint64_t)(nwords * 8)))
        copy_from_user(kread, readfds, (size_t)(nwords * 8));
    if (writefds && user_ok((uint64_t)writefds, (uint64_t)(nwords * 8)))
        copy_from_user(kwrite, writefds, (size_t)(nwords * 8));

    struct { int fd; short events; short revents; } pfd[256];
    int npfd = 0;
    for (int i = 0; i < nfds && npfd < 256; i++) {
        int in_read  = kread[i / 64]  & (1ULL << (i % 64));
        int in_write = kwrite[i / 64] & (1ULL << (i % 64));
        if (in_read || in_write) {
            pfd[npfd].fd = i;
            pfd[npfd].events = 0;
            if (in_read)  pfd[npfd].events |= 1;
            if (in_write) pfd[npfd].events |= 4;
            pfd[npfd].revents = 0;
            npfd++;
        }
    }
    if (npfd == 0) return 0;

    int timeout_ms = -1;
    if (num == 270 && a5) {
        struct { long sec; long nsec; } ts;
        if (user_ok(a5, 16)) {
            copy_from_user(&ts, (void *)a5, 16);
            timeout_ms = (int)(ts.sec * 1000 + ts.nsec / 1000000);
        }
    } else if (num == 23 && a5) {
        struct { long sec; long usec; } tv;
        if (user_ok(a5, 16)) {
            copy_from_user(&tv, (void *)a5, 16);
            timeout_ms = (int)(tv.sec * 1000 + tv.usec / 1000);
        }
    }

    long ret = do_poll(pfd, npfd, timeout_ms);

    uint64_t out_read[4] = {0}, out_write[4] = {0};
    int total_ready = 0;
    for (int i = 0; i < npfd; i++) {
        if (pfd[i].revents & 1) {
            out_read[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
            total_ready++;
        }
        if (pfd[i].revents & 4) {
            out_write[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
            total_ready++;
        }
        if (pfd[i].revents & 0x18) {
            out_read[pfd[i].fd / 64]  |= (1ULL << (pfd[i].fd % 64));
            out_write[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
            total_ready++;
        }
    }
    if (readfds && user_ok((uint64_t)readfds, (uint64_t)(nwords * 8)))
        copy_to_user(readfds, out_read, (size_t)(nwords * 8));
    if (writefds && user_ok((uint64_t)writefds, (uint64_t)(nwords * 8)))
        copy_to_user(writefds, out_write, (size_t)(nwords * 8));
    if (exceptfds && user_ok((uint64_t)exceptfds, (uint64_t)(nwords * 8))) {
        uint64_t zero[4] = {0};
        copy_to_user(exceptfds, zero, (size_t)(nwords * 8));
    }
    return total_ready > 0 ? (long)total_ready : (ret == 0 ? 0 : ret);
}

long do_ppoll(long a1, long a2, long a3) {
    int timeout_ms = -1;
    if (a3) {
        struct { long sec; long nsec; } ts;
        if (user_ok(a3, 16) && !copy_from_user(&ts, (void *)a3, 16))
            timeout_ms = (int)(ts.sec * 1000 + ts.nsec / 1000000);
    }
    return do_poll((void *)a1, (int)a2, timeout_ms);
}
