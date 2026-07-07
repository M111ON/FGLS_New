#pragma once
#include <stdint.h>
#include "geo_config_engine.h"
#include "theta_map.h"

#define WHE_PHI_PRIME       UINT64_C(0x9E3779B97F4A7C15)

#define WHE_SUSPICIOUS_K    GEO_TE_CYCLE

#define WHE_PENTA_BASE      0x1Au

typedef struct {
    uint64_t violation_bits;
    uint32_t drift_sum;
    uint32_t hit_count;
} FingerprintCtx;

typedef struct {
    uint64_t fp;
    uint64_t tail_base;
    uint32_t tail_start_step;
    uint8_t  tail_on;
    uint8_t  suspicious;
    uint8_t  _pad[2];
} TailFP;

typedef struct {
    FingerprintCtx fp_ctx;
    TailFP         tail;
    uint64_t       mirror_fp;
    uint32_t       anomaly;
    uint32_t       step_count;
} WheCtx;

typedef char _whe_ctx_size_check[sizeof(WheCtx) <= 128 ? 1 : -1];

static inline uint8_t whe_forbidden_mask(uint32_t step, uint64_t expected_route)
{
    uint32_t s   = step ^ (uint32_t)(expected_route >> 32);
    uint8_t  rot = (uint8_t)(s % 5u);
    return (uint8_t)(((WHE_PENTA_BASE << rot) | (WHE_PENTA_BASE >> (5u - rot))) & 0x1Fu);
}

static inline uint8_t whe_geo_to_triangle(uint8_t edge)
{
    return edge % 5u;
}

static inline uint8_t whe_detect_violation(uint8_t actual_edge,
                                            uint8_t forbidden_mask)
{
    uint8_t tri = whe_geo_to_triangle(actual_edge);
    return (uint8_t)((forbidden_mask >> tri) & 1u);
}

static inline void whe_fp_update(FingerprintCtx *fp,
                                  uint8_t         violation,
                                  uint32_t        drift,
                                  uint32_t        step)
{
    if (violation) {
        fp->violation_bits ^= (UINT64_C(1) << (step & 63u));
        fp->hit_count++;
    }
    fp->drift_sum += drift;
}

static inline void whe_tail_update(TailFP   *t,
                                    uint64_t  expected,
                                    uint64_t  actual,
                                    uint32_t  drift,
                                    uint32_t  step)
{
    if (!t->tail_on && (actual != expected)) {
        t->tail_on         = 1;
        t->tail_base       = expected ^ (uint64_t)step;
        t->tail_start_step = step;
        t->suspicious      = 0;
    }

    if (t->tail_on) {
        uint64_t offset = actual ^ t->tail_base;

        t->fp ^= (offset * WHE_PHI_PRIME)
              ^ ((uint64_t)step << 1)
              ^ ((uint64_t)drift << (step & 31u));

        if (offset == 0u) {
            t->tail_on = 0;
            return;
        }

        if ((step - t->tail_start_step) > WHE_SUSPICIOUS_K) {
            t->suspicious = 1;
        }
    }
}

static inline void whe_split(const WheCtx *parent, WheCtx *child)
{
    *child = *parent;
}

static inline void whe_merge(WheCtx *parent, const WheCtx *child)
{
    parent->tail.fp               ^= (child->tail.fp * WHE_PHI_PRIME);
    parent->fp_ctx.violation_bits ^= child->fp_ctx.violation_bits;
    parent->fp_ctx.drift_sum      += child->fp_ctx.drift_sum;
    parent->fp_ctx.hit_count      += child->fp_ctx.hit_count;
    parent->anomaly               += child->anomaly;
}

static inline uint8_t whe_mirror_check(WheCtx   *ctx,
                                        uint64_t  fp_left,
                                        uint64_t  fp_right)
{
    if (fp_left != fp_right) {
        ctx->anomaly++;
        ctx->mirror_fp = fp_left ^ fp_right;
        return 1;
    }
    return 0;
}

