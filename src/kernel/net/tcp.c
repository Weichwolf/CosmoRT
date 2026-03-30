/* CosmoRT TCP — Per-Socket Ringbuffer, State-Machine, Hash-Lookup
 * CUBIC (RFC 8312), SACK (RFC 2018), Window Scaling (RFC 7323),
 * Fast Retransmit/Recovery (RFC 5681 §3.2), IW=10 (RFC 6928),
 * Timestamps/PAWS (RFC 7323), ECN (RFC 3168), TFO (RFC 7413). */

#include "net/tcp.h"
#include "net/net.h"
#include "net/net_util.h"
#include "core/timer.h"
#include "core/rt.h"
#include "mm/page_alloc.h"
#include "core/event_queue.h"

/* ── Constants ────────────────────────────────────── */

#define MSS           1460
#define CUBIC_C_1024  410    /* C=0.4 × 1024 */
#define CUBIC_BETA    717    /* β=0.7 × 1024 */
#define CUBIC_ONE_MINUS_BETA  307  /* (1-β)=0.3 × 1024 */
#define RCV_WSCALE    7      /* advertised: 2^7 = 128 → 128×64K = 8MB */

/* ── Ringbuffer ────────────────────────────────────── */

int rxring_init(tcp_rxring_t *r) {
    r->head = 0;
    r->tail = 0;
    r->lock = (spinlock_t)SPINLOCK_INIT;
    int npages = (NET_TCP_RXBUF + 4095) / 4096;
    r->buf = (uint8_t *)pages_alloc(npages);
    if (!r->buf) return -12; /* -ENOMEM */
    return 0;
}

void rxring_destroy(tcp_rxring_t *r) {
    if (r->buf) {
        int npages = (NET_TCP_RXBUF + 4095) / 4096;
        pages_free(r->buf, npages);
        r->buf = 0;
    }
    r->head = r->tail = 0;
}

int rxring_used(const tcp_rxring_t *r) {
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return (int)((t - h) & (NET_TCP_RXBUF - 1));
}

int rxring_free(const tcp_rxring_t *r) {
    return NET_TCP_RXBUF - 1 - rxring_used(r);
}

int rxring_push(tcp_rxring_t *r, const void *data, int len) {
    if (!r->buf) return 0;
    uint64_t flags;
    spin_lock_irq(&r->lock, &flags);
    int avail = NET_TCP_RXBUF - 1 - (int)((r->tail - r->head) & (NET_TCP_RXBUF - 1));
    if (len > avail) len = avail;
    if (len <= 0) { spin_unlock_irq(&r->lock, flags); return 0; }

    const uint8_t *src = data;
    uint32_t mask = NET_TCP_RXBUF - 1;
    for (int i = 0; i < len; i++)
        r->buf[(r->tail + (uint32_t)i) & mask] = src[i];
    r->tail = (r->tail + (uint32_t)len) & mask;
    spin_unlock_irq(&r->lock, flags);
    return len;
}

int rxring_pop(tcp_rxring_t *r, void *buf, int len) {
    if (!r->buf) return 0;
    uint64_t flags;
    spin_lock_irq(&r->lock, &flags);
    int used = (int)((r->tail - r->head) & (NET_TCP_RXBUF - 1));
    if (len > used) len = used;
    if (len <= 0) { spin_unlock_irq(&r->lock, flags); return 0; }

    uint8_t *dst = buf;
    uint32_t mask = NET_TCP_RXBUF - 1;
    for (int i = 0; i < len; i++)
        dst[i] = r->buf[(r->head + (uint32_t)i) & mask];
    r->head = (r->head + (uint32_t)len) & mask;
    spin_unlock_irq(&r->lock, flags);
    return len;
}

/* ── TCP Hash Table ────────────────────────────────── */

#define TCP_HASH_SIZE 256  /* must be power of 2 */

static net_tcp_t *tcp_hash[TCP_HASH_SIZE];
static spinlock_t tcp_hash_lock = SPINLOCK_INIT;

static uint32_t tcp_hash_fn(uint16_t lport, uint16_t rport, const uint8_t *sip) {
    uint32_t h = (uint32_t)lport ^ ((uint32_t)rport << 7);
    h ^= (uint32_t)sip[0] ^ ((uint32_t)sip[1] << 8) ^
         ((uint32_t)sip[2] << 16) ^ ((uint32_t)sip[3] << 24);
    h ^= h >> 16;
    return h & (TCP_HASH_SIZE - 1);
}

void tcp_register(net_tcp_t *c) {
    uint32_t idx = tcp_hash_fn(c->local_port, c->remote_port, c->dst_ip);
    uint64_t flags;
    spin_lock_irq(&tcp_hash_lock, &flags);
    c->hash_next = tcp_hash[idx];
    tcp_hash[idx] = c;
    spin_unlock_irq(&tcp_hash_lock, flags);
}

void tcp_unregister(net_tcp_t *c) {
    uint32_t idx = tcp_hash_fn(c->local_port, c->remote_port, c->dst_ip);
    uint64_t flags;
    spin_lock_irq(&tcp_hash_lock, &flags);
    net_tcp_t **pp = &tcp_hash[idx];
    while (*pp) {
        if (*pp == c) {
            *pp = c->hash_next;
            c->hash_next = 0;
            break;
        }
        pp = &(*pp)->hash_next;
    }
    spin_unlock_irq(&tcp_hash_lock, flags);
}

net_tcp_t *tcp_find(uint16_t local_port, uint16_t remote_port, const uint8_t *src_ip) {
    uint32_t idx = tcp_hash_fn(local_port, remote_port, src_ip);
    net_tcp_t *c = __atomic_load_n(&tcp_hash[idx], __ATOMIC_ACQUIRE);
    while (c) {
        if (c->local_port == local_port && c->remote_port == remote_port &&
            c->dst_ip[0] == src_ip[0] && c->dst_ip[1] == src_ip[1] &&
            c->dst_ip[2] == src_ip[2] && c->dst_ip[3] == src_ip[3])
            return c;
        c = c->hash_next;
    }
    return 0;
}

