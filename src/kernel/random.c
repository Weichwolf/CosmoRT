/* CosmoRT CSPRNG — ChaCha20 with multiple entropy sources
 *
 * Works without RDRAND (QEMU TCG). Entropy from RDTSC, UEFI memory map,
 * timer calibration, boot info, and interrupt jitter. RDRAND mixed in
 * when available.
 */

#include "random.h"
#include "serial.h"
#include "spinlock.h"
#include "memops.h"

/* ── ChaCha20 core ─────────────────────────────── */

#define QR(a,b,c,d) do { \
    a += b; d ^= a; d = (d << 16) | (d >> 16); \
    c += d; b ^= c; b = (b << 12) | (b >> 20); \
    a += b; d ^= a; d = (d << 8)  | (d >> 24); \
    c += d; b ^= c; b = (b << 7)  | (b >> 25); \
} while(0)

static void chacha20_block(uint32_t out[16], const uint32_t state[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = state[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0],x[4],x[ 8],x[12]);
        QR(x[1],x[5],x[ 9],x[13]);
        QR(x[2],x[6],x[10],x[14]);
        QR(x[3],x[7],x[11],x[15]);
        QR(x[0],x[5],x[10],x[15]);
        QR(x[1],x[6],x[11],x[12]);
        QR(x[2],x[7],x[ 8],x[13]);
        QR(x[3],x[4],x[ 9],x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + state[i];
}

/* ── State ─────────────────────────────────────── */

static uint32_t csprng_state[16];
static uint32_t entropy_pool[8];
static int rng_initialized;
static spinlock_t rng_lock = SPINLOCK_INIT;

/* ── Entropy mixing ────────────────────────────── */

static void entropy_mix(uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    for (int i = 0; i < 8; i++) {
        entropy_pool[i] ^= lo;
        entropy_pool[i] = (entropy_pool[i] << 13) | (entropy_pool[i] >> 19);
        entropy_pool[i] += hi;
        lo ^= entropy_pool[(i + 3) & 7];
        hi ^= entropy_pool[(i + 5) & 7];
    }
}

/* ── Init ──────────────────────────────────────── */

void random_init(struct boot_info *info) {
    uint32_t lo, hi;

    /* RDTSC — high resolution, unique per boot */
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    entropy_mix(((uint64_t)hi << 32) | lo);

    /* UEFI memory map (physical addresses vary per boot) */
    entropy_mix(info->mmap_addr);
    entropy_mix(info->mmap_size);
    entropy_mix(info->fb_addr);

    /* Timer calibration (varies with CPU/bus) */
    extern uint64_t timer_tsc_per_ms;
    entropy_mix(timer_tsc_per_ms);

    /* Stack address */
    uint64_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    entropy_mix(sp);

    /* RSDP address */
    entropy_mix(info->rsdp_addr);

    /* RDRAND if available */
    if (memops_has_rdrand) {
        uint64_t r;
        __asm__ volatile("rdrand %0" : "=r"(r));
        entropy_mix(r);
        __asm__ volatile("rdrand %0" : "=r"(r));
        entropy_mix(r);
    }

    /* ChaCha20 constants: "expand 32-byte k" */
    csprng_state[0]  = 0x61707865;
    csprng_state[1]  = 0x3320646e;
    csprng_state[2]  = 0x79622d32;
    csprng_state[3]  = 0x6b206574;

    /* Key from entropy pool */
    for (int i = 0; i < 8; i++)
        csprng_state[4 + i] = entropy_pool[i];

    /* Counter */
    csprng_state[12] = 0;
    csprng_state[13] = 0;

    /* Nonce from another RDTSC sample */
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    csprng_state[14] = lo;
    csprng_state[15] = hi;

    rng_initialized = 1;

    serial_puts("random: CSPRNG init (ChaCha20");
    if (memops_has_rdrand) serial_puts(" + RDRAND");
    serial_puts(")\n");
}

/* ── Generate random bytes (thread-safe) ───────── */

int random_get(void *buf, size_t len) {
    if (!rng_initialized) return -1;

    uint8_t *out = (uint8_t *)buf;
    uint64_t flags;

    while (len > 0) {
        spin_lock_irq(&rng_lock, &flags);

        uint32_t block[16];
        chacha20_block(block, csprng_state);

        csprng_state[12]++;
        if (csprng_state[12] == 0) csprng_state[13]++;

        spin_unlock_irq(&rng_lock, flags);

        size_t n = len > 64 ? 64 : len;
        uint8_t *src = (uint8_t *)block;
        for (size_t i = 0; i < n; i++)
            out[i] = src[i];
        out += n;
        len -= n;
    }
    return 0;
}

/* ── Interrupt entropy (called from timer handler) ── */

void random_add_interrupt_entropy(void) {
    if (!rng_initialized) return;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    static uint32_t irq_count;
    if ((++irq_count & 63) == 0) {
        entropy_mix(((uint64_t)hi << 32) | lo);
        /* Re-key CSPRNG with fresh entropy */
        for (int i = 0; i < 8; i++)
            csprng_state[4 + i] ^= entropy_pool[i];
    }
}
