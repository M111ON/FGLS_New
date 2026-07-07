#pragma once
#include <stdint.h>
#include "pogls_fold.h"
#include "geo_thirdeye.h"

#define FIBO_PERIOD_SIG       17u
#define FIBO_PERIOD_DRIFT     72u
#define FIBO_PERIOD_FLUSH    144u
#define FIBO_PERIOD_SNAP     720u

#define FIBO_BRIDGE_SQ    20736u
#define FIBO_CPU_WORLD      128u
#define FIBO_ICOSA_WORLD    162u

typedef uint8_t FiboEvent;
#define FIBO_EV_NONE        0x00
#define FIBO_EV_SIG_FAIL    0x01
#define FIBO_EV_DRIFT       0x02
#define FIBO_EV_FLUSH       0x04
#define FIBO_EV_SNAP        0x08
#define FIBO_EV_CROSS_DRIFT 0x10
#define FIBO_EV_TE_STRESSED 0x20
#define FIBO_EV_TE_ANOMALY  0x40

typedef struct {
    uint64_t sum8;
    uint64_t sum9;
} FiboDualCh;

typedef struct {
    uint16_t c17;
    uint8_t  c72;
    uint8_t  c144;
    uint16_t c720;
} FiboClockCtx;

typedef struct {
    FiboDualCh   dual;
    FiboClockCtx clk;
    uint64_t     prev_delta;
    uint64_t     prev_window_inc;
    GeoSeed      seed;
    ThirdEye     eye;
    uint32_t     prev_fx;
} FiboCtx;

static inline void fibo_ctx_init(FiboCtx *ctx)
{
    ctx->dual.sum8 = 0;
    ctx->dual.sum9 = 0;

    ctx->clk.c17  = FIBO_PERIOD_SIG;
    ctx->clk.c72  = FIBO_PERIOD_DRIFT;
    ctx->clk.c144 = FIBO_PERIOD_FLUSH;
    ctx->clk.c720 = FIBO_PERIOD_SNAP;

    ctx->prev_delta      = 0;
    ctx->prev_window_inc = 0;
    ctx->prev_fx         = 0;

    ctx->seed.gen2 = 0;
    ctx->seed.gen3 = 0;

    te_init(&ctx->eye, ctx->seed);
}

#define GEOSEED_GOLDEN   0x9E3779B97F4A7C15ULL
#define GEOSEED_PAIR_MIX 0x6C62272E07BB0142ULL

static inline void fibo_ctx_set_seed(FiboCtx *ctx, GeoSeed seed)
{
    uint64_t topo = seed.gen2 & 0x7FFFu;

    if (topo == 0) {
        topo = (GEOSEED_GOLDEN & 0x7FFFu) | 0x0001u;
        seed.gen2 = (seed.gen2 & ~0x7FFFu) | topo;
    }

    uint16_t parity = (uint16_t)(__builtin_popcountll(topo) & 0xFFFFu);
    parity ^= (uint16_t)(GEOSEED_PAIR_MIX & 0xFFFFu);

    uint8_t  face_id     = (uint8_t)((topo >> 11) & 0xFu);
    uint8_t  vertex_mask = (uint8_t)((topo >>  6) & 0x1Fu);
    uint8_t  edge_mask   = (uint8_t)((topo >>  1) & 0x1Fu);
    uint64_t ve_bias     = (uint64_t)((vertex_mask * 8u) ^ (edge_mask * 9u));
    uint64_t entropy     = (GEOSEED_GOLDEN ^ (face_id * GEOSEED_GOLDEN)) ^ ve_bias;

    seed.gen2 = topo
              | ((uint64_t)parity  << 33)
              | ((entropy & 0x1FFFFFFFFULL) << 15);

    if (seed.gen2 == 0) seed.gen2 = GEOSEED_GOLDEN;

    if (seed.gen3 == 0) seed.gen3 = seed.gen2 ^ GEOSEED_GOLDEN;

    ctx->seed = seed;
    te_init(&ctx->eye, seed);
}

static inline uint32_t fibo_fx_full(const DiamondBlock *b, uint16_t hop)
{
    uint32_t bits = (uint32_t)__builtin_popcountll(b->core.raw);
    uint32_t gear = (uint32_t)core_fibo_gear(b->core);
    return bits + hop + gear;
}

static inline uint32_t fibo_fx_fast(const DiamondBlock *b)
{
    return (uint32_t)__builtin_popcountll(b->core.raw);
}

static inline void fibo_accum(FiboDualCh *d, uint32_t fx)
{
    d->sum8 += (uint64_t)fx << 3;
    d->sum9 += (uint64_t)fx * 9u;
}

