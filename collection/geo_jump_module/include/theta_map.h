#pragma once
#include <stdint.h>

typedef struct {
    uint8_t face;
    uint8_t edge;
    uint8_t z;
} ThetaCoord;

#define THETA_MIX_A  UINT64_C(0xff51afd7ed558ccd)
#define THETA_MIX_B  UINT64_C(0xc4ceb9fe1a85ec53)

static inline uint64_t theta_mix64(uint64_t x) {
    x ^= x >> 33;
    x *= THETA_MIX_A;
    x ^= x >> 33;
    x *= THETA_MIX_B;
    x ^= x >> 33;
    return x;
}

static inline ThetaCoord theta_map(uint64_t raw) {
    uint64_t h = theta_mix64(raw);

    uint32_t hi = (uint32_t)(h >> 32);
    uint8_t  face = (uint8_t)(((uint64_t)hi * 12u) >> 32);

    uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
    uint8_t  edge = (uint8_t)(((uint64_t)lo * 5u) >> 32);

    uint8_t  z = (uint8_t)((h >> 16) & 0xFFu);

    return (ThetaCoord){ face, edge, z };
}

static inline void theta_map_batch(const uint64_t *in, ThetaCoord *out, int n) {
    for (int i = 0; i < n; i++)
        out[i] = theta_map(in[i]);
}
