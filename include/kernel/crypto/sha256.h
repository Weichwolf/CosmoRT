/* CosmoRT SHA-256 — FIPS 180-4 compliant.
 * Scalar C implementation (portable). No inline-asm.
 * RT-safe: no FPU, no malloc. */
#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint8_t  buf[64];
    uint64_t count;       /* bytes processed */
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t hash[32]);
void sha256_oneshot(const void *data, size_t len, uint8_t hash[32]);

#endif
