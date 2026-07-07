#pragma once
#include <stdint.h>
#include <string.h>

static uint8_t _GF_EXP[512];
static uint8_t _GF_LOG[256];
static int     _gf256_ready = 0;

static inline void gf256_init(void) {
    if (_gf256_ready) return;
    uint16_t x = 1u;
    for (int i = 0; i < 255; i++) {
        _GF_EXP[i] = (uint8_t)x;
        _GF_LOG[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100u) x ^= 0x11Du;
    }
    for (int i = 255; i < 512; i++)
        _GF_EXP[i] = _GF_EXP[i - 255];
    _GF_LOG[0]   = 0;
    _gf256_ready = 1;
}

static inline uint8_t gf256_mul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0u;
    return _GF_EXP[(uint16_t)_GF_LOG[a] + _GF_LOG[b]];
}

static inline uint8_t gf256_inv(uint8_t a) {
    return _GF_EXP[255u - _GF_LOG[a]];
}

static inline uint8_t gf256_pow(uint8_t a, uint8_t n) {
    if (n == 0u) return 1u;
    if (!a)      return 0u;
    return _GF_EXP[((uint16_t)_GF_LOG[a] * n) % 255u];
}

static inline uint8_t gf256_div(uint8_t a, uint8_t b) {
    if (!a) return 0u;
    return gf256_mul(a, gf256_inv(b));
}

static inline uint8_t gf256_add(uint8_t a, uint8_t b) {
    return a ^ b;
}