/* ── TFO Cookie Cache (RFC 7413) ──────────────────── */

static tfo_cache_entry_t tfo_cache[NET_TFO_CACHE_MAX];

int tfo_cache_lookup(const uint8_t *ip, uint8_t *cookie_out, uint8_t *len_out) {
    for (int i = 0; i < NET_TFO_CACHE_MAX; i++) {
        if (tfo_cache[i].cookie_len &&
            tfo_cache[i].ip[0] == ip[0] && tfo_cache[i].ip[1] == ip[1] &&
            tfo_cache[i].ip[2] == ip[2] && tfo_cache[i].ip[3] == ip[3]) {
            mcpy(cookie_out, tfo_cache[i].cookie, tfo_cache[i].cookie_len);
            *len_out = tfo_cache[i].cookie_len;
            return 0;
        }
    }
    return -1;
}

void tfo_cache_store(const uint8_t *ip, const uint8_t *cookie, uint8_t len) {
    if (len < 4 || len > 16) return;
    /* Overwrite existing entry for same IP, else use first empty, else slot 0 */
    int slot = -1;
    for (int i = 0; i < NET_TFO_CACHE_MAX; i++) {
        if (tfo_cache[i].cookie_len == 0) { if (slot < 0) slot = i; continue; }
        if (tfo_cache[i].ip[0] == ip[0] && tfo_cache[i].ip[1] == ip[1] &&
            tfo_cache[i].ip[2] == ip[2] && tfo_cache[i].ip[3] == ip[3]) {
            slot = i; break;
        }
    }
    if (slot < 0) slot = 0;
    mcpy(tfo_cache[slot].ip, ip, 4);
    mcpy(tfo_cache[slot].cookie, cookie, len);
    tfo_cache[slot].cookie_len = len;
}

/* ── TCP Checksum ──────────────────────────────────── */