static inline uint8_t fibo_drift_type(uint64_t delta, uint64_t prev)
{
    if (delta >= prev) return 0;

    int64_t diff = (int64_t)(prev - delta);

    int t = (diff % 9 == 0);
    int s = (diff % 8 == 0);

    return (t && !s) ? 1 :
           (s && !t) ? 2 : 3;
}

static inline uint64_t fibo_sig_encode(uint64_t N)
{
    return (N << 4) + N + 0xFFFFFFFFFFFFFFF7ULL;
}

static inline int fibo_sig_verify(uint64_t sig)
{
    return ((sig + 9) % 17) == 0;
}

static inline uint64_t fibo_sig_decode(uint64_t sig)
{
    return (sig + 9) / 17;
}

static inline int fibo_cross_check(uint64_t cpu_val, uint64_t icosa_val)
{
    return (cpu_val * FIBO_ICOSA_WORLD) == (icosa_val * FIBO_CPU_WORLD);
}

static inline uint64_t fibo_cpu_to_icosa(uint64_t cpu_val)
{
    return cpu_val * FIBO_ICOSA_WORLD / FIBO_CPU_WORLD;
}

static inline uint64_t fibo_icosa_to_cpu(uint64_t icosa_val)
{
    return icosa_val * FIBO_CPU_WORLD / FIBO_ICOSA_WORLD;
}

static inline FiboEvent fibo_clock_tick(FiboCtx *ctx, uint32_t fx)
{
    FiboEvent ev = FIBO_EV_NONE;

    fibo_accum(&ctx->dual, fx);

    uint32_t drift = (fx > ctx->prev_fx) ? (fx - ctx->prev_fx)
                                         : (ctx->prev_fx - fx);
    ctx->prev_fx = fx;

    uint8_t spoke    = (uint8_t)(ctx->seed.gen2 & 0x7u) % 6u;
    uint8_t slot_hot = (uint8_t)(drift > 4u ? 1u : 0u);

    if (--ctx->clk.c17 == 0) {
        ctx->clk.c17 = FIBO_PERIOD_SIG;
        if (!fibo_sig_verify(ctx->seed.gen2)) {
            ev |= FIBO_EV_SIG_FAIL;
        }
    }

    if (--ctx->clk.c72 == 0) {
        ctx->clk.c72 = FIBO_PERIOD_DRIFT;

        uint64_t delta     = ctx->dual.sum9 - ctx->dual.sum8;
        uint64_t increment = delta - ctx->prev_delta;

        if (ctx->prev_window_inc > 0 && increment < ctx->prev_window_inc) {
            ev |= FIBO_EV_DRIFT;
        }

        ctx->prev_window_inc = increment;
        ctx->prev_delta      = delta;
    }

    if (--ctx->clk.c144 == 0) {
        ctx->clk.c144 = FIBO_PERIOD_FLUSH;
        ev |= FIBO_EV_FLUSH;

        te_tick(&ctx->eye, ctx->seed, spoke, slot_hot, drift);

        if (ctx->eye.qrpn_state == QRPN_ANOMALY)
            ev |= FIBO_EV_TE_ANOMALY;
        else if (ctx->eye.qrpn_state == QRPN_STRESSED)
            ev |= FIBO_EV_TE_STRESSED;
    } else {
        te_tick(&ctx->eye, ctx->seed, spoke, slot_hot, drift);
    }

    if (--ctx->clk.c720 == 0) {
        ctx->clk.c720 = FIBO_PERIOD_SNAP;
        ev |= FIBO_EV_SNAP;
    }

    return ev;
}

static inline uint8_t fibo_mirror_mask(const FiboCtx *ctx)
{
    uint8_t spoke = (uint8_t)(ctx->seed.gen2 & 0x7u) % 6u;
    return te_get_mask(&ctx->eye, spoke);
}

static inline uint8_t fibo_qrpn_state(const FiboCtx *ctx)
{
    return ctx->eye.qrpn_state;
}

static inline void fibo_dual_reset(FiboDualCh *d)
{
    d->sum8 = 0;
    d->sum9 = 0;
}

static inline FiboEvent fibo_hop(FiboCtx           *ctx,
                                  const DiamondBlock *b,
                                  uint16_t            hop)
{
    uint32_t fx = fibo_fx_full(b, hop);
    return fibo_clock_tick(ctx, fx);
}

static inline FiboEvent fibo_hop_fast(FiboCtx           *ctx,
                                       const DiamondBlock *b)
{
    uint32_t fx = fibo_fx_fast(b);
    return fibo_clock_tick(ctx, fx);
}
