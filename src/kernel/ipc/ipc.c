/* CosmoRT IPC — Synchronous message passing + Notifications
 *
 * Per-endpoint locks for scalability (no global contention).
 * Global lock only for endpoint allocation (rare path).
 */

#include "ipc/ipc.h"
#include "proc/process.h"
#include "core/percpu.h"
#include "core/event_queue.h"
#include "hw/serial.h"
#include "spinlock.h"
#include "proc/thread.h"
#include "arch/arch.h"

extern void sched_add(thread_t *t);

/* Thread lookup — delegated to process.c O(1) tid_table */

/* Wake a thread blocked on IPC receive via event_post.
 * event_post writes EQ_IPC_MSG into the thread's event queue,
 * then calls sched_wake (CAS BLOCKED→RUNNABLE). Race-free. */
static void ipc_wake_receiver(ipc_endpoint_t *ep) {
    if (ep->blocked_tid > 0) {
        thread_t *t = thread_find_by_tid(ep->blocked_tid);
        if (t)
            event_post(t, EQ_IPC_MSG, (uint64_t)0);
        ep->blocked_tid = -1;
    }
    ep->blocked_pid = -1;
}

static ipc_endpoint_t endpoints[IPC_MAX_ENDPOINTS];
static spinlock_t ipc_alloc_lock = SPINLOCK_INIT; /* only for create */

/* Per-endpoint lock helpers (cast _lock field to spinlock_t*) */
static inline spinlock_t *ep_lock(ipc_endpoint_t *ep) {
    return (spinlock_t *)&ep->_lock;
}

void ipc_init(void) {
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        endpoints[i].state = EP_FREE;
        endpoints[i].owner_pid = -1;
        endpoints[i].blocked_pid = -1;
        endpoints[i].blocked_tid = -1;
        endpoints[i].notify_word = 0;
        endpoints[i]._lock = 0;
    }
    serial_puts("IPC: init\n");
}

int ipc_create_endpoint(int owner_pid) {
    uint64_t flags;
    spin_lock_irq(&ipc_alloc_lock, &flags);
    for (int i = 0; i < IPC_MAX_ENDPOINTS; i++) {
        if (endpoints[i].state == EP_FREE) {
            endpoints[i].state = EP_IDLE;
            endpoints[i].owner_pid = owner_pid;
            endpoints[i].blocked_pid = -1;
            endpoints[i].blocked_tid = -1;
            endpoints[i].notify_word = 0;
            spin_unlock_irq(&ipc_alloc_lock, flags);
            return i;
        }
    }
    spin_unlock_irq(&ipc_alloc_lock, flags);
    return -1;
}

int ipc_send(int ep_id, const ipc_msg_t *msg) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;

    ipc_endpoint_t *ep = &endpoints[ep_id];
    uint64_t flags;
    spin_lock_irq(ep_lock(ep), &flags);

    if (ep->state == EP_FREE) {
        spin_unlock_irq(ep_lock(ep), flags);
        return -1;
    }

    if (ep->state == EP_RECV_WAIT) {
        ep->msg = *msg;
        ep->state = EP_IDLE;
        ipc_wake_receiver(ep);
        spin_unlock_irq(ep_lock(ep), flags);
        return 0;
    }

    ep->msg = *msg;
    ep->state = EP_SEND_WAIT;
    process_t *cur = proc_current();
    if (cur) ep->blocked_pid = (int)cur->pid;
    spin_unlock_irq(ep_lock(ep), flags);
    return 0;
}

