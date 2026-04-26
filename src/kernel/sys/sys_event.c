/* CosmoRT — Event syscalls: pselect6, select, poll, ppoll
 *
 * Linux file_operations.poll feeds wait queues into a poll_table; each
 * watched fd's wq gets a wait_queue_entry that links the polling thread.
 * On any fd's wake_up, the registered entry's func wakes the polling
 * thread (default_wake_function). We register one wait_queue_entry per
 * fd directly on the fd's wq, with priv = thread_current() and
 * func = default_wake_function. After schedule_timeout returns we
 * detach all entries.
 */

#include "internal.h"
#include "core/waitqueue.h"
#include "event/fd.h"

struct k_pollfd { int fd; short events; short revents; };
#define POLLIN  0x0001
#define POLLOUT 0x0004

#define POLL_MAX_FDS 256

static long poll_loop(struct k_pollfd *kfds, int nfds, void *fds_ptr,
                      int timeout, thread_t *t);

/* ── pselect6 / select → convert to poll ── */

long do_pselect6(int nfds, uint64_t *readfds, long a3, long a4, long a5, long num) {
    uint64_t *writefds = (uint64_t *)a3;
    uint64_t *exceptfds = (uint64_t *)a4;
    (void)exceptfds;

    if (nfds <= 0) return 0;
    if (nfds > POLL_MAX_FDS) nfds = POLL_MAX_FDS;

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

    struct k_pollfd pfd[POLL_MAX_FDS];
    int npfd = 0;
    for (int i = 0; i < nfds && npfd < POLL_MAX_FDS; i++) {
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

    int timeout_ms = -1;
    if (num == 270 && a5) {
        struct { long sec; long nsec; } ts;
        if (!user_ok(a5, 16) || copy_from_user(&ts, (void *)a5, 16)) return -EFAULT;
        timeout_ms = (int)(ts.sec * 1000 + ts.nsec / 1000000);
    } else if (num == 23 && a5) {
        struct { long sec; long usec; } tv;
        if (!user_ok(a5, 16) || copy_from_user(&tv, (void *)a5, 16)) return -EFAULT;
        timeout_ms = (int)(tv.sec * 1000 + tv.usec / 1000);
    }

    long ret = 0;
    if (npfd > 0) {
        thread_t *t = thread_current();
        if (!t) return -EFAULT;
        ret = poll_loop(pfd, npfd, 0, timeout_ms, t);
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

/* ── SYS_POLL (7) — generic FD polling ──
 *
 * Linux poll_wait registers one wait_queue_entry per fd on the fd's wq;
 * on any wake the polling thread wakes. We mirror that: per-call array of
 * wait_queue_entry_t, one per fd that has a wq, all priv=current thread,
 * func=default_wake_function. After the sleep we detach.
 *
 * Sources without wq (FD_PTY, FD_INOTIFY, FD_SERIAL, regular files) get no
 * push-wake; level-trigger still works because we re-scan on every spurious
 * wake. Pathological case: poll(infinite) on a pty with no other event —
 * blocks forever; same constraint as epoll. */

static long poll_block_one_round(thread_t *t,
                                 struct k_pollfd *kfds, int nfds,
                                 uint64_t timeout_ns) {
    /* Per-fd wait entries on each fd's wq + a single private wq the poller
     * sleeps on. wake propagates: source wq -> default_wake_function on the
     * registered entry -> try_to_wake_up(t, ...). The poller's wait then
     * sleeps on its own private wq; nothing actually wakes that wq, so we
     * sleep until try_to_wake_up flips state from BLOCKED to RUNNABLE,
     * at which point schedule() returns. */
    (void)t;

    /* Stack-bounded entries — POLL_MAX_FDS=256 means 256*sizeof(wait_queue_entry_t)
     * = 256*40 = 10 KB, fits in the 64 KB kernel stack. */
    wait_queue_entry_t entries[POLL_MAX_FDS];
    wait_queue_head_t *wqs[POLL_MAX_FDS];
    int nattach = 0;

    for (int i = 0; i < nfds; i++) {
        uint32_t want = 0;
        if (kfds[i].events & POLLIN)  want |= EPOLLIN;
        if (kfds[i].events & POLLOUT) want |= EPOLLOUT;
        wait_queue_head_t *wq = fd_get_poll_wq(kfds[i].fd, want);
        if (!wq) continue;
        entries[nattach].task  = thread_current();
        entries[nattach].flags = 0;
        entries[nattach].func  = default_wake_function;
        entries[nattach].next  = 0;
        entries[nattach].prev  = 0;
        wqs[nattach] = wq;
        add_wait_queue(wq, &entries[nattach]);
        nattach++;
    }

    /* Private wq solely for schedule_timeout_interruptible's prepare_to_wait
     * mechanics; the actual wakers are on the per-fd wqs above which call
     * default_wake_function -> try_to_wake_up directly on this thread. */
    wait_queue_head_t priv_wq;
    init_waitqueue_head(&priv_wq);
    DEFINE_WAIT(priv_wait);

    long rc = schedule_timeout_interruptible(&priv_wq, &priv_wait, timeout_ns);

    for (int i = 0; i < nattach; i++) remove_wait_queue(wqs[i], &entries[i]);
    return rc;
}

static long poll_loop(struct k_pollfd *kfds, int nfds, void *fds_ptr,
                      int timeout, thread_t *t) {
    int infinite = (timeout < 0);
    uint64_t deadline = infinite ? 0 : timer_ms() + (uint64_t)timeout;

    for (;;) {
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
        if (timeout == 0) return 0;
        if (!infinite && timer_ms() >= deadline) return 0;

        uint64_t timeout_ns = 0;
        if (!infinite) {
            uint64_t now = timer_ms();
            if (now >= deadline) return 0;
            timeout_ns = (deadline - now) * 1000000ULL;
        }

        long sr = poll_block_one_round(t, kfds, nfds, timeout_ns);
        if (sr == -4)  return -EINTR;
        /* -110 = ETIMEDOUT: re-scan once more then return whatever we find. */
        if (sr == -110) {
            int ready2 = 0;
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
                if (kfds[i].revents) ready2++;
            }
            if (ready2 > 0 && fds_ptr)
                copy_to_user(fds_ptr, kfds, (size_t)nfds * sizeof(struct k_pollfd));
            return ready2;
        }
        /* spurious wake: re-loop and re-scan */
    }
}

long do_poll(void *fds_ptr, int nfds, int timeout) {
    if (nfds <= 0 || nfds > POLL_MAX_FDS) return -EINVAL;
    struct k_pollfd kfds[POLL_MAX_FDS];
    { int r = copy_from_user(kfds, fds_ptr, (size_t)nfds * sizeof(struct k_pollfd)); if (r) return r; }

    thread_t *t = thread_current();
    if (!t) return -EFAULT;

    return poll_loop(kfds, nfds, fds_ptr, timeout, t);
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
