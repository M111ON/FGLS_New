#pragma once
#include <stdint.h>
#include "pogls_fold.h"

#ifndef BRIDGE_MASK
#  define BRIDGE_MASK 0xFFu
typedef struct __attribute__((packed)) {
    uint64_t value;
    uint32_t addr;
} BridgeEntry;
#endif

#ifndef unlikely
#  define unlikely(x) __builtin_expect(!!(x), 0)
#endif

static inline uint32_t diamond_drift_score(const DiamondBlock *b,
                                            uint64_t baseline)
{
    uint64_t intersect = fold_fibo_intersect(b);
    uint64_t lost = baseline & ~intersect;
    return (uint32_t)__builtin_popcountll(lost);
}

static inline int diamond_gate(const DiamondBlock *b,
                                uint64_t baseline,
                                uint32_t *drift_acc)
{
    if (!fold_xor_audit(b)) return 0;

    if ((b->core.raw & 7u) == 0u) {
        uint32_t d = diamond_drift_score(b, baseline);
        *drift_acc += d;
        if (unlikely(*drift_acc > 72u)) {
            *drift_acc = 0;
            return 2;
        }
    }

    return 1;
}

static inline int diamond_route(const DiamondBlock *b)
{
    uint64_t intersect = fold_fibo_intersect(b);
    return (__builtin_popcountll(intersect) & 1u) ? 1 : 2;
}

static inline int diamond_next_active(uint64_t cell_mask, int from_pos)
{
    uint64_t remaining = cell_mask >> from_pos;
    if (!remaining) return 64;
    return from_pos + __builtin_ctzll(remaining);
}

static inline uint32_t diamond_batch4(const DiamondBlock b[4],
                                       uint64_t baseline,
                                       uint32_t *drift_acc,
                                       int routes[4])
{
    uint32_t passed = 0;
    for (int i = 0; i < 4; i++) {
        int g = diamond_gate(&b[i], baseline, drift_acc);
        if (g == 0) { routes[i] = 0; continue; }
        routes[i] = diamond_route(&b[i]);
        passed++;
    }
    return passed;
}

static inline uint64_t diamond_baseline(void)
{
    DiamondBlock ref;
    memset(&ref, 0, sizeof(ref));
    ref.core.raw = 0x0909090909090909ULL;
    ref.invert   = ~ref.core.raw;
    fold_build_quad_mirror(&ref);
    return fold_fibo_intersect(&ref);
}

static inline uint64_t diamond_shift_fwd(uint64_t raw, uint8_t n) {
    n &= 63u;
    return (raw << n) | (raw >> (64u - n));
}
static inline uint64_t diamond_shift_rev(uint64_t raw, uint8_t n) {
    n &= 63u;
    return (raw >> n) | (raw << (64u - n));
}

static inline uint64_t diamond_compress_overlap(uint64_t a, uint64_t b,
                                                  uint8_t *offset_out) {
    uint64_t overlap  = a & b;
    uint64_t unique_a = a & ~b;
    uint64_t unique_b = b & ~a;
    *offset_out = (uint8_t)(__builtin_popcountll(unique_a ^ unique_b) & 0xFFu);
    return overlap | (((uint64_t)*offset_out) << __builtin_popcountll(overlap));
}

static inline void diamond_dna_write(DiamondBlock *b,
                                      uint64_t route_addr,
                                      uint16_t hop_count,
                                      uint8_t  last_offset) {
    HoneycombSlot s;
    memset(&s, 0, sizeof(s));
    s.merkle_root = route_addr;
    s.dna_count   = hop_count;
    s.reserved[0] = last_offset;
    honeycomb_write(b, &s);
}

typedef enum {
    FLOW_CONTINUE = 0,
    FLOW_END_DEAD = 1,
    FLOW_END_DRIFT = 2,
    FLOW_END_RING = 3
} FlowEndReason;

