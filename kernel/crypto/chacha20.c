#include "crypto/chacha20.h"
#include "lib/string.h"

static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static uint32_t load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void chacha20_next_block(struct chacha20_ctx *ctx) {
    uint32_t x[16];
    memcpy(x, ctx->state, sizeof(x));

    for (int i = 0; i < 10; i++) {
        x[0] += x[4]; x[12] = rotl32(x[12] ^ x[0], 16);
        x[1] += x[5]; x[13] = rotl32(x[13] ^ x[1], 16);
        x[2] += x[6]; x[14] = rotl32(x[14] ^ x[2], 16);
        x[3] += x[7]; x[15] = rotl32(x[15] ^ x[3], 16);

        x[8] += x[12]; x[4] = rotl32(x[4] ^ x[8], 12);
        x[9] += x[13]; x[5] = rotl32(x[5] ^ x[9], 12);
        x[10] += x[14]; x[6] = rotl32(x[6] ^ x[10], 12);
        x[11] += x[15]; x[7] = rotl32(x[7] ^ x[11], 12);

        x[0] += x[4]; x[12] = rotl32(x[12] ^ x[0], 8);
        x[1] += x[5]; x[13] = rotl32(x[13] ^ x[1], 8);
        x[2] += x[6]; x[14] = rotl32(x[14] ^ x[2], 8);
        x[3] += x[7]; x[15] = rotl32(x[15] ^ x[3], 8);

        x[8] += x[12]; x[4] = rotl32(x[4] ^ x[8], 7);
        x[9] += x[13]; x[5] = rotl32(x[5] ^ x[9], 7);
        x[10] += x[14]; x[6] = rotl32(x[6] ^ x[10], 7);
        x[11] += x[15]; x[7] = rotl32(x[7] ^ x[11], 7);

        x[0] += x[5]; x[15] = rotl32(x[15] ^ x[0], 16);
        x[1] += x[6]; x[12] = rotl32(x[12] ^ x[1], 16);
        x[2] += x[7]; x[13] = rotl32(x[13] ^ x[2], 16);
        x[3] += x[4]; x[14] = rotl32(x[14] ^ x[3], 16);

        x[10] += x[15]; x[5] = rotl32(x[5] ^ x[10], 12);
        x[11] += x[12]; x[6] = rotl32(x[6] ^ x[11], 12);
        x[8] += x[13]; x[7] = rotl32(x[7] ^ x[8], 12);
        x[9] += x[14]; x[4] = rotl32(x[4] ^ x[9], 12);

        x[0] += x[5]; x[15] = rotl32(x[15] ^ x[0], 8);
        x[1] += x[6]; x[12] = rotl32(x[12] ^ x[1], 8);
        x[2] += x[7]; x[13] = rotl32(x[13] ^ x[2], 8);
        x[3] += x[4]; x[14] = rotl32(x[14] ^ x[3], 8);

        x[10] += x[15]; x[5] = rotl32(x[5] ^ x[10], 7);
        x[11] += x[12]; x[6] = rotl32(x[6] ^ x[11], 7);
        x[8] += x[13]; x[7] = rotl32(x[7] ^ x[8], 7);
        x[9] += x[14]; x[4] = rotl32(x[4] ^ x[9], 7);
    }

    for (int i = 0; i < 16; i++)
        ctx->keystream32[i] = x[i] + ctx->state[i];

    ctx->state[12]++;
    if (ctx->state[12] == 0)
        ctx->state[13]++;
}

void chacha20_init(struct chacha20_ctx *ctx, const uint8_t key[32], const uint8_t nonce[12],
                   uint32_t counter) {
    static const uint8_t magic[16] = {
        101,120,112,97,110,100,32,51,50,45,98,121,116,101,32,107
    };

    memset(ctx, 0, sizeof(*ctx));

    ctx->state[0] = load32_le(magic + 0);
    ctx->state[1] = load32_le(magic + 4);
    ctx->state[2] = load32_le(magic + 8);
    ctx->state[3] = load32_le(magic + 12);

    for (int i = 0; i < 8; i++)
        ctx->state[4 + i] = load32_le(key + i * 4);

    ctx->state[12] = counter;
    ctx->state[13] = load32_le(nonce + 0);
    ctx->state[14] = load32_le(nonce + 4);
    ctx->state[15] = load32_le(nonce + 8);

    ctx->position = CHACHA20_BLOCK_SIZE;
}

void chacha20_encrypt(struct chacha20_ctx *ctx, uint8_t *data, size_t len) {
    uint8_t *ks = (uint8_t *)ctx->keystream32;

    for (size_t i = 0; i < len; i++) {
        if (ctx->position >= CHACHA20_BLOCK_SIZE) {
            chacha20_next_block(ctx);
            ctx->position = 0;
        }
        data[i] ^= ks[ctx->position];
        ctx->position++;
    }
}

struct chacha20_rng g_chacha20_rng;

static void chacha20_rng_reseed(struct chacha20_rng *rng, const uint8_t seed[32]) {
    uint8_t nonce[12];
    memset(nonce, 0, sizeof(nonce));
    chacha20_init(&rng->inner, seed, nonce, 0);
    rng->counter = 0;
}

void chacha20_rng_init(struct chacha20_rng *rng, const uint8_t seed[32]) {
    chacha20_rng_reseed(rng, seed);
}

void chacha20_rng_bytes(struct chacha20_rng *rng, uint8_t *buf, size_t len) {
    uint8_t *ks = (uint8_t *)rng->inner.keystream32;

    for (size_t i = 0; i < len; i++) {
        if (rng->inner.position >= CHACHA20_BLOCK_SIZE) {
            chacha20_next_block(&rng->inner);
            rng->inner.position = 0;
        }
        buf[i] = ks[rng->inner.position];
        rng->inner.position++;
    }
    rng->counter += len;
}