static uint16_t tcp_cksum(const uint8_t *sip, const uint8_t *dip,
                           const uint8_t *tcp, int tlen) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i += 2)
        sum += (uint32_t)((sip[i] << 8) | sip[i + 1]);
    for (int i = 0; i < 4; i += 2)
        sum += (uint32_t)((dip[i] << 8) | dip[i + 1]);
    sum += 6;
    sum += (uint32_t)tlen;
    for (int i = 0; i < tlen; i += 2) {
        uint16_t w = (uint16_t)(tcp[i] << 8);
        if (i + 1 < tlen) w |= tcp[i + 1];
        sum += w;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ── Send TCP Segment (with options support) ──────── */

/* Build Timestamp option (10 bytes): NOP+NOP+Kind8+Len10+TSval+TSecr */
static int ts_build_opt(net_tcp_t *c, uint8_t *buf) {
    if (!c->ts_enabled) return 0;
    buf[0] = 1; buf[1] = 1; /* NOP NOP for alignment */
    buf[2] = 8; buf[3] = 10;
    put32(buf + 4, (uint32_t)timer_ms());
    put32(buf + 8, c->ts_recent);
    return 12;
}

static void send_tcp_opts(net_tcp_t *c, uint8_t flags,
                          const void *data, int dlen,
                          const uint8_t *opts, int optlen) {
    uint8_t pkt[1536];

    /* Append Timestamp option if enabled and not a SYN (SYN builds its own) */
    uint8_t ts_opt[12];
    int ts_len = 0;
    if (c->ts_enabled && !(flags & 0x02))
        ts_len = ts_build_opt(c, ts_opt);

    int total_optlen = optlen + ts_len;
    int thdr = 20 + total_optlen;
    while (thdr & 3) thdr++;
    int tt = thdr + dlen;
    mzero(pkt, 54 + total_optlen + dlen);

    /* ECN: set ECT(0) = 0x02 on outgoing TCP if ECN negotiated */
    uint8_t tos = 0;
    if (c->ecn_enabled && !(flags & 0x02)) /* not on SYN */
        tos = 0x02; /* ECT(0) */

    /* ECN TCP flags: ECE if CE received, CWR after cwnd reduction */
    if (c->ecn_enabled && !(flags & 0x02)) {
        if (c->ecn_ce_pending)   flags |= 0x40; /* ECE */
        if (c->ecn_cwr_sent)  { flags |= 0x80; c->ecn_cwr_sent = 0; }
    }

    net_build_ip_hdr_tos(pkt, c->dst_mac, c->dst_ip, 6, (uint16_t)tt, tos);
    uint8_t *t = pkt + 34;
    put16(t, c->local_port);
    put16(t + 2, c->remote_port);
    put32(t + 4, c->snd_nxt);
    put32(t + 8, c->rcv_nxt);
    t[12] = (uint8_t)((thdr / 4) << 4);
    t[13] = flags;
    /* Advertise rcv_wnd based on actual available buffer space */
    uint16_t adv_wnd;
    {
        int free_space = rxring_free(&c->rx);
        if (free_space < 0) free_space = 0;
        if (c->wscale_ok)
            adv_wnd = (uint16_t)((uint32_t)free_space >> c->rcv_wscale);
        else
            adv_wnd = (uint16_t)(free_space > 65535 ? 65535 : free_space);
        if (adv_wnd == 0 && free_space > 0) adv_wnd = 1;
    }
    put16(t + 14, adv_wnd);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (optlen > 0 && opts) mcpy(t + 20, opts, optlen);
    if (ts_len > 0) mcpy(t + 20 + optlen, ts_opt, ts_len);
    if (dlen > 0 && data) mcpy(t + thdr, data, dlen);
    uint16_t ck = tcp_cksum(net_my_ip, c->dst_ip, t, tt);
    t[16] = (uint8_t)(ck >> 8);
    t[17] = (uint8_t)ck;
    net_send_raw(pkt, (uint16_t)(34 + tt));
}

static void send_tcp(net_tcp_t *c, uint8_t flags, const void *data, int dlen) {
    /* For SACK blocks on ACKs: build SACK option if we have blocks */
    if ((flags & 0x10) && c->sack_count > 0 && dlen == 0) {
        uint8_t opts[40]; /* max 4 SACK blocks = 2 + 4*8 = 34 bytes */
        int n = c->sack_count;
        if (n > 4) n = 4;
        int olen = 2 + n * 8;
        opts[0] = 5;              /* Kind = SACK */
        opts[1] = (uint8_t)olen;  /* Length */
        for (int i = 0; i < n; i++) {
            put32(opts + 2 + i * 8, c->sack_blocks[i].left);
            put32(opts + 2 + i * 8 + 4, c->sack_blocks[i].right);
        }
        send_tcp_opts(c, flags, data, dlen, opts, olen);
        return;
    }
    send_tcp_opts(c, flags, data, dlen, 0, 0);
}

/* ── SYN Options: MSS + WScale + SACK-Perm + Timestamps + TFO cookie ── */

static void send_syn(net_tcp_t *c, uint8_t flags) {
    /* MSS(4) + WScale(3) + NOP(1) + SACK-Perm(2) + NOP+NOP + Timestamp(10)
     * + TFO cookie request(2-18) */
    uint8_t opts[48];
    int olen = 0;
    mzero(opts, sizeof(opts));

    /* MSS option: Kind=2, Len=4, Value=1460 */
    opts[olen++] = 2; opts[olen++] = 4;
    put16(opts + olen, MSS); olen += 2;

    /* Window Scale: Kind=3, Len=3, Shift=RCV_WSCALE */
    opts[olen++] = 3; opts[olen++] = 3; opts[olen++] = RCV_WSCALE;

    /* NOP padding */
    opts[olen++] = 1;

    /* SACK Permitted: Kind=4, Len=2 */
    opts[olen++] = 4; opts[olen++] = 2;

    /* NOP + NOP for alignment before Timestamp */
    opts[olen++] = 1; opts[olen++] = 1;

    /* Timestamp option: Kind=8, Len=10, TSval, TSecr=0 (SYN) */
    opts[olen++] = 8; opts[olen++] = 10;
    put32(opts + olen, (uint32_t)timer_ms()); olen += 4;
    put32(opts + olen, 0); olen += 4; /* TSecr=0 on SYN */

    /* ECN: set CWR+ECE on outgoing SYN to negotiate */
    if (flags == 0x02) /* SYN only (client), not SYN-ACK */
        flags |= 0xC0; /* CWR(0x80) + ECE(0x40) */
    else if (flags == 0x12 && c->ecn_enabled) /* SYN-ACK: respond with ECE only */
        flags |= 0x40;

    /* TFO cookie: on SYN, request or send cached cookie */
    if (!(flags & 0x10)) { /* SYN only, not SYN-ACK */
        uint8_t cookie[16];
        uint8_t cookie_len = 0;
        if (tfo_cache_lookup(c->dst_ip, cookie, &cookie_len) == 0) {
            /* Have cookie: include it (Kind=34, Len=2+cookie_len) */
            opts[olen++] = 34;
            opts[olen++] = (uint8_t)(2 + cookie_len);
            mcpy(opts + olen, cookie, cookie_len);
            olen += cookie_len;
        } else {
            /* No cookie: request one (Kind=34, Len=2, empty) */
            opts[olen++] = 34; opts[olen++] = 2;
        }
    }

    /* Pad to 4-byte boundary */
    while (olen & 3) opts[olen++] = 1; /* NOP */

    send_tcp_opts(c, flags, 0, 0, opts, olen);
}

/* ── Parse TCP Options from SYN-ACK ──────────────── */

/* Parse result for per-packet timestamp values (not stored in conn on SYN-ACK parse) */
typedef struct {
    uint32_t tsval;    /* peer's TSval */
    uint32_t tsecr;    /* echo of our TSval */
    uint8_t  has_ts;   /* 1 if TSopt present */
} tcp_opt_parsed_t;

static void parse_tcp_options_ex(net_tcp_t *c, const uint8_t *opts, int optlen,
                                 tcp_opt_parsed_t *parsed) {
    int i = 0;
    while (i < optlen) {
        uint8_t kind = opts[i];
        if (kind == 0) break;            /* End of options */
        if (kind == 1) { i++; continue; } /* NOP */
        if (i + 1 >= optlen) break;
        uint8_t olen = opts[i + 1];
        if (olen < 2 || i + olen > optlen) break;

        if (kind == 3 && olen == 3) {
            /* Window Scale */
            c->snd_wscale = opts[i + 2];
            if (c->snd_wscale > 14) c->snd_wscale = 14;
            c->wscale_ok = 1;
        } else if (kind == 4 && olen == 2) {
            /* SACK Permitted — peer supports SACK */
            (void)0;
        } else if (kind == 5 && olen >= 10) {
            /* SACK blocks */
            int nblocks = (olen - 2) / 8;
            if (nblocks > 4) nblocks = 4;
            c->sack_count = nblocks;
            for (int j = 0; j < nblocks; j++) {
                c->sack_blocks[j].left  = get32(opts + i + 2 + j * 8);
                c->sack_blocks[j].right = get32(opts + i + 2 + j * 8 + 4);
            }
        } else if (kind == 8 && olen == 10) {
            /* Timestamp (RFC 7323): TSval + TSecr */
            uint32_t tsval = get32(opts + i + 2);
            uint32_t tsecr = get32(opts + i + 6);
            if (parsed) {
                parsed->tsval = tsval;
                parsed->tsecr = tsecr;
                parsed->has_ts = 1;
            }
            /* Negotiate: if we see it in SYN/SYN-ACK, enable */
            c->ts_enabled = 1;
            /* Update ts_recent for PAWS */
            if ((int32_t)(tsval - c->ts_recent) >= 0) {
                c->ts_recent = tsval;
                c->ts_recent_age = timer_ms();
            }
        } else if (kind == 34) {
            /* TFO Cookie (RFC 7413) */
            if (olen > 2) {
                /* Server sent us a cookie — cache it */
                uint8_t clen = olen - 2;
                if (clen >= 4 && clen <= 16) {
                    tfo_cache_store(c->dst_ip, opts + i + 2, clen);
                    c->tfo_cookie_len = clen;
                    mcpy(c->tfo_cookie, opts + i + 2, clen);
                    c->tfo_enabled = 1;
                }
            }
        }
        i += olen;
    }
}

static void parse_tcp_options(net_tcp_t *c, const uint8_t *opts, int optlen) {
    parse_tcp_options_ex(c, opts, optlen, 0);
}

/* ── SACK Block Management (RX side) ─────────────── */

static void sack_update(net_tcp_t *c, uint32_t seq, int len) {
    /* Add a received OOO segment to our SACK block list */
    uint32_t left = seq, right = seq + (uint32_t)len;

    /* Try to merge with existing blocks */
    for (int i = 0; i < c->sack_count; i++) {
        if ((int32_t)(left - c->sack_blocks[i].right) <= 0 &&
            (int32_t)(right - c->sack_blocks[i].left) >= 0) {
            /* Overlapping or adjacent — merge */
            if ((int32_t)(left - c->sack_blocks[i].left) < 0)
                c->sack_blocks[i].left = left;
            if ((int32_t)(right - c->sack_blocks[i].right) > 0)
                c->sack_blocks[i].right = right;
            return;
        }
    }

    /* New block */
    if (c->sack_count < 4) {
        c->sack_blocks[c->sack_count].left = left;
        c->sack_blocks[c->sack_count].right = right;
        c->sack_count++;
    } else {
        /* Evict oldest (slot 3) */
        c->sack_blocks[3].left = left;
        c->sack_blocks[3].right = right;
    }
}

static void sack_advance(net_tcp_t *c, uint32_t ack) {
    /* Remove SACK blocks fully covered by cumulative ACK */
    int w = 0;
    for (int r = 0; r < c->sack_count; r++) {
        if ((int32_t)(c->sack_blocks[r].right - ack) > 0) {
            if (w != r) c->sack_blocks[w] = c->sack_blocks[r];
            w++;
        }
    }
    c->sack_count = w;
}

/* ── OOO Buffer Helpers (called on RT-Core, no locks) ── */

static void ooo_insert(net_tcp_t *c, uint32_t seq, const uint8_t *data, int len) {
    if (len <= 0 || len > 1460) return;
    for (int i = 0; i < c->ooo_count; i++)
        if (c->ooo[i].seq == seq) return; /* duplicate */

    int slot;
    if (c->ooo_count < NET_TCP_OOO_SLOTS) {
        slot = c->ooo_count++;
    } else {
        slot = 0;
        for (int i = 1; i < NET_TCP_OOO_SLOTS; i++)
            if ((int32_t)(c->ooo[i].seq - c->ooo[slot].seq) < 0)
                slot = i;
    }
    c->ooo[slot].seq = seq;
    c->ooo[slot].len = (uint16_t)len;
    mcpy(c->ooo[slot].data, data, len);

    /* Update SACK blocks for OOO segment */
    sack_update(c, seq, len);
}

static int ooo_drain(net_tcp_t *c) {
    int drained = 0, progress = 1;
    while (progress && c->ooo_count > 0) {
        progress = 0;
        for (int i = 0; i < c->ooo_count; i++) {
            if (c->ooo[i].seq == c->rcv_nxt) {
                rxring_push(&c->rx, c->ooo[i].data, c->ooo[i].len);
                c->rcv_nxt += c->ooo[i].len;
                drained += c->ooo[i].len;
                c->ooo_count--;
                if (i < c->ooo_count)
                    c->ooo[i] = c->ooo[c->ooo_count];
                progress = 1;
                break;
            }
        }
    }
    /* Remove SACK blocks covered by advanced rcv_nxt */
    sack_advance(c, c->rcv_nxt);
    return drained;
}

/* ── Integer Cube Root (Newton, ×1024 scaled) ─────── */

static uint32_t integer_cbrt(uint64_t x) {
    if (x == 0) return 0;
    /* Initial estimate: start high enough */
    uint64_t g = 1;
    while (g * g * g < x) {
        if (g > 100000) break;
        g <<= 1;
    }
    /* Newton iterations: g = (2*g + x/(g*g)) / 3 */
    for (int i = 0; i < 32; i++) {
        uint64_t g2 = g * g;
        if (g2 == 0) break;
        uint64_t g_new = (2 * g + x / g2) / 3;
        if (g_new >= g) break;
        g = g_new;
    }
    /* Refine: ensure g^3 <= x < (g+1)^3 */
    while ((g + 1) * (g + 1) * (g + 1) <= x) g++;
    return (uint32_t)g;
}

/* ── CUBIC Congestion Control (RFC 8312) ──────────── */

/*
 * W_cubic(t) = C × (t - K)³ + W_max
 * K = cbrt(W_max × β_cubic / C)   [β_cubic = 0.3 = 1-0.7]
 *
 * All cwnd values in bytes. K in ms.
 * Fixed-point: multiply by 1024 where noted.
 *
 * C = 0.4 segments/s³ → scale for bytes:
 *   C_bytes = 0.4 × MSS = 584 bytes/s³
 * But t is in ms, so t³ is ms³. Need C in bytes/ms³:
 *   C_eff = 584 / 1000³ = 5.84e-7 bytes/ms³
 * To avoid FP: W(t) = C_eff × (t-K)³ + W_max
 *   = (584 × (t-K)³) / 1000000000 + W_max
 *   = (MSS × 4 × (t-K)³) / (10 × 1000000000) + W_max
 * Simplified: (MSS * 4 * dt³) / 10_000_000_000
 */

static uint32_t cubic_k_ms(uint32_t w_max_bytes) {
    /* K = cbrt(W_max × (1-β) / C)  in ms
     * K = cbrt(w_max_seg × 0.3 / 0.4) ms × 1000  (C is per-second)
     * K = cbrt(w_max_seg × 3/4) × 1000^(1/3)
     * K = cbrt(w_max_seg × 3/4 × 1000) ms
     * = cbrt(w_max_seg × 750) ms */
    uint32_t w_seg = w_max_bytes / MSS;
    if (w_seg == 0) w_seg = 1;
    uint64_t val = (uint64_t)w_seg * 750;
    return integer_cbrt(val);
}

static uint32_t cubic_w(uint64_t t_ms, uint32_t w_max, uint32_t k_ms) {
    /* W(t) = C × (t - K)³ + W_max
     * C = 0.4 seg/s³, t and K in ms
     * W(t) = 0.4 × ((t-K)/1000)³ × MSS + W_max
     * = MSS × 4 × (t-K)³ / (10 × 10^9) + W_max
     * = MSS × 4 × dt³ / 10000000000 + W_max */
    int64_t dt = (int64_t)t_ms - (int64_t)k_ms; /* can be negative */
    int neg = 0;
    if (dt < 0) { neg = 1; dt = -dt; }

    /* dt³ can overflow for large dt. Cap at ~10000ms = 10s */
    if (dt > 10000) dt = 10000;
    uint64_t dt3 = (uint64_t)dt * (uint64_t)dt * (uint64_t)dt;
    /* MSS * 4 * dt3 / 10_000_000_000 */
    uint64_t inc = ((uint64_t)MSS * 4 * dt3) / 10000000000ULL;

    if (neg) {
        if (inc >= w_max) return MSS; /* floor */
        return w_max - (uint32_t)inc;
    }
    uint64_t w = (uint64_t)w_max + inc;
    if (w > 0x7FFFFFFF) w = 0x7FFFFFFF; /* cap at ~2GB */
    return (uint32_t)w;
}

/* TCP-friendly check: Reno-equivalent throughput must not be worse */
static uint32_t reno_friendly_w(net_tcp_t *c, uint64_t t_ms) {
    /* W_tcp(t) = W_max × (1-β) + 3 × β/(2-β) × t/RTT
     * Simplified: W_tcp = W_max*0.3 + 1.5*0.7/1.3 * t/RTT
     * ≈ W_max*0.3 + 0.808 * t/RTT * MSS
     * With RTT estimate from RTO: */
    uint64_t rtt = c->rto_ms ? c->rto_ms : 100;
    uint64_t w_base = ((uint64_t)c->cubic_w_max * CUBIC_ONE_MINUS_BETA) / 1024;
    uint64_t w_inc = (808 * t_ms * MSS) / (1000 * rtt);
    uint64_t w = w_base + w_inc;
    if (w > 0x7FFFFFFF) w = 0x7FFFFFFF;
    if (w < MSS) w = MSS;
    return (uint32_t)w;
}

static void cc_init(net_tcp_t *c) {
    c->cwnd = 10 * MSS;           /* IW=10 (RFC 6928) */
    c->ssthresh = 0x7FFFFFFF;     /* effectively infinite */
    c->dup_ack_count = 0;
    c->in_recovery = 0;
    c->cubic_t_epoch = 0;
    c->cubic_w_max = 0;
    c->cubic_w_last = 0;
    c->cubic_k = 0;
}

static void cc_on_ack(net_tcp_t *c, uint32_t bytes_acked) {
    if (bytes_acked == 0) return;

    /* Exit Fast Recovery on new ACK past recovery_seq */
    if (c->in_recovery) {
        if ((int32_t)(c->snd_una - c->recovery_seq) >= 0) {
            c->in_recovery = 0;
            c->cwnd = c->ssthresh;
            c->dup_ack_count = 0;
        }
        return;
    }

    c->dup_ack_count = 0;

    /* Slow-Start */
    if (c->cwnd < c->ssthresh) {
        c->cwnd += MSS;
        return;
    }

    /* CUBIC Congestion Avoidance */
    if (c->cubic_t_epoch == 0) {
        /* First ACK after loss or start — begin epoch */
        c->cubic_t_epoch = timer_ms();
        if (c->cubic_w_max == 0)
            c->cubic_w_max = c->cwnd; /* initial */
        c->cubic_k = cubic_k_ms(c->cubic_w_max);
    }

    uint64_t t = timer_ms() - c->cubic_t_epoch;
    uint32_t w_cub = cubic_w(t, c->cubic_w_max, c->cubic_k);
    uint32_t w_tcp = reno_friendly_w(c, t);

    /* Use the larger of CUBIC and Reno-friendly */
    uint32_t target = w_cub > w_tcp ? w_cub : w_tcp;
    if (target > c->cwnd)
        c->cwnd = target;
    else {
        /* At minimum, grow by 1 byte per RTT (like Reno) */
        uint32_t inc = (MSS * MSS) / c->cwnd;
        if (inc < 1) inc = 1;
        c->cwnd += inc;
    }
}

static void cc_on_loss(net_tcp_t *c) {
    /* Fast Convergence: if W_max < W_last, reduce W_max further */
    if (c->cubic_w_last && c->cwnd < c->cubic_w_last)
        c->cubic_w_max = (uint32_t)((uint64_t)c->cwnd * (1024 + CUBIC_BETA) / 2048);
    else
        c->cubic_w_max = c->cwnd;

    c->cubic_w_last = c->cwnd;
    c->cubic_t_epoch = timer_ms();
    c->cubic_k = cubic_k_ms(c->cubic_w_max);

    /* β = 0.7: cwnd *= 0.7 */
    c->ssthresh = (uint32_t)((uint64_t)c->cwnd * CUBIC_BETA / 1024);
    if (c->ssthresh < 2 * MSS) c->ssthresh = 2 * MSS;
    c->cwnd = c->ssthresh;
}

/* ── TCP Input (from dispatcher) ───────────────────── */

void tcp_input(const uint8_t *pkt, int len) {
    if (len < 54) return;

    uint16_t dport = get16(pkt + 36);
    uint16_t sport = get16(pkt + 34);
    const uint8_t *src_ip = pkt + 26;

    net_tcp_t *c = tcp_find(dport, sport, src_ip);
    if (!c) {
        q_push(&q_tcp, pkt, len);
        struct thread *wt = __atomic_load_n(&q_tcp_wait_thread, __ATOMIC_ACQUIRE);
        if (wt) event_post(wt, EQ_SOCKET_CONNECT, 0);
        return;
    }

    uint8_t flags = pkt[47];
    int doff = (pkt[46] >> 4) * 4;
    if (doff < 20) doff = 20;
    int ip_total = get16(pkt + 16);
    if (ip_total < 20 + doff) return;
    int plen = ip_total - 20 - doff;
    if (14 + 20 + doff + plen > len) plen = len - 14 - 20 - doff;
    if (plen < 0) plen = 0;
    uint32_t tseq = get32(pkt + 38);

    /* Parse TCP options from any packet with options */
    tcp_opt_parsed_t opt_parsed;
    mzero(&opt_parsed, sizeof(opt_parsed));
    if (doff > 20) {
        const uint8_t *opts = pkt + 34 + 20;
        int optlen = doff - 20;
        parse_tcp_options_ex(c, opts, optlen, &opt_parsed);
    }

    /* PAWS (RFC 7323): drop segment with old timestamp */
    if (c->ts_enabled && opt_parsed.has_ts && c->state >= TCP_ESTABLISHED) {
        if ((int32_t)(opt_parsed.tsval - c->ts_recent) < 0 && !(flags & 0x04)) {
            /* Timestamp older than ts_recent and not RST — drop, send ACK */
            send_tcp(c, 0x10, 0, 0);
            return;
        }
    }

    /* ECN: check IP header CE marking (bits 0-1 of TOS byte = pkt[15]) */
    if (c->ecn_enabled && c->state >= TCP_ESTABLISHED) {
        uint8_t ecn_bits = pkt[15] & 0x03;
        if (ecn_bits == 0x03) /* CE (Congestion Experienced) */
            c->ecn_ce_pending = 1;
    }

    /* SYN-ACK for outgoing connect */
    if (c->state == TCP_SYN_SENT && (flags & 0x12) == 0x12) {
        c->rcv_nxt = tseq + 1;
        c->snd_nxt++;
        c->snd_una = c->snd_nxt;
        /* Parse SYN-ACK options for Window Scale, SACK, Timestamps, TFO */
        if (doff > 20)
            parse_tcp_options(c, pkt + 34 + 20, doff - 20);
        /* Set rcv_wscale now that negotiation is done */
        if (c->wscale_ok)
            c->rcv_wscale = RCV_WSCALE;
        /* Initialize snd_wnd from SYN-ACK (missed if we return early) */
        { uint32_t raw_wnd = get16(pkt + 48);
          c->snd_wnd = c->wscale_ok ? (raw_wnd << c->snd_wscale) : raw_wnd; }
        /* ECN negotiation: SYN-ACK must have ECE but not CWR */
        if ((flags & 0xC0) == 0x40) /* ECE only */
            c->ecn_enabled = 1;
        else
            c->ecn_enabled = 0; /* peer doesn't support ECN */
        send_tcp(c, 0x10, 0, 0);
        c->state = TCP_ESTABLISHED;
        c->connect_err = 0;
        cc_init(c);
        struct thread *wt = __atomic_load_n(&c->wait_thread, __ATOMIC_ACQUIRE);
        if (wt) event_post(wt, EQ_SOCKET_DATA, 0);
        return;
    }

    /* RST */
    if (flags & 0x04) {
        c->state = TCP_CLOSED;
        c->got_rst = 1;
        c->connect_err = -1;
        struct thread *wt = __atomic_load_n(&c->wait_thread, __ATOMIC_ACQUIRE);
        if (wt) event_post(wt, EQ_SOCKET_DATA, 0);
        return;
    }

    /* ACK processing */
    if (flags & 0x10) {
        uint32_t ack_num = get32(pkt + 42);
        int state_changed = 0;

        if ((int32_t)(ack_num - c->snd_una) > 0) {
            uint32_t bytes_acked = ack_num - c->snd_una;
            c->snd_una = ack_num;

            /* Timestamp-based RTT measurement (RFC 7323) */
            if (c->ts_enabled && opt_parsed.has_ts && opt_parsed.tsecr) {
                uint64_t rtt = timer_ms() - (uint64_t)opt_parsed.tsecr;
                if (rtt > 0 && rtt < 120000) {
                    /* Karn's algorithm: use timestamp RTT, update RTO */
                    /* Simple SRTT: RTO = max(200, rtt * 2) capped at MAX_RTO */
                    uint64_t new_rto = rtt * 2;
                    if (new_rto < 200) new_rto = 200;
                    if (new_rto > NET_TCP_MAX_RTO_MS) new_rto = NET_TCP_MAX_RTO_MS;
                    c->rto_ms = new_rto;
                }
            }

            /* ECN: if peer sent ECE, reduce cwnd and send CWR */
            if (c->ecn_enabled && (flags & 0x40)) { /* ECE */
                if (!c->ecn_cwr_sent) {
                    cc_on_loss(c);
                    c->ecn_cwr_sent = 1;
                }
                /* Clear ce_pending when we see our CWR acknowledged */
            }
            /* ECN: if peer sent CWR, stop sending ECE */
            if (c->ecn_enabled && (flags & 0x80))
                c->ecn_ce_pending = 0;

            cc_on_ack(c, bytes_acked);
        } else if (ack_num == c->snd_una && plen == 0) {
            /* Duplicate ACK */
            c->dup_ack_count++;

            if (!c->in_recovery && c->dup_ack_count == 3) {
                /* Fast Retransmit (RFC 5681 §3.2) */
                cc_on_loss(c);
                c->in_recovery = 1;
                c->recovery_seq = c->snd_nxt;
                /* Inflate cwnd by 3 segments for the 3 DupACKs */
                c->cwnd += 3 * MSS;
                /* Retransmit first unacked segment */
                send_tcp(c, 0x10, 0, 0);
            } else if (c->in_recovery) {
                /* Each additional DupACK: inflate cwnd by MSS */
                c->cwnd += MSS;
            }
        }

        /* Window update with scaling */
        uint32_t raw_wnd = get16(pkt + 48);
        if (c->wscale_ok)
            c->snd_wnd = raw_wnd << c->snd_wscale;
        else
            c->snd_wnd = raw_wnd;

        if (c->state == TCP_FIN_WAIT1 && ack_num == c->snd_nxt) {
            c->state = TCP_FIN_WAIT2; state_changed = 1;
        } else if (c->state == TCP_CLOSING && ack_num == c->snd_nxt) {
            c->state = TCP_TIME_WAIT; state_changed = 1;
        } else if (c->state == TCP_LAST_ACK && ack_num == c->snd_nxt) {
            c->state = TCP_CLOSED; tcp_unregister(c); state_changed = 1;
        }
        if (state_changed) {
            struct thread *wt = __atomic_load_n(&c->wait_thread, __ATOMIC_ACQUIRE);
            if (wt) event_post(wt, EQ_SOCKET_DATA, 0);
        }
    }

    /* Data */
    if (plen > 0) {
        const uint8_t *payload = pkt + 14 + 20 + doff;
        if (tseq == c->rcv_nxt) {
            int stored = rxring_push(&c->rx, payload, plen);
            c->rcv_nxt = tseq + (uint32_t)stored;
            ooo_drain(c);
            send_tcp(c, 0x10, 0, 0);
            struct thread *wt = __atomic_load_n(&c->wait_thread, __ATOMIC_ACQUIRE);
            if (wt) event_post(wt, EQ_SOCKET_DATA, 0);
        } else if ((int32_t)(tseq - c->rcv_nxt) > 0) {
            ooo_insert(c, tseq, payload, plen);
            send_tcp(c, 0x10, 0, 0); /* DupACK with SACK */
        }
    }

    /* FIN */
    if (flags & 0x01) {
        uint32_t fin_seq = tseq + (uint32_t)plen;
        if (fin_seq != c->rcv_nxt) return;
        c->got_fin = 1;
        c->rcv_nxt++;
        if (c->state == TCP_ESTABLISHED) {
            c->state = TCP_CLOSE_WAIT;
            send_tcp(c, 0x10, 0, 0);
        } else if (c->state == TCP_FIN_WAIT1) {
            c->state = TCP_CLOSING;
            send_tcp(c, 0x10, 0, 0);
        } else if (c->state == TCP_FIN_WAIT2) {
            c->state = TCP_TIME_WAIT;
            send_tcp(c, 0x10, 0, 0);
        }
        struct thread *wt = __atomic_load_n(&c->wait_thread, __ATOMIC_ACQUIRE);
        if (wt) event_post(wt, EQ_SOCKET_DATA, 0);
    }

    /* Keepalive: reset probe timer on any valid packet */
    if (c->keepalive) {
        c->keepalive_next = timer_ms() + NET_TCP_KEEPALIVE_INTERVAL_MS;
        c->keepalive_probes = 0;
    }
}

/* ── TCP Accept ────────────────────────────────────── */

int net_tcp_accept(net_tcp_t *c, uint16_t local_port, int timeout_ms) {
    (void)timeout_ms;
    if (c->state == TCP_SYN_RCVD) {
        uint8_t pkt[Q_PKT];
        int len = q_pop(&q_tcp, pkt, sizeof(pkt));
        if (len < 54) return -11;
        if (get16(pkt + 36) != c->local_port ||
            get16(pkt + 34) != c->remote_port) {
            q_push(&q_tcp, pkt, len);
            return -11;
        }
        uint8_t fl = pkt[47];
        if (fl & 0x04) { c->state = TCP_CLOSED; return -1; }
        if (fl & 0x10) {
            uint32_t ack_num = get32(pkt + 42);
            if (ack_num == c->snd_nxt + 1) {
                c->snd_nxt++;
                c->snd_una = c->snd_nxt;
                c->state = TCP_ESTABLISHED;
                tcp_register(c);
                return 0;
            }
        }
        return -11;
    }

    uint8_t pkt[Q_PKT];
    int len = q_pop(&q_tcp, pkt, sizeof(pkt));
    if (len < 54) return -11;
    uint16_t dport = get16(pkt + 36);
    if (dport != local_port) { q_push(&q_tcp, pkt, len); return -11; }
    uint8_t fl = pkt[47];
    if (!((fl & 0x02) && !(fl & 0x10))) { q_push(&q_tcp, pkt, len); return -11; }

    mzero(c, sizeof(*c));
    rxring_init(&c->rx);
    mcpy(c->dst_mac, pkt + 6, 6);
    mcpy(c->dst_ip, pkt + 26, 4);
    c->remote_port = get16(pkt + 34);
    c->local_port = get16(pkt + 36);
    uint32_t client_isn = get32(pkt + 38);
    c->rcv_nxt = client_isn + 1;
    uint32_t rseq;
    net_random(&rseq, (int)sizeof(rseq));
    c->snd_nxt = rseq;
    c->snd_una = rseq;
    c->rcv_wnd = (uint16_t)(NET_TCP_RXBUF >> RCV_WSCALE);

    /* Parse SYN options from client (Timestamps, Window Scale, SACK, TFO) */
    int doff = (pkt[46] >> 4) * 4;
    if (doff > 20) {
        parse_tcp_options(c, pkt + 34 + 20, doff - 20);
        if (c->wscale_ok) c->rcv_wscale = RCV_WSCALE;
    }

    /* ECN negotiation: if client SYN has CWR+ECE, enable ECN */
    if ((fl & 0xC0) == 0xC0)
        c->ecn_enabled = 1;

    /* Send SYN-ACK with options */
    send_syn(c, 0x12);
    c->state = TCP_SYN_RCVD;
    return -11;
}

/* ── TCP Connect ───────────────────────────────────── */

int net_tcp_connect(net_tcp_t *c, const uint8_t *dst_ip, uint16_t port) {
    if (c->state == TCP_ESTABLISHED) return 0;
    if (c->state == TCP_SYN_SENT) return -11;
    if (c->got_rst) return -1;

    mzero(c, sizeof(*c));
    rxring_init(&c->rx);
    mcpy(c->dst_ip, dst_ip, 4);
    c->remote_port = port;
    uint32_t rseq;
    net_random(&rseq, (int)sizeof(rseq));
    uint16_t rport;
    net_random(&rport, (int)sizeof(rport));
    c->local_port = (uint16_t)((rport % 16384) + 49152);
    c->snd_nxt = rseq;
    c->snd_una = rseq;
    c->rcv_nxt = 0;
    c->rcv_wnd = (uint16_t)(NET_TCP_RXBUF >> RCV_WSCALE);
    c->rcv_wscale = RCV_WSCALE;
    c->state = TCP_CLOSED;
    cc_init(c);

    /* Loopback: skip ARP (packets bypass NIC via loopback_inject) */
    if (dst_ip[0] == 127)
        mcpy(c->dst_mac, net_my_mac, 6);
    else if (net_arp_resolve(net_gw_ip, c->dst_mac) < 0) return -1;

    c->state = TCP_SYN_SENT;
    tcp_register(c);
    /* SYN with MSS + Window Scale + SACK Permitted options */
    send_syn(c, 0x02);
    return -11;
}

/* ── TCP Send ──────────────────────────────────────── */

int net_tcp_send(net_tcp_t *c, const void *data, int len) {
    if (c->state != TCP_ESTABLISHED) return -1;
    const uint8_t *p = data;
    int sent = 0;
    uint32_t eff_wnd = c->cwnd;
    if (c->snd_wnd && c->snd_wnd < eff_wnd) eff_wnd = c->snd_wnd;
    if (eff_wnd < MSS) eff_wnd = MSS;

    while (sent < len) {
        int ch = len - sent;
        if (ch > 1400) ch = 1400;
        uint32_t in_flight = c->snd_nxt - c->snd_una;
        if (in_flight >= eff_wnd) {
            if (sent > 0) break;
        }
        send_tcp(c, 0x18, p + sent, ch);
        c->snd_nxt += (uint32_t)ch;
        sent += ch;
    }
    return sent;
}

/* ── TCP Recv ──────────────────────────────────────── */

int net_tcp_recv(net_tcp_t *c, void *buf, int bufsize, int timeout_ms) {
    (void)timeout_ms;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) {
        if (c->state == TCP_CLOSED && (c->got_fin || c->got_rst)) {
            int n = rxring_pop(&c->rx, buf, bufsize);
            return n > 0 ? n : (c->got_rst ? -1 : 0);
        }
        return -1;
    }
    int free_before = rxring_free(&c->rx);
    int got = rxring_pop(&c->rx, buf, bufsize);
    if (got > 0) {
        /* RFC 1122 §4.2.2.17: send window update when free space crosses
         * MSS threshold or window was zero.  Prevents sender stall. */
        int free_after = rxring_free(&c->rx);
        if (free_before < MSS && free_after >= MSS)
            send_tcp(c, 0x10, 0, 0);
        return got;
    }
    if (c->got_fin) return 0;
    if (c->got_rst) return -1;
    return -11;
}

