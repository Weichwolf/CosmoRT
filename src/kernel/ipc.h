/* IPC — Synchronous message passing + Notifications (seL4-style)
 *
 * Endpoints: send/recv points for synchronous IPC.
 * Notifications: async signal delivery (word-sized).
 * wait_any: block until any endpoint in a set has a message.
 */
#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include "config.h"

#define IPC_MAX_ENDPOINTS 64
#define IPC_MSG_WORDS     4    /* message payload: 4 x 64-bit words */

/* Endpoint states */
#define EP_FREE      0
#define EP_IDLE      1
#define EP_SEND_WAIT 2   /* sender blocked, waiting for receiver */
#define EP_RECV_WAIT 3   /* receiver blocked, waiting for sender */

/* IPC Message */
typedef struct {
    uint64_t words[IPC_MSG_WORDS];
    int      sender_pid;
} ipc_msg_t;

/* Endpoint */
typedef struct {
    int      state;
    int      owner_pid;     /* process that created this endpoint */
    ipc_msg_t msg;          /* buffered message */
    int      blocked_pid;   /* pid of blocked process (-1 if none) */
    /* Notification word (async) */
    uint64_t notify_word;   /* bitmask of pending notifications */
} ipc_endpoint_t;

/* Wait set for wait_any */
typedef struct {
    int ep_ids[16];         /* endpoint IDs to wait on */
    int count;
} ipc_wait_set_t;

/* Initialize IPC subsystem */
void ipc_init(void);

/* Create an endpoint. Returns endpoint ID, or -1. */
int ipc_create_endpoint(int owner_pid);

/* Synchronous send: blocks until receiver calls recv. */
int ipc_send(int ep_id, const ipc_msg_t *msg);

/* Synchronous recv: blocks until sender calls send. */
int ipc_recv(int ep_id, ipc_msg_t *msg);

/* Non-blocking try_recv. Returns 0 if message available, -1 if not. */
int ipc_try_recv(int ep_id, ipc_msg_t *msg);

/* Send notification (async, non-blocking). ORs bits into notify_word. */
int ipc_notify(int ep_id, uint64_t bits);

/* Wait on any endpoint in set. Returns index of ready endpoint. */
int ipc_wait_any(const ipc_wait_set_t *set, ipc_msg_t *msg);

#endif
