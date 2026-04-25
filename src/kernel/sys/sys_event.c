/* CosmoRT — Event syscalls: pselect6, select, poll, ppoll */

#include "internal.h"
#include "core/event_queue.h"
#include "core/hrtimer.h"
#include "event/fd.h"

struct k_pollfd { int fd; short events; short revents; };
#define POLLIN  0x0001
#define POLLOUT 0x0004

extern void epoll_sleeper_add_ext(thread_t *t);
extern void epoll_sleeper_remove_ext(thread_t *t);

static long poll_loop_ns(struct k_pollfd *kfds, int nfds, void *fds_ptr,
                         int64_t timeout_ns, thread_t *t);

/* ── pselect6 / select → convert to poll ── */

long do_pselect6(int nfds, uint64_t *readfds, long a3, long a4, long a5, long num) {
    uint64_t *writefds = (uint64_t *)a3;
    uint64_t *exceptfds = (uint64_t *)a4;
    (void)exceptfds;

    if (nfds <= 0) return 0;
    if (nfds > 256) nfds = 256;

    int nwords = (nfds + 63) / 64;
    if (nwords > 4) nwords = 4;

    uint64_t kread[4] = {0}, kwrite[4] = {0};
    if (readfds) {
        if (!user_ok((uint64_t)readfds, (size_t)(nwords * 8)) ||
            copy_from_user(kread, readfds, (size_t)(nwords * 8)))
            return -EFAULT;
    }
    if (writefds) {
        if (!user_ok((uint64_t)writefds, (size_t)(nwords * 8)) ||
            copy_from_user(kwrite, writefds, (size_t)(nwords * 8)))
            return -EFAULT;
    }

    struct k_pollfd pfd[256];
    int npfd = 0;
    for (int i = 0; i < nfds && npfd < 256; i++) {
        int in_read  = kread[i / 64]  & (1ULL << (i % 64));
        int in_write = kwrite[i / 64] & (1ULL << (i % 64));
        if (in_read || in_write) {
            pfd[npfd].fd = i;
            pfd[npfd].events = 0;
            if (in_read)  pfd[npfd].events |= POLLIN;
            if (in_write) pfd[npfd].events |= POLLOUT;
            pfd[npfd].revents = 0;
            npfd++;
        }
    }

    int64_t timeout_ns = -1;
    if (num == 270 && a5) {
        struct { long sec; long nsec; } ts;
        if (!user_ok(a5, 16) || copy_from_user(&ts, (void *)a5, 16)) return -EFAULT;
        timeout_ns = (int64_t)ts.sec * NSEC_PER_SEC + (int64_t)ts.nsec;
    } else if (num == 23 && a5) {
        struct { long sec; long usec; } tv;
        if (!user_ok(a5, 16) || copy_from_user(&tv, (void *)a5, 16)) return -EFAULT;
        timeout_ns = (int64_t)tv.sec * NSEC_PER_SEC + (int64_t)tv.usec * (int64_t)NSEC_PER_USEC;
    }

    long ret = 0;
    if (npfd > 0) {
        thread_t *t = thread_current();
        if (!t) return -EFAULT;
        ret = poll_loop_ns(pfd, npfd, 0, timeout_ns, t);
        epoll_sleeper_remove_ext(t);
        t->wake_at = 0;
        if (ret < 0 && ret != -EINTR) return ret;
    }

    uint64_t out_read[4] = {0}, out_write[4] = {0}, out_except[4] = {0};
    int total_ready = 0;
    for (int i = 0; i < npfd; i++) {
        if (pfd[i].revents & POLLIN) {
            out_read[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
            total_ready++;
        }
        if (pfd[i].revents & POLLOUT) {
            out_write[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
            total_ready++;
        }
        if (pfd[i].revents & 0x18) {
            out_read[pfd[i].fd / 64]  |= (1ULL << (pfd[i].fd % 64));
            out_write[pfd[i].fd / 64] |= (1ULL << (pfd[i].fd % 64));
            total_ready++;
        }
    }
    if (readfds && copy_to_user(readfds, out_read, (size_t)(nwords * 8)))
        return -EFAULT;
    if (writefds && copy_to_user(writefds, out_write, (size_t)(nwords * 8)))
        return -EFAULT;
    if (exceptfds && user_ok((uint64_t)exceptfds, (size_t)(nwords * 8))) {
        if (copy_to_user(exceptfds, out_except, (size_t)(nwords * 8)))
            return -EFAULT;
    }
    if (ret == -EINTR) return -EINTR;
    return total_ready;
}

/* ── SYS_POLL (7) — generic FD polling ── */

static long poll_loop_ns(struct k_pollfd *kfds, int nfds, void *fds_ptr,
                         int64_t timeout_ns, thread_t *t) {
    int infinite = (timeout_ns < 0);
    uint64_t deadline_ns = infinite ? 0 : hrtimer_now_ns() + (uint64_t)timeout_ns;

    for (;;) {
        t->wake_at = infinite ? 0 : (deadline_ns / NSEC_PER_MSEC);
        epoll_sleeper_add_ext(t);

        int ready = 0;
        for (int i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            uint32_t interest = 0;
            if (kfds[i].events & POLLIN)  interest |= 0x001;
            if (kfds[i].events & POLLOUT) interest |= 0x004;
            uint32_t r_ev = fd_poll_readiness(kfds[i].fd, interest);
            if (r_ev & 0x001) kfds[i].revents |= POLLIN;
            if (r_ev & 0x004) kfds[i].revents |= POLLOUT;
            if (r_ev & 0x010) kfds[i].revents |= 0x0010;
            if (r_ev & 0x008) kfds[i].revents |= 0x0008;
            if (kfds[i].revents) ready++;
        }
        if (ready > 0) {
            if (fds_ptr)
                copy_to_user(fds_ptr, kfds, (size_t)nfds * sizeof(struct k_pollfd));
            return ready;
        }
        if (timeout_ns == 0) return 0;
        if (!infinite && hrtimer_now_ns() >= deadline_ns) return 0;

        uint64_t now_ns = hrtimer_now_ns();
        int64_t remaining_ns = infinite ? -1
            : (int64_t)(deadline_ns > now_ns ? deadline_ns - now_ns : 0);
        if (remaining_ns == 0) return 0;
        event_t ev;
        int wr = event_wait_ns(&t->eq, &ev, remaining_ns);
        if (wr == -4) return -EINTR;
    }
}

long do_poll(void *fds_ptr, int nfds, int timeout) {
    if (nfds <= 0 || nfds > 256) return -EINVAL;
    struct k_pollfd kfds[256];
    { int r = copy_from_user(kfds, fds_ptr, (size_t)nfds * sizeof(struct k_pollfd)); if (r) return r; }

    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    int64_t timeout_ns = (timeout < 0) ? -1 : (int64_t)timeout * (int64_t)NSEC_PER_MSEC;
    long ret = poll_loop_ns(kfds, nfds, fds_ptr, timeout_ns, t);
    epoll_sleeper_remove_ext(t);
    t->wake_at = 0;
    return ret;
}

/* ── ppoll (271) ── */

long do_ppoll(long a1, long a2, long a3) {
    int64_t timeout_ns = -1;
    if (a3) { /* timespec *tmo */
        struct { long sec; long nsec; } ts;
        if (user_ok(a3, 16) && !copy_from_user(&ts, (void *)a3, 16))
            timeout_ns = (int64_t)ts.sec * NSEC_PER_SEC + (int64_t)ts.nsec;
    }

    int nfds = (int)a2;
    if (nfds <= 0 || nfds > 256) return -EINVAL;
    struct k_pollfd kfds[256];
    void *fds_ptr = (void *)a1;
    { int r = copy_from_user(kfds, fds_ptr, (size_t)nfds * sizeof(struct k_pollfd));
      if (r) return r; }

    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    long ret = poll_loop_ns(kfds, nfds, fds_ptr, timeout_ns, t);
    epoll_sleeper_remove_ext(t);
    t->wake_at = 0;
    return ret;
}
