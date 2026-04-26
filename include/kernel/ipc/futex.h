/* CosmoRT Futex — Fast Userspace Mutex with Priority Inheritance
 *
 * Linux-compatible subset: FUTEX_WAIT, FUTEX_WAKE, FUTEX_LOCK_PI, FUTEX_UNLOCK_PI.
 * Blocking via per-bucket wait_queue_head_t + try_to_wake_up + syscall restart.
 * Timeout support: FUTEX_WAIT blocks for at most the specified duration.
 */
#ifndef FUTEX_H
#define FUTEX_H

#include <stdint.h>

struct timespec;

/* Futex operations (Linux-compatible) */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_FD            2
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_PRIVATE_FLAG  128

/* Futex userspace value: low 30 bits = owner TID, bit 31 = FUTEX_WAITERS */
#define FUTEX_TID_MASK      0x3FFFFFFFU
#define FUTEX_OWNER_DIED    0x40000000U
#define FUTEX_WAITERS       0x80000000U

void futex_init(void);

long do_futex(uint32_t *uaddr, int op, uint32_t val,
              const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3);

#endif