static inline FlowEndReason diamond_flow_end(const DiamondBlock *b,
                                              uint64_t baseline,
                                              uint64_t isect,
                                              uint64_t route_addr,
                                              uint16_t hop_count,
                                              uint32_t drift_acc) {
    FlowEndReason reason = FLOW_CONTINUE;

    if      (isect == 0)         reason = FLOW_END_DEAD;
    else if (drift_acc > 72u)    reason = FLOW_END_DRIFT;

    if (reason != FLOW_CONTINUE) {
        uint8_t offset = (reason == FLOW_END_DEAD)
            ? (uint8_t)__builtin_popcountll(baseline)
            : (uint8_t)(__builtin_popcountll(baseline & ~isect) & 0xFFu);
        diamond_dna_write((DiamondBlock*)b, route_addr, hop_count, offset);
    }
    return reason;
}

typedef struct {
    uint64_t route_addr;
    uint16_t hop_count;
    uint16_t _pad;
    uint32_t drift_acc;
} DiamondFlowCtx;

static inline void diamond_flow_init(DiamondFlowCtx *ctx) {
    ctx->route_addr = 0;
    ctx->hop_count  = 0;
    ctx->_pad       = 0;
    ctx->drift_acc  = 0;
}

#define DIAMOND_ROUTE_P     0x9E3779B185EBCA87ULL
#define DIAMOND_ROUTE_P_INV 0x0887493432BADB37ULL
#define DIAMOND_ROUTE_K     7u
#ifndef DIAMOND_HOP_MAX
#  define DIAMOND_HOP_MAX 60000u
#endif

static inline uint64_t diamond_route_update(uint64_t r, uint64_t intersect) {
    return ((r << DIAMOND_ROUTE_K) | (r >> (64u - DIAMOND_ROUTE_K)))
           ^ (intersect * DIAMOND_ROUTE_P);
}

static inline uint32_t diamond_batch_run(
    DiamondBlock      *cells,
    uint32_t           n,
    uint64_t           baseline,
    DiamondFlowCtx    *ctx)
{
    uint32_t dna_writes = 0;

    for (uint32_t i = 0; i < n; i++) {
        if (!fold_xor_audit(&cells[i])) continue;

        uint64_t isect = fold_fibo_intersect(&cells[i]);

        uint64_t r_next = diamond_route_update(ctx->route_addr, isect);

        if ((cells[i].core.raw & 7u) == 0u) {
            ctx->drift_acc += (uint32_t)__builtin_popcountll(baseline & ~isect);
        }

        FlowEndReason reason = diamond_flow_end(&cells[i], baseline, isect,
                                                 r_next, ctx->hop_count,
                                                 ctx->drift_acc);
        if (unlikely(reason != FLOW_CONTINUE)) {
            dna_writes++;
            ctx->route_addr = 0; ctx->hop_count = 0; ctx->drift_acc = 0;
            continue;
        }

        ctx->route_addr = r_next;
        ctx->hop_count++;
    }

    return dna_writes;
}

static inline void diamond_ctx_push_temp(
    BridgeEntry    *temp_ring,
    uint32_t       *temp_head,
    const DiamondFlowCtx *ctx)
{
    uint32_t i = (*temp_head) & BRIDGE_MASK;
    temp_ring[i].value = ctx->route_addr;
    uint8_t flags = 0u;
    if (ctx->drift_acc > 72u)  flags |= 0x1u;
    uint8_t drift_packed = (uint8_t)(ctx->drift_acc > 254u ? 254u : ctx->drift_acc);
    temp_ring[i].addr  = ((uint32_t)ctx->hop_count & 0xFFFFu)
                       | ((uint32_t)drift_packed   << 16)
                       | (0xD0u                    << 24)
                       | ((uint32_t)(flags & 0xFu) << 20);
    (*temp_head)++;
}

static inline int diamond_ctx_pop_temp(
    BridgeEntry    *temp_ring,
    uint32_t       *temp_tail,
    uint32_t        temp_head,
    DiamondFlowCtx *ctx_out)
{
    if (*temp_tail == temp_head) return 0;
    uint32_t i = (*temp_tail) & BRIDGE_MASK;
    if ((temp_ring[i].addr >> 28) != 0xDu) return 0;
    ctx_out->route_addr = temp_ring[i].value;
    ctx_out->hop_count  = (uint16_t)(temp_ring[i].addr & 0xFFFFu);
    ctx_out->drift_acc  = (uint32_t)((temp_ring[i].addr >> 16) & 0xFFu);
    ctx_out->_pad       = 0;
    (*temp_tail)++;
    return 1;
}

