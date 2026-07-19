#pragma once

#include <stddef.h>
#include <stdint.h>

#define CHACHA20_KEY_SIZE 32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64
#define CHACHA20_ROUNDS 20

struct chacha20_ctx {
    uint32_t state[16];
    uint32_t keystream32[16];
    size_t position;
};

struct chacha20_rng {
    struct chacha20_ctx inner;
    uint64_t counter;
};

void chacha20_init(struct chacha20_ctx *ctx, const uint8_t key[32], const uint8_t nonce[12],
                   uint32_t counter);
void chacha20_encrypt(struct chacha20_ctx *ctx, uint8_t *data, size_t len);

void chacha20_rng_init(struct chacha20_rng *rng, const uint8_t seed[32]);
void chacha20_rng_bytes(struct chacha20_rng *rng, uint8_t *buf, size_t len);
void chacha20_rng_mix(struct chacha20_rng *rng, const uint8_t *data, size_t len);

extern struct chacha20_rng g_chacha20_rng;
