/* CosmoRT signalfd — intentionally unimplemented
 *
 * signalfd requires deep integration with the signal delivery path:
 * pending signals must be consumed by read() instead of delivered to
 * handlers. Complexity outweighs benefit — programs fall back to
 * sigaction/sigwaitinfo. Node.js does not require signalfd.
 */

#include "epoll.h"

long do_signalfd4(int fd, const uint64_t *mask, int flags) {
    (void)fd; (void)mask; (void)flags;
    return -ENOSYS;
}