static inline uint32_t diamond_batch_temporal(
    DiamondBlock      *cells,
    uint32_t           n,
    uint64_t           baseline,
    DiamondFlowCtx    *ctx,
    BridgeEntry       *temp_ring,
    uint32_t          *temp_head,
    uint32_t          *temp_tail)
{
    DiamondFlowCtx resume;
    if (diamond_ctx_pop_temp(temp_ring, temp_tail, *temp_head, &resume)) {
        ctx->route_addr  = resume.route_addr;
        ctx->hop_count  += resume.hop_count;
        ctx->drift_acc  += resume.drift_acc;
    }

    uint32_t dna_writes = diamond_batch_run(cells, n, baseline, ctx);

    if (ctx->hop_count > 0) {
        uint32_t used = (*temp_head - *temp_tail) & BRIDGE_MASK;
        int overflow = (used >= (uint32_t)(BRIDGE_MASK - 1)) || (ctx->drift_acc > 254u);
        if (overflow && n > 0) {
            diamond_dna_write(&cells[n-1], ctx->route_addr,
                              ctx->hop_count, (uint8_t)(ctx->drift_acc & 0xFFu));
        } else {
            diamond_ctx_push_temp(temp_ring, temp_head, ctx);
        }
        diamond_flow_init(ctx);
    }

    return dna_writes;
}

typedef struct {
    uint64_t route_addr;
    uint16_t hop_count;
    uint8_t  offset;
    uint8_t  valid;
} DiamondReplay;

static inline DiamondReplay diamond_replay(const DiamondBlock *b) {
    DiamondReplay r;
    HoneycombSlot s = honeycomb_read(b);
    r.valid      = (s.dna_count > 0 || s.merkle_root != 0) ? 1u : 0u;
    r.route_addr = s.merkle_root;
    r.hop_count  = s.dna_count;
    r.offset     = s.reserved[0];
    return r;
}

static inline uint64_t diamond_replay_step(uint64_t curr, uint64_t prev) {
    uint64_t x = (prev << DIAMOND_ROUTE_K) | (prev >> (64u - DIAMOND_ROUTE_K));
    return (curr ^ x) * DIAMOND_ROUTE_P_INV;
}

#ifdef __AVX2__
#include <immintrin.h>

#define _diamond_rotl64_x4(x, k) \
    _mm256_or_si256(_mm256_slli_epi64((x), (k)), \
                    _mm256_srli_epi64((x), 64-(k)))

static inline __m256i _diamond_mul64lo_x4(__m256i a, __m256i b) {
    const __m256i mask32 = _mm256_set1_epi64x(0xFFFFFFFFULL);
    __m256i a_lo = _mm256_and_si256(a, mask32);
    __m256i a_hi = _mm256_srli_epi64(a, 32);
    __m256i b_lo = _mm256_and_si256(b, mask32);
    __m256i b_hi = _mm256_srli_epi64(b, 32);

    __m256i lo_lo = _mm256_mul_epu32(a_lo, b_lo);
    __m256i mid   = _mm256_add_epi64(_mm256_mul_epu32(a_hi, b_lo),
                                     _mm256_mul_epu32(a_lo, b_hi));
    return _mm256_add_epi64(lo_lo, _mm256_slli_epi64(mid, 32));
}

static inline __m256i diamond_replay_step_x4(__m256i curr, __m256i prev) {
    __m256i x    = _diamond_rotl64_x4(prev, DIAMOND_ROUTE_K);
    __m256i diff = _mm256_xor_si256(curr, x);
    __m256i pinv = _mm256_set1_epi64x((int64_t)DIAMOND_ROUTE_P_INV);
    return _diamond_mul64lo_x4(diff, pinv);
}

static inline void diamond_replay_step_x8(
    const uint64_t curr[8], const uint64_t prev[8], uint64_t out[8])
{
    __m256i c0 = _mm256_loadu_si256((const __m256i*)curr);
    __m256i c1 = _mm256_loadu_si256((const __m256i*)(curr+4));
    __m256i p0 = _mm256_loadu_si256((const __m256i*)prev);
    __m256i p1 = _mm256_loadu_si256((const __m256i*)(prev+4));
    _mm256_storeu_si256((__m256i*)out,   diamond_replay_step_x4(c0, p0));
    _mm256_storeu_si256((__m256i*)(out+4), diamond_replay_step_x4(c1, p1));
}

