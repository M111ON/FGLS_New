/*
 * wallet_seed_c.c — CPU-only SplitMix64 batch for Python ctypes
 *
 * Compile:
 *   gcc -O3 -shared -o wallet_seed_c.dll wallet_seed_c.c
 */

#include <stdint.h>
#include <string.h>

#define CHUNK_SZ 64
#define EXPORT __declspec(dllexport)

static inline uint64_t splitmix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

EXPORT int wallet_seed_batch_c(
    const uint8_t *chunks,
    uint64_t *seeds,
    int n)
{
    for (int idx = 0; idx < n; idx++) {
        const uint8_t *c = chunks + (size_t)idx * CHUNK_SZ;
        uint64_t acc = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t w = 0;
            for (int j = 0; j < 8; j++) {
                w |= (uint64_t)c[i * 8 + j] << (j * 8);
            }
            acc ^= w;
        }
        seeds[idx] = splitmix64(acc);
    }
    return 0;
}

EXPORT int wallet_xorfold_batch_c(
    const uint8_t *chunks,
    uint32_t *checksums,
    int n)
{
    for (int idx = 0; idx < n; idx++) {
        const uint8_t *c = chunks + (size_t)idx * CHUNK_SZ;
        uint32_t acc = 0;
        for (int i = 0; i < 16; i++) {
            uint32_t w = 0;
            for (int j = 0; j < 4; j++) {
                w |= (uint32_t)c[i * 4 + j] << (j * 8);
            }
            acc ^= w;
        }
        checksums[idx] = acc;
    }
    return 0;
}
