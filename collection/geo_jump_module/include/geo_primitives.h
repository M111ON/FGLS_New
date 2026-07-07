#pragma once
#include <stdint.h>

static inline uint64_t _mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static inline uint64_t rotl64_local(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t derive_next_core(uint64_t core, uint8_t face, uint32_t step) {
    uint64_t salt = ((uint64_t)face << 56) ^ (uint64_t)step;
    uint64_t a = _mix64(core ^ salt);
    uint64_t b = rotl64_local(core, (face + step) & 63);
    return _mix64(a ^ b);
}
