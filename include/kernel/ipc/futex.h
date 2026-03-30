/* CosmoRT Futex — Fast Userspace Mutex with Priority Inheritance */
#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>

struct timespec;

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_FD            2
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_PRIVATE_FLAG  128

#define FUTEX_TID_MASK      0x3FFFFFFFU
#define FUTEX_OWNER_DIED    0x40000000U
#define FUTEX_WAITERS       0x80000000U

void futex_init(void);

long do_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3);

#endif
