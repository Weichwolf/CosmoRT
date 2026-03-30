/* CosmoRT CSPRNG — ChaCha20 with multiple entropy sources */

#include "random.h"
#include "hw/serial.h"
#include "spinlock.h"
#include "arch/arch.h"
#include "memops.h"

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

static uint32_t csprng_state[16];
static uint32_t entropy_pool[8];
static int rng_initialized;
static spinlock_t rng_lock = SPINLOCK_INIT;

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

void random_init(struct boot_info *info) {
    entropy_mix(arch_rdtsc());

    entropy_mix(info->mmap_addr);
    entropy_mix(info->mmap_size);
    entropy_mix(info->fb_addr);

    extern uint64_t timer_tsc_per_ms;
    entropy_mix(timer_tsc_per_ms);

    entropy_mix(arch_get_rsp());

    entropy_mix(info->rsdp_addr);

    if (memops_has_rdrand) {
        entropy_mix(arch_rdrand());
        entropy_mix(arch_rdrand());
    }

    csprng_state[0]  = 0x61707865;
    csprng_state[1]  = 0x3320646e;
    csprng_state[2]  = 0x79622d32;
    csprng_state[3]  = 0x6b206574;

    for (int i = 0; i < 8; i++)
        csprng_state[4 + i] = entropy_pool[i];

    csprng_state[12] = 0;
    csprng_state[13] = 0;

    {
        uint64_t tsc = arch_rdtsc();
        csprng_state[14] = (uint32_t)tsc;
        csprng_state[15] = (uint32_t)(tsc >> 32);
    }

    rng_initialized = 1;

    serial_puts("random: CSPRNG init (ChaCha20");
    if (memops_has_rdrand) serial_puts(" + RDRAND");
    serial_puts(")\n");
}

int random_get(void *buf, size_t len) {
    if (!rng_initialized) return -1;

    uint8_t *out = (uint8_t *)buf;
    uint64_t flags;

    while (len > 0) {
        spin_lock_irq(&rng_lock, &flags);

        uint32_t key_block[16], out_block[16];
        chacha20_block(key_block, csprng_state);
        csprng_state[12]++;
        if (csprng_state[12] == 0) csprng_state[13]++;

        chacha20_block(out_block, csprng_state);
        csprng_state[12]++;
        if (csprng_state[12] == 0) csprng_state[13]++;

        for (int i = 0; i < 8; i++)
            csprng_state[4 + i] = key_block[i];

        for (int i = 0; i < 16; i++)
            key_block[i] = 0;

        spin_unlock_irq(&rng_lock, flags);

        size_t n = len > 64 ? 64 : len;
        uint8_t *src = (uint8_t *)out_block;
        for (size_t i = 0; i < n; i++)
            out[i] = src[i];
        out += n;
        len -= n;
    }
    return 0;
}

void random_add_interrupt_entropy(void) {
    if (!rng_initialized) return;
    static uint32_t irq_count;
    if ((++irq_count & 63) != 0) return;

    entropy_mix(arch_rdtsc());

    if (memops_has_rdrand) {
        uint64_t r;
        if (arch_rdrand_checked(&r)) entropy_mix(r);
    }

    uint64_t rng_flags;
    spin_lock_irq(&rng_lock, &rng_flags);
    for (int i = 0; i < 8; i++)
        csprng_state[4 + i] ^= entropy_pool[i];
    spin_unlock_irq(&rng_lock, rng_flags);
}
