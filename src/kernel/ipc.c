/* CosmoRT IPC — Synchronous message passing + Notifications
 *
 * Thread-aware: blocking/unblocking operates on threads via scheduler.
 * Spinlock-protected for SMP safety.
 */

#include "ipc.h"
#include "process.h"
#include "percpu.h"
#include "serial.h"
#include "spinlock.h"

static ipc_endpoint_t endpoints[IPC_MAX_ENDPOINTS];
static spinlock_t ipc_lock = SPINLOCK_INIT;

void ipc_init(void) {
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        endpoints[i].state = EP_FREE;
        endpoints[i].owner_pid = -1;
        endpoints[i].blocked_pid = -1;
        endpoints[i].notify_word = 0;
    }
    serial_puts("IPC: init\n");
}

int ipc_create_endpoint(int owner_pid) {
    uint64_t flags;
    spin_lock_irq(&ipc_lock, &flags);
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        if (endpoints[i].state == EP_FREE) {
            endpoints[i].state = EP_IDLE;
            endpoints[i].owner_pid = owner_pid;
            endpoints[i].blocked_pid = -1;
            endpoints[i].notify_word = 0;
            spin_unlock_irq(&ipc_lock, flags);
            return i;
        }
    }
    spin_unlock_irq(&ipc_lock, flags);
    return -1;
}

int ipc_send(int ep_id, const ipc_msg_t *msg) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;

    uint64_t flags;
    spin_lock_irq(&ipc_lock, &flags);

    ipc_endpoint_t *ep = &endpoints[ep_id];
    if (ep->state == EP_FREE) {
        spin_unlock_irq(&ipc_lock, flags);
        return -1;
    }

    if (ep->state == EP_RECV_WAIT && ep->blocked_pid >= 0) {
        ep->msg = *msg;
        ep->state = EP_IDLE;
        /* TODO: unblock waiting thread via scheduler */
        ep->blocked_pid = -1;
        spin_unlock_irq(&ipc_lock, flags);
        return 0;
    }

    ep->msg = *msg;
    ep->state = EP_SEND_WAIT;
    process_t *cur = proc_current();
    if (cur) ep->blocked_pid = (int)cur->pid;
    spin_unlock_irq(&ipc_lock, flags);
    return 0;
}

int ipc_recv(int ep_id, ipc_msg_t *msg) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;

    uint64_t flags;
    spin_lock_irq(&ipc_lock, &flags);

    ipc_endpoint_t *ep = &endpoints[ep_id];
    if (ep->state == EP_FREE) {
        spin_unlock_irq(&ipc_lock, flags);
        return -1;
    }

    if (ep->notify_word) {
        msg->words[0] = ep->notify_word;
        msg->words[1] = msg->words[2] = msg->words[3] = 0;
        msg->sender_pid = -1;
        ep->notify_word = 0;
        spin_unlock_irq(&ipc_lock, flags);
        return 0;
    }

    if (ep->state == EP_SEND_WAIT) {
        *msg = ep->msg;
        ep->state = EP_IDLE;
        ep->blocked_pid = -1;
        spin_unlock_irq(&ipc_lock, flags);
        return 0;
    }

    ep->state = EP_RECV_WAIT;
    process_t *cur = proc_current();
    if (cur) ep->blocked_pid = (int)cur->pid;
    spin_unlock_irq(&ipc_lock, flags);
    return -2; /* would block */
}

int ipc_try_recv(int ep_id, ipc_msg_t *msg) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;

    uint64_t flags;
    spin_lock_irq(&ipc_lock, &flags);

    ipc_endpoint_t *ep = &endpoints[ep_id];

    if (ep->notify_word) {
        msg->words[0] = ep->notify_word;
        msg->words[1] = msg->words[2] = msg->words[3] = 0;
        msg->sender_pid = -1;
        ep->notify_word = 0;
        spin_unlock_irq(&ipc_lock, flags);
        return 0;
    }

    if (ep->state == EP_SEND_WAIT) {
        *msg = ep->msg;
        ep->state = EP_IDLE;
        ep->blocked_pid = -1;
        spin_unlock_irq(&ipc_lock, flags);
        return 0;
    }

    spin_unlock_irq(&ipc_lock, flags);
    return -1;
}

int ipc_notify(int ep_id, uint64_t bits) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;
    ipc_endpoint_t *ep = &endpoints[ep_id];
    if (ep->state == EP_FREE) return -1;

    __sync_fetch_and_or(&ep->notify_word, bits);

    if (ep->state == EP_RECV_WAIT) {
        ep->state = EP_IDLE;
        ep->blocked_pid = -1;
        /* TODO: unblock waiting thread */
    }
    return 0;
}

int ipc_wait_any(const ipc_wait_set_t *set, ipc_msg_t *msg) {
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < set->count; i++) {
            if (ipc_try_recv(set->ep_ids[i], msg) == 0)
                return i;
        }
        if (round == 0) __asm__ volatile("pause");
    }
    return -1;
}