int ipc_recv(int ep_id, ipc_msg_t *msg) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;

    ipc_endpoint_t *ep = &endpoints[ep_id];
    uint64_t flags;
    spin_lock_irq(ep_lock(ep), &flags);

    if (ep->state == EP_FREE) {
        spin_unlock_irq(ep_lock(ep), flags);
        return -1;
    }

    if (ep->notify_word) {
        msg->words[0] = ep->notify_word;
        msg->words[1] = msg->words[2] = msg->words[3] = 0;
        msg->sender_pid = -1;
        ep->notify_word = 0;
        spin_unlock_irq(ep_lock(ep), flags);
        return 0;
    }

    if (ep->state == EP_SEND_WAIT) {
        *msg = ep->msg;
        ep->state = EP_IDLE;
        ep->blocked_pid = -1;
        spin_unlock_irq(ep_lock(ep), flags);
        return 0;
    }

    /* Record blocked info and set state */
    ep->state = EP_RECV_WAIT;
    process_t *cur = proc_current();
    if (cur) ep->blocked_pid = (int)cur->pid;
    thread_t *ct = thread_current();
    if (ct) ep->blocked_tid = ct->tid;
    spin_unlock_irq(ep_lock(ep), flags);

    /* Block via event_wait — ipc_send/ipc_notify will event_post us.
     * Thread context: event_wait blocks (never returns, syscall restarts).
     * On restart, ipc_recv re-executes and finds the message.
     * No thread context (boot): fall back to spin + EAGAIN. */
    if (ct) {
        event_t ev;
        event_wait(&ct->eq, &ev, -1);
        /* unreachable — syscall restarts, re-enters ipc_recv */
    }
    return -EAGAIN;
}

int ipc_try_recv(int ep_id, ipc_msg_t *msg) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;

    ipc_endpoint_t *ep = &endpoints[ep_id];
    uint64_t flags;
    spin_lock_irq(ep_lock(ep), &flags);

    if (ep->notify_word) {
        msg->words[0] = ep->notify_word;
        msg->words[1] = msg->words[2] = msg->words[3] = 0;
        msg->sender_pid = -1;
        ep->notify_word = 0;
        spin_unlock_irq(ep_lock(ep), flags);
        return 0;
    }

    if (ep->state == EP_SEND_WAIT) {
        *msg = ep->msg;
        ep->state = EP_IDLE;
        ep->blocked_pid = -1;
        spin_unlock_irq(ep_lock(ep), flags);
        return 0;
    }

    spin_unlock_irq(ep_lock(ep), flags);
    return -1;
}

int ipc_notify(int ep_id, uint64_t bits) {
    if (ep_id < 0 || ep_id >= IPC_MAX_ENDPOINTS) return -1;
    ipc_endpoint_t *ep = &endpoints[ep_id];
    if (ep->state == EP_FREE) return -1;

    uint64_t flags;
    spin_lock_irq(ep_lock(ep), &flags);
    ep->notify_word |= bits;

    if (ep->state == EP_RECV_WAIT) {
        ep->state = EP_IDLE;
        ipc_wake_receiver(ep);
    }
    spin_unlock_irq(ep_lock(ep), flags);
    return 0;
}

int ipc_wait_any(const ipc_wait_set_t *set, ipc_msg_t *msg) {
    /* Fast path: scan all endpoints for a ready message */
    for (int i = 0; i < set->count; i++) {
        if (ipc_try_recv(set->ep_ids[i], msg) == 0)
            return i;
    }

    /* No message available — register this thread on all endpoints
     * and block via event_wait. On wake (from any endpoint's sender),
     * the syscall restarts and the fast-path scan finds the message. */
    thread_t *ct = thread_current();
    if (!ct) return -1;

    for (int i = 0; i < set->count; i++) {
        int id = set->ep_ids[i];
        if (id < 0 || id >= IPC_MAX_ENDPOINTS) continue;
        ipc_endpoint_t *ep = &endpoints[id];
        uint64_t flags;
        spin_lock_irq(ep_lock(ep), &flags);
        if (ep->state != EP_FREE && ep->blocked_tid <= 0) {
            ep->state = EP_RECV_WAIT;
            ep->blocked_tid = ct->tid;
            process_t *cur = proc_current();
            if (cur) ep->blocked_pid = (int)cur->pid;
        }
        spin_unlock_irq(ep_lock(ep), flags);
    }

    event_t ev;
    event_wait(&ct->eq, &ev, -1);
    /* unreachable — syscall restarts, re-enters ipc_wait_any */

    return -1;
}