#endif

#ifdef __AVX2__

typedef struct {
    uint64_t route_addr[4];
    uint64_t hop_count[4];
    uint64_t drift_acc[4];
    uint64_t snap_route[4];
    uint64_t snap_hop[4];
    uint64_t snap_drift[4];
} DiamondFlowCtx4;

static inline void diamond_flow4_init(DiamondFlowCtx4 *c) {
    _mm256_storeu_si256((__m256i*)c->route_addr, _mm256_setzero_si256());
    _mm256_storeu_si256((__m256i*)c->hop_count,  _mm256_setzero_si256());
    _mm256_storeu_si256((__m256i*)c->drift_acc,  _mm256_setzero_si256());
}

static inline uint32_t diamond_batch_temporal_x4(
    DiamondFlowCtx4   *ctx,
    const uint64_t    *isect4,
    uint32_t           steps,
    uint64_t           baseline)
{
    __m256i r   = _mm256_loadu_si256((const __m256i*)ctx->route_addr);
    __m256i hop = _mm256_loadu_si256((const __m256i*)ctx->hop_count);
    __m256i drf = _mm256_loadu_si256((const __m256i*)ctx->drift_acc);

    const __m256i Pv      = _mm256_set1_epi64x((int64_t)DIAMOND_ROUTE_P);
    const __m256i one     = _mm256_set1_epi64x(1);
    const __m256i thr72   = _mm256_set1_epi64x(72);
    const __m256i zero    = _mm256_setzero_si256();
    uint32_t      resets  = 0;

    for (uint32_t i = 0; i < steps; i++) {
        __m256i i4 = _mm256_loadu_si256((const __m256i*)(isect4 + i*4));

        __m256i rot = _mm256_or_si256(_mm256_slli_epi64(r, DIAMOND_ROUTE_K),
                                      _mm256_srli_epi64(r, 64 - DIAMOND_ROUTE_K));
        r = _mm256_xor_si256(rot, _diamond_mul64lo_x4(i4, Pv));

        hop = _mm256_add_epi64(hop, one);

        if ((i & 7u) == 0u) {
            uint64_t tmp[4]; _mm256_storeu_si256((__m256i*)tmp, i4);
            uint64_t bl[4];  _mm256_storeu_si256((__m256i*)bl,
                _mm256_andnot_si256(i4, _mm256_set1_epi64x((int64_t)baseline)));
            uint64_t d[4];   _mm256_storeu_si256((__m256i*)d, drf);
            d[0] += (uint64_t)__builtin_popcountll(bl[0]);
            d[1] += (uint64_t)__builtin_popcountll(bl[1]);
            d[2] += (uint64_t)__builtin_popcountll(bl[2]);
            d[3] += (uint64_t)__builtin_popcountll(bl[3]);
            drf = _mm256_loadu_si256((const __m256i*)d);
        }

        __m256i dead  = _mm256_cmpeq_epi64(i4, zero);
        __m256i drift_over = _mm256_cmpgt_epi64(drf, thr72);
        __m256i end   = _mm256_or_si256(dead, drift_over);

        if (unlikely(!_mm256_testz_si256(end, end))) {
            uint64_t em[4]; _mm256_storeu_si256((__m256i*)em, end);
            resets |= ((em[0]!=0u)<<0) | ((em[1]!=0u)<<1)
                    | ((em[2]!=0u)<<2) | ((em[3]!=0u)<<3);
            _mm256_storeu_si256((__m256i*)ctx->snap_route, r);
            _mm256_storeu_si256((__m256i*)ctx->snap_hop,   hop);
            _mm256_storeu_si256((__m256i*)ctx->snap_drift, drf);
            r   = _mm256_blendv_epi8(r,   zero, end);
            hop = _mm256_blendv_epi8(hop, zero, end);
            drf = _mm256_blendv_epi8(drf, zero, end);
        }
    }

    _mm256_storeu_si256((__m256i*)ctx->route_addr, r);
    _mm256_storeu_si256((__m256i*)ctx->hop_count,  hop);
    _mm256_storeu_si256((__m256i*)ctx->drift_acc,  drf);
    return resets;
}
#endif
