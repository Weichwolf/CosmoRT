/* CosmoRT — Event syscalls: pselect6, select, ppoll */

#include "internal.h"

/* ── pselect6 / select → convert to poll ── */

long do_pselect6(int nfds, uint64_t *readfds, long a3, long a4, long a5, long num) {
    (void)a3; (void)a4; /* writefds, exceptfds — ignored */
    /* Minimal: poll the fds in readfds with POLLIN */
    if (nfds <= 0) return 0;
    if (nfds > 64) nfds = 64;
    /* Build pollfd array on kernel stack */
    struct { int fd; short events; short revents; } pfd[64];
    int npfd = 0;
    for (int i = 0; i < nfds && npfd < 64; i++) {
        int in_read = readfds && user_ok((uint64_t)readfds, 8) &&
                      (readfds[i / 64] & (1ULL << (i % 64)));
        if (in_read) {
            pfd[npfd].fd = i;
            pfd[npfd].events = 1; /* POLLIN */
            pfd[npfd].revents = 0;
            npfd++;
        }
    }
    if (npfd == 0) return 0;
    /* Compute timeout in ms */
    int timeout_ms = -1;
    if (num == 270 && a5) { /* pselect6: timespec */
        struct { long sec; long nsec; } ts;
        if (user_ok(a5, 16)) {
            copy_from_user(&ts, (void *)a5, 16);
            timeout_ms = (int)(ts.sec * 1000 + ts.nsec / 1000000);
        }
    } else if (num == 23 && a5) { /* select: timeval */
        struct { long sec; long usec; } tv;
        if (user_ok(a5, 16)) {
            copy_from_user(&tv, (void *)a5, 16);
            timeout_ms = (int)(tv.sec * 1000 + tv.usec / 1000);
        }
    }
    long ret = do_poll(pfd, npfd, timeout_ms);
    /* Write back readfds */
    if (readfds && user_ok((uint64_t)readfds, (uint64_t)((nfds + 63) / 64 * 8))) {
        int nwords = (nfds + 63) / 64;
        uint64_t out[1] = {0};
        for (int i = 0; i < npfd; i++) {
            if (pfd[i].revents & 1)
                out[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
        }
        for (int w = 0; w < nwords && w < 1; w++)
            copy_to_user(&readfds[w], &out[w], 8);
    }
    return ret > 0 ? ret : 0;
}

/* ── ppoll (271) ── */

long do_ppoll(long a1, long a2, long a3) {
    int timeout_ms = -1;
    if (a3) { /* timespec *tmo */
        struct { long sec; long nsec; } ts;
        if (user_ok(a3, 16) && !copy_from_user(&ts, (void *)a3, 16))
            timeout_ms = (int)(ts.sec * 1000 + ts.nsec / 1000000);
    }
    return do_poll((void *)a1, (int)a2, timeout_ms);
}