/* ── TCP Close ─────────────────────────────────────── */

void net_tcp_close(net_tcp_t *c) {
    if (c->state == TCP_ESTABLISHED) {
        send_tcp(c, 0x11, 0, 0);
        c->snd_nxt++;
        c->state = TCP_FIN_WAIT1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        send_tcp(c, 0x11, 0, 0);
        c->snd_nxt++;
        c->state = TCP_LAST_ACK;
    }
    tcp_unregister(c);
    rxring_destroy(&c->rx);
    c->state = TCP_CLOSED;
}

/* ── TCP Retransmit (called from Timer Wheel on RT-Core) ── */

void net_tcp_retransmit(void *conn) {
    net_tcp_t *c = (net_tcp_t *)conn;
    if (!c || c->state != TCP_ESTABLISHED) return;
    cc_on_loss(c);
    c->in_recovery = 0; /* RTO exits fast recovery */
    c->cubic_t_epoch = timer_ms();
    send_tcp(c, 0x10, 0, 0);
}

/* ── TCP Keepalive Probe (called from Timer Wheel on RT-Core) ── */

void net_tcp_keepalive_probe(void *conn) {
    net_tcp_t *c = (net_tcp_t *)conn;
    if (!c || !c->keepalive) return;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return;

    c->keepalive_probes++;
    if (c->keepalive_probes >= NET_TCP_KEEPALIVE_MAX_PROBES) {
        c->state = TCP_CLOSED;
        c->got_rst = 1;
        struct thread *wt = __atomic_load_n(&c->wait_thread, __ATOMIC_ACQUIRE);
        if (wt) event_post(wt, EQ_SOCKET_DATA, 0);
        return;
    }

    uint32_t saved = c->snd_nxt;
    c->snd_nxt = saved - 1;
    send_tcp(c, 0x10, 0, 0);
    c->snd_nxt = saved;
    c->keepalive_next = timer_ms() + NET_TCP_KEEPALIVE_INTERVAL_MS;
}
