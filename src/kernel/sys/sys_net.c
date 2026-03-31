/* CosmoRT — Network/socket syscalls: sendmsg, recvmsg, sendmmsg, recvmmsg */

#include "internal.h"

/* ── Cold-path error helper ── */

__attribute__((cold))
static void sendmmsg_error(long err) {
    serial_puts("sendmmsg: err=");
    serial_hex64((uint64_t)err);
    serial_putchar('\n');
}

/* ── SYS_SENDMSG (46) ── */

long do_sendmsg(int fd, const void *msg, int flags) {
    process_t *sp = proc_current();
    if (sp) {
        fd_entry_t *sfe = fd_get(&sp->fds, fd);
        if (sfe && sfe->type == FD_SOCKET) {
            /* Parse msghdr */
            struct { uint64_t name; uint32_t namelen; uint32_t _pad;
                     uint64_t iov; uint64_t iovlen;
                     uint64_t control; uint64_t controllen; int flags; } mh;
            if (copy_from_user(&mh, (void *)msg, sizeof(mh))) return -EFAULT;
            /* Get first iovec */
            struct { uint64_t base; uint64_t len; } iov1 = {0, 0};
            if (mh.iovlen > 0 && mh.iov)
                copy_from_user(&iov1, (void *)mh.iov, sizeof(iov1));
            return do_sendto(fd, (void *)iov1.base, (long)iov1.len,
                             flags, (void *)mh.name, (int)mh.namelen);
        }
    }
    return usock_sendmsg(fd, (const void *)msg, flags);
}

/* ── SYS_RECVMSG (47) ── */

long do_recvmsg(int fd, void *msg, int flags) {
    process_t *sp = proc_current();
    if (sp) {
        fd_entry_t *sfe = fd_get(&sp->fds, fd);
        if (sfe && sfe->type == FD_SOCKET) {
            struct { uint64_t name; uint32_t namelen; uint32_t _pad;
                     uint64_t iov; uint64_t iovlen;
                     uint64_t control; uint64_t controllen; int flags; } mh;
            if (copy_from_user(&mh, (void *)msg, sizeof(mh))) return -EFAULT;
            struct { uint64_t base; uint64_t len; } iov1 = {0, 0};
            if (mh.iovlen > 0 && mh.iov)
                copy_from_user(&iov1, (void *)mh.iov, sizeof(iov1));
            /* Pass src_addr pointer from msghdr. namelen is a value in msghdr,
             * but do_recvfrom needs a user-pointer to write back the length.
             * Compute the address of namelen within the user msghdr struct. */
            int *user_namelen_ptr = mh.name ? (int *)((uint64_t)msg + 8) : 0;
            return do_recvfrom(fd, (void *)iov1.base, (long)iov1.len,
                               flags, (void *)mh.name, user_namelen_ptr);
        }
    }
    return usock_recvmsg(fd, (void *)msg, flags);
}

/* ── SYS_SENDMMSG (307) ── */

long do_sendmmsg(int fd, uint64_t mmsg_arr, int vlen, int flags) {
    /* struct mmsghdr { struct msghdr hdr; unsigned int len; }; — 64 bytes */
    (void)flags;
    if (vlen <= 0) return 0;
    if (vlen > 16) vlen = 16; /* cap */

    process_t *mp = proc_current();
    if (!mp) return -EFAULT;
    fd_entry_t *mfe = fd_get(&mp->fds, fd);
    int sent = 0;
    for (int mi = 0; mi < vlen; mi++) {
        /* Each mmsghdr = msghdr(56 bytes) + msg_len(4 bytes) */
        uint64_t mhdr_addr = mmsg_arr + (uint64_t)mi * 64;
        struct { uint64_t name; uint32_t namelen; uint32_t _pad;
                 uint64_t iov; uint64_t iovlen;
                 uint64_t control; uint64_t controllen; int flags; } mh;
        if (copy_from_user(&mh, (void *)mhdr_addr, sizeof(mh))) break;
        struct { uint64_t base; uint64_t len; } iov1 = {0, 0};
        if (mh.iovlen > 0 && mh.iov)
            copy_from_user(&iov1, (void *)mh.iov, sizeof(iov1));
        long r;
        if (mfe && mfe->type == FD_SOCKET)
            r = do_sendto(fd, (void *)iov1.base, (long)iov1.len,
                          0, (void *)mh.name, (int)mh.namelen);
        else
            r = usock_sendmsg(fd, (void *)mhdr_addr, 0);
        if (__builtin_expect(r < 0, 0)) { sendmmsg_error(r); break; }
        /* Write msg_len back */
        uint32_t msg_len = (uint32_t)r;
        copy_to_user((void *)(mhdr_addr + 56), &msg_len, 4);
        sent++;
    }
    return sent > 0 ? sent : -EFAULT;
}

/* ── SYS_RECVMMSG (299) ── */

long do_recvmmsg(int fd, uint64_t mmsg_arr, int vlen, int flags) {
    if (vlen <= 0 || vlen > 1024) return -EINVAL;
    int received = 0;
    for (int i = 0; i < vlen; i++) {
        uint64_t mhdr_addr = mmsg_arr + (uint64_t)i * 64;
        if (!user_ok(mhdr_addr, 64)) break;
        /* Parse msghdr: same as recvmsg */
        struct { uint64_t name; uint32_t namelen; uint32_t _pad;
                 uint64_t iov; uint64_t iovlen;
                 uint64_t control; uint64_t controllen; int flags; } mh;
        if (copy_from_user(&mh, (void *)mhdr_addr, sizeof(mh))) break;
        struct { uint64_t base; uint64_t len; } iov1 = {0, 0};
        if (mh.iovlen > 0 && mh.iov)
            copy_from_user(&iov1, (void *)mh.iov, sizeof(iov1));
        int *user_namelen_ptr = mh.name ? (int *)((uint64_t)mhdr_addr + 8) : 0;
        long r = do_recvfrom(fd, (void *)iov1.base, (long)iov1.len,
                             flags, (void *)mh.name, user_namelen_ptr);
        if (r < 0) { if (received == 0) return r; break; }
        uint32_t msg_len = (uint32_t)r;
        copy_to_user((void *)(mhdr_addr + 56), &msg_len, 4);
        received++;
    }
    return received;
}
