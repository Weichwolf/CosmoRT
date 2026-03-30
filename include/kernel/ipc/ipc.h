/* IPC — Synchronous message passing + Notifications (seL4-style) */
#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include "config.h"

#define IPC_MAX_ENDPOINTS 64
#define IPC_MSG_WORDS     4

#define EP_FREE      0
#define EP_IDLE      1
#define EP_SEND_WAIT 2
#define EP_RECV_WAIT 3

typedef struct {
    uint64_t words[IPC_MSG_WORDS];
    int      sender_pid;
} ipc_msg_t;

typedef struct {
    int      state;
    int      owner_pid;
    ipc_msg_t msg;
    int      blocked_pid;
    int      blocked_tid;
    uint64_t notify_word;
    uint64_t _lock;
} ipc_endpoint_t;

typedef struct {
    int ep_ids[16];
    int count;
} ipc_wait_set_t;

void ipc_init(void);

int ipc_create_endpoint(int owner_pid);

int ipc_send(int ep_id, const ipc_msg_t *msg);

int ipc_recv(int ep_id, ipc_msg_t *msg);

int ipc_try_recv(int ep_id, ipc_msg_t *msg);

int ipc_notify(int ep_id, uint64_t bits);

int ipc_wait_any(const ipc_wait_set_t *set, ipc_msg_t *msg);

#endif