static inline void whe_step(WheCtx   *ctx,
                              uint8_t   actual_edge,
                              uint64_t  expected_route,
                              uint64_t  actual_route,
                              uint32_t  drift,
                              uint32_t  step)
{
    uint8_t forbidden = whe_forbidden_mask(step, expected_route);

    uint8_t violation = whe_detect_violation(actual_edge, forbidden);

    whe_fp_update(&ctx->fp_ctx, violation, drift, step);

    whe_tail_update(&ctx->tail, expected_route, actual_route, drift, step);

    ctx->step_count++;
}

static inline uint64_t whe_violation_fp(const FingerprintCtx *fp)
{
    return fp->violation_bits
         ^ ((uint64_t)fp->drift_sum  << 32)
         ^ (uint64_t)fp->hit_count;
}

static inline uint64_t whe_final_fp(const WheCtx *ctx,
                                     uint64_t      actual_route,
                                     uint32_t      hop_count)
{
    uint64_t v_fp = whe_violation_fp(&ctx->fp_ctx);
    return v_fp
         ^ ctx->tail.fp
         ^ actual_route
         ^ (uint64_t)hop_count;
}

static inline void whe_init(WheCtx *ctx)
{
    __builtin_memset(ctx, 0, sizeof(WheCtx));
}

static inline void whe_reset(WheCtx *ctx)
{
    whe_init(ctx);
}

static inline uint8_t whe_tail_active(const WheCtx *ctx)
{
    return ctx->tail.tail_on;
}

static inline uint8_t whe_is_suspicious(const WheCtx *ctx)
{
    return ctx->tail.suspicious;
}

static inline uint8_t whe_has_anomaly(const WheCtx *ctx)
{
    return (ctx->anomaly > 0u) ? 1u : 0u;
}

static inline uint32_t whe_violation_rate_fp8(const WheCtx *ctx)
{
    if (ctx->step_count == 0u) return 0u;
    return (ctx->fp_ctx.hit_count << 8) / ctx->step_count;
}

static inline int whe_selftest(void)
{
    uint8_t m_a = whe_forbidden_mask(0, 0x0000000000000000ULL);
    uint8_t m_b = whe_forbidden_mask(0, 0xDEADBEEF00000000ULL);
    if (m_a == m_b) return 0;

    uint64_t er = 0xABCD000000000000ULL;
    uint8_t m0  = whe_forbidden_mask(0, er);
    uint8_t m5  = whe_forbidden_mask(5, er);
    if (m0 != m5) return 0;

    uint8_t tri_forbidden = (uint8_t)__builtin_ctz(m0 & 0x1Fu);
    if (!whe_detect_violation(tri_forbidden, m0)) return 0;

    WheCtx ctx; whe_init(&ctx);
    whe_step(&ctx, 0, 0xABCDEFULL, 0xABCDEFULL, 0, 0);
    if (whe_tail_active(&ctx)) return 0;

    whe_step(&ctx, 0, 0xABCDEFULL, 0x000001ULL, 0, 1);
    if (!whe_tail_active(&ctx)) return 0;

    uint64_t tail_base_at1 = 0xABCDEFULL ^ (uint64_t)1;
    whe_step(&ctx, 0, 0xABCDEFULL, tail_base_at1, 0, 2);
    if (whe_tail_active(&ctx)) return 0;

    WheCtx parent; whe_init(&parent);
    whe_step(&parent, 1, 0x100ULL, 0x200ULL, 5, 10);
    WheCtx child; whe_split(&parent, &child);

    uint64_t fp_before = parent.tail.fp;
    whe_merge(&parent, &child);
    if (parent.tail.fp == fp_before && child.tail.fp != 0) return 0;

    WheCtx c1; whe_init(&c1);
    WheCtx c2; whe_init(&c2);
    whe_step(&c1, 1, 0x100ULL, 0x200ULL,  0, 5);
    whe_step(&c2, 1, 0x100ULL, 0x200ULL, 99, 5);
    if (whe_final_fp(&c1, 0, 0) == whe_final_fp(&c2, 0, 0)) return 0;

    WheCtx ctx2; whe_init(&ctx2);
    if (!whe_mirror_check(&ctx2, 0xAAAAULL, 0xBBBBULL)) return 0;

    return 1;
}
