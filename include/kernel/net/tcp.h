/* CosmoRT TCP — Per-Socket Ringbuffer, State-Machine (RFC 793) */
#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "config.h"
#include "core/mutex.h"

struct thread;

#define NET_TCP_MAX       256
#define NET_TCP_OOO_SLOTS 4
#define NET_TFO_CACHE_MAX 64

enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

typedef struct {
    uint8_t  *buf;
    uint32_t head, tail;
    mutex_t lock;
} tcp_rxring_t;

int      rxring_init(tcp_rxring_t *r);
void     rxring_destroy(tcp_rxring_t *r);
int      rxring_push(tcp_rxring_t *r, const void *data, int len);
int      rxring_pop(tcp_rxring_t *r, void *buf, int len);
int      rxring_used(const tcp_rxring_t *r);
int      rxring_free(const tcp_rxring_t *r);

typedef struct net_tcp {
    uint8_t  dst_ip[4];
    uint8_t  dst_mac[6];
    uint16_t local_port, remote_port;

    uint32_t snd_nxt, snd_una;
    uint32_t rcv_nxt;
    uint32_t snd_wnd, rcv_wnd;

    enum tcp_state state;

    tcp_rxring_t rx;

    struct {
        uint32_t seq;
        uint16_t len;
        uint8_t  data[1460];
    } ooo[NET_TCP_OOO_SLOTS];
    int ooo_count;

    uint32_t cwnd;
    uint32_t ssthresh;
    uint16_t dup_ack_count;
    uint8_t  in_recovery;
    uint32_t recovery_seq;
    uint64_t cubic_t_epoch;
    uint32_t cubic_w_max;
    uint32_t cubic_w_last;
    uint32_t cubic_k;

    struct { uint32_t left, right; } sack_blocks[4];
    int      sack_count;

    uint8_t  snd_wscale;
    uint8_t  rcv_wscale;
    uint8_t  wscale_ok;

    uint64_t rto_ms, last_send_ms;

    uint8_t got_fin, got_rst;

    uint64_t rcv_timeo_ms;
    uint64_t snd_timeo_ms;

    uint8_t  keepalive;
    uint8_t  keepalive_probes;
    uint64_t keepalive_next;

    int8_t   connect_err;

    uint8_t  ts_enabled;
    uint32_t ts_recent;
    uint64_t ts_recent_age;

    uint8_t  ecn_enabled;
    uint8_t  ecn_ce_pending;
    uint8_t  ecn_cwr_sent;

    uint8_t  tfo_enabled;
    uint8_t  tfo_cookie[16];
    uint8_t  tfo_cookie_len;

    struct thread *wait_thread;

    struct net_tcp *hash_next;
} net_tcp_t;

typedef struct {
    uint8_t  ip[4];
    uint8_t  cookie[16];
    uint8_t  cookie_len;
} tfo_cache_entry_t;

int  tfo_cache_lookup(const uint8_t *ip, uint8_t *cookie_out, uint8_t *len_out);
void tfo_cache_store(const uint8_t *ip, const uint8_t *cookie, uint8_t len);

void     tcp_register(net_tcp_t *c);
void     tcp_unregister(net_tcp_t *c);
net_tcp_t *tcp_find(uint16_t local_port, uint16_t remote_port, const uint8_t *src_ip);

int  net_tcp_connect(net_tcp_t *c, const uint8_t *dst_ip, uint16_t port);
int  net_tcp_accept(net_tcp_t *c, uint16_t local_port, int timeout_ms);
int  net_tcp_send(net_tcp_t *c, const void *data, int len);
int  net_tcp_recv(net_tcp_t *c, void *buf, int bufsize, int timeout_ms);
void net_tcp_close(net_tcp_t *c);

void tcp_input(const uint8_t *pkt, int len);

void net_send_raw(const uint8_t *data, uint16_t len);
void net_build_ip_hdr(uint8_t *pkt, const uint8_t *dst_mac,
                      const uint8_t *dst_ip, uint8_t proto, uint16_t plen);
void net_build_ip_hdr_tos(uint8_t *pkt, const uint8_t *dst_mac,
                          const uint8_t *dst_ip, uint8_t proto,
                          uint16_t plen, uint8_t tos);
int  net_arp_resolve(const uint8_t *ip, uint8_t *mac_out);

#endif
