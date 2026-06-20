/*
 * geo_fast_intersect_avx2.h — AVX2 x4/x8 geo_fast_intersect
 * ═══════════════════════════════════════════════════════════
 * Scalar:  raw & ror(raw,8) & ror(raw,16) & ror(raw,24)  — 4 ops/input
 * x4:      same logic, 4 uint64 lanes in 1 __m256i        — 4 ops/4 inputs
 * x8:      2x __m256i unrolled                            — hides latency
 *
 * Requires: -mavx2
 * Include after geo_hardening_whe.h (scalar geo_fast_intersect defined there)
 * ═══════════════════════════════════════════════════════════
 */

#ifndef GEO_FAST_INTERSECT_AVX2_H
#define GEO_FAST_INTERSECT_AVX2_H

#include <stdint.h>
#include "geo_hardening_whe.h"   /* scalar geo_fast_intersect */

#ifdef __AVX2__
#include <immintrin.h>

/* ror 64-bit x4 — rotr(x, k) = (x >> k) | (x << 64-k)
 * reuse _diamond_rotl64_x4 pattern from geo_diamond_field.h */
#define _gfi_rotr64_x4(x, k) \
    _mm256_or_si256(_mm256_srli_epi64((x), (k)), \
                    _mm256_slli_epi64((x), 64-(k)))

/*
 * geo_fast_intersect_x4 — 4 lanes in parallel
 *   in : 4 × uint64_t (aligned or unaligned)
 *   out: 4 × uint64_t
 */
static inline void geo_fast_intersect_x4(const uint64_t in[4], uint64_t out[4])
{
    __m256i raw = _mm256_loadu_si256((const __m256i*)in);
    __m256i r8  = _gfi_rotr64_x4(raw,  8);
    __m256i r16 = _gfi_rotr64_x4(raw, 16);
    __m256i r24 = _gfi_rotr64_x4(raw, 24);
    __m256i res = _mm256_and_si256(raw,
                  _mm256_and_si256(r8,
                  _mm256_and_si256(r16, r24)));
    _mm256_storeu_si256((__m256i*)out, res);
}

/*
 * geo_fast_intersect_x8 — 8 lanes, 2 registers unrolled
 * hides 3-cycle AND latency on Skylake/Zen2
 */
static inline void geo_fast_intersect_x8(const uint64_t in[8], uint64_t out[8])
{
    __m256i raw0 = _mm256_loadu_si256((const __m256i*)in);
    __m256i raw1 = _mm256_loadu_si256((const __m256i*)(in + 4));

    __m256i r8_0  = _gfi_rotr64_x4(raw0,  8);
    __m256i r8_1  = _gfi_rotr64_x4(raw1,  8);
    __m256i r16_0 = _gfi_rotr64_x4(raw0, 16);
    __m256i r16_1 = _gfi_rotr64_x4(raw1, 16);
    __m256i r24_0 = _gfi_rotr64_x4(raw0, 24);
    __m256i r24_1 = _gfi_rotr64_x4(raw1, 24);

    _mm256_storeu_si256((__m256i*)out,
        _mm256_and_si256(raw0,
        _mm256_and_si256(r8_0,
        _mm256_and_si256(r16_0, r24_0))));
    _mm256_storeu_si256((__m256i*)(out + 4),
        _mm256_and_si256(raw1,
        _mm256_and_si256(r8_1,
        _mm256_and_si256(r16_1, r24_1))));
}

/*
 * geo_fast_intersect_batch — process N inputs (N must be multiple of 4)
 * Falls back per-element for tail if N % 4 != 0
 */
static inline void geo_fast_intersect_batch(const uint64_t *in,
                                             uint64_t       *out,
                                             uint32_t        n)
{
    uint32_t i = 0;
    for (; i + 7 < n; i += 8)
        geo_fast_intersect_x8(in + i, out + i);
    for (; i + 3 < n; i += 4)
        geo_fast_intersect_x4(in + i, out + i);
    for (; i < n; i++)
        out[i] = geo_fast_intersect(in[i]);
}

#else  /* no AVX2 — scalar fallback */

static inline void geo_fast_intersect_x4(const uint64_t in[4], uint64_t out[4]) {
    for (int i = 0; i < 4; i++) out[i] = geo_fast_intersect(in[i]);
}
static inline void geo_fast_intersect_x8(const uint64_t in[8], uint64_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = geo_fast_intersect(in[i]);
}
static inline void geo_fast_intersect_batch(const uint64_t *in,
                                             uint64_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) out[i] = geo_fast_intersect(in[i]);
}

#endif /* __AVX2__ */
#endif /* GEO_FAST_INTERSECT_AVX2_H */
