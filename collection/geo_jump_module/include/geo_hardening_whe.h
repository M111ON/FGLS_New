#pragma once
#include <stdint.h>
#include "geo_diamond_field.h"
#include "theta_map.h"
#include "geo_route.h"
#include "pogls_fold.h"
#include "geo_whe.h"
#include "geo_read.h"

#undef  spawn_pool_init
static inline void spawn_pool_init_fixed(SpawnPool *p)
{
    p->used = 0;
    for (uint8_t i = 0; i < TORUS_POOL_SIZE; i++)
        p->pool[i] = TORUS_BLUEPRINT;
}
#define spawn_pool_init spawn_pool_init_fixed

#define GEO_SEED_GUARD  UINT64_C(0x9E3779B97F4A7C15)
#define TOPO_MASK       UINT64_C(0x0000FFFFFFFFFFFF)

static inline uint64_t geo_route_addr_guard(uint64_t route_addr,
                                              uint64_t isect)
{
    if ((route_addr & TOPO_MASK) == 0)
        route_addr ^= GEO_SEED_GUARD ^ isect;
    return route_addr;
}

static inline uint64_t geo_fast_intersect(uint64_t raw) {
    uint64_t r8  = (raw >> 8)  | (raw << 56);
    uint64_t r16 = (raw >> 16) | (raw << 48);
    uint64_t r24 = (raw >> 24) | (raw << 40);
    return raw & r8 & r16 & r24;
}

static inline int geo_fast_audit(uint64_t raw) {
    return (raw ^ ~raw) == 0xFFFFFFFFFFFFFFFFULL;
}

static inline uint32_t geo_fused_write_batch(
    const uint64_t    *raw_in,
    uint32_t           n,
    uint64_t           baseline,
    DiamondFlowCtx    *ctx,
    DodecaTable       *dodeca,
    uint8_t            segment,
    WheCtx            *whe,
    uint32_t           step_offset)
{
    uint32_t dna_writes = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t h  = theta_mix64(raw_in[i]);
        uint32_t hi = (uint32_t)(h >> 32);
        uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
        uint8_t  face = (uint8_t)(((uint64_t)hi * 12u) >> 32);
        uint8_t  edge = (uint8_t)(((uint64_t)lo *  5u) >> 32);

        uint64_t core_raw = ((uint64_t)face << 59)
                          | ((uint64_t)edge << 52)
                          | (raw_in[i] & UINT64_C(0x000FFFFFFFFFFFFF));

        uint64_t isect = geo_fast_intersect(core_raw);

        if ((core_raw & 7u) == 0u)
            ctx->drift_acc += (uint32_t)__builtin_popcountll(baseline & ~isect);

        uint64_t r_next = diamond_route_update(ctx->route_addr, isect);
        r_next = geo_route_addr_guard(r_next, isect);

        if (whe) {
            uint64_t expected_r = ((uint64_t)face << 56)
                                | ((uint64_t)edge << 48)
                                | (uint8_t)((h >> 16) & 0xFFu);
            whe_step(whe, edge, expected_r, r_next, ctx->drift_acc, step_offset + i);
        }

        int at_end = (isect == 0) || (ctx->drift_acc > 72u)
                  || (ctx->hop_count >= DIAMOND_HOP_MAX);

        if (at_end) {
            uint8_t offset = (uint8_t)(__builtin_popcountll(baseline & ~isect) & 0xFF);
            if (whe) {
                uint64_t fp = whe_final_fp(whe, r_next, ctx->hop_count);
                offset ^= (uint8_t)(fp & 0xFFu);
                whe_reset(whe);
            }
            dodeca_insert(dodeca, r_next, 0, 0, offset, ctx->hop_count, segment);
            dna_writes++;
            ctx->route_addr = 0;
            ctx->hop_count  = 0;
            ctx->drift_acc  = 0;
            continue;
        }

        ctx->route_addr = r_next;
        ctx->hop_count++;
    }

    return dna_writes;
}

static inline int geo_hardening_selftest(void) {
    uint64_t guarded = geo_route_addr_guard(0ULL, 0ULL);
    if (guarded == 0) return 0;

    uint64_t nonzero = 0x0000ABCD12345678ULL;
    uint64_t result  = geo_route_addr_guard(nonzero, 0ULL);
    if (result != nonzero) return 0;

    uint64_t raw[64];
    for (int i = 0; i < 64; i++)
        raw[i] = (uint64_t)i * 0x9E3779B185EBCA87ULL ^ 0x000000771e050000ULL;

    uint64_t baseline = diamond_baseline();
    DodecaTable dodeca; dodeca_init(&dodeca);
    DiamondFlowCtx ctx; diamond_flow_init(&ctx);
    uint32_t dna = geo_fused_write_batch(raw, 64, baseline, &ctx, &dodeca, 0, NULL, 0);

    return (dna > 0);
}
