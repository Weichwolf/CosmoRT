/* CosmoRT signalfd — intentionally unimplemented */

#include "event/epoll.h"

long do_signalfd4(int fd, const uint64_t *mask, int flags) {
    (void)fd; (void)mask; (void)flags;
    return -ENOSYS;
}
