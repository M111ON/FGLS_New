#ifndef ICOSA_TWIN_BRIDGE_H
#define ICOSA_TWIN_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define ICOSA_FACES          20u
#define ICOSA_EDGES           3u
#define ICOSA_TOTAL_UNITS    (ICOSA_FACES * ICOSA_EDGES)

#define ICOSA_CPU_WORLD      128u
#define ICOSA_GPU_WORLD      162u

#define ICOSA_GPU_BATCH      65536u
#define ICOSA_GPU_TPB         256u
#define ICOSA_GPU_MAX_STAGES    8u
#define ICOSA_GPU_BUNDLE_WORDS  9u

#define ICOSA_EV_NONE        0x00u
#define ICOSA_EV_FLUSH       0x01u
#define ICOSA_EV_BOUNDARY    0x02u

typedef struct {
    uint8_t face;  /* 0..19  icosa face */
    uint8_t edge;  /* 0..2   triangle edge */
    uint8_t z;     /* 0..255 torus layer */
} IcosaThetaCoord;

static inline uint64_t icosa_theta_mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static inline IcosaThetaCoord icosa_theta_map(uint64_t raw) {
    uint64_t h = icosa_theta_mix64(raw);
    uint32_t hi = (uint32_t)(h >> 32);
    uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);
    IcosaThetaCoord t;
    t.face = (uint8_t)(((uint64_t)hi * ICOSA_FACES) >> 32);
    t.edge = (uint8_t)(((uint64_t)lo * ICOSA_EDGES) >> 32);
    t.z    = (uint8_t)((h >> 16) & 0xFFu);
    return t;
}

static inline uint64_t icosa_fast_intersect(uint64_t core_raw) {
    uint64_t r8  = (core_raw >> 8)  | (core_raw << 56);
    uint64_t r16 = (core_raw >> 16) | (core_raw << 48);
    uint64_t r24 = (core_raw >> 24) | (core_raw << 40);
    return core_raw & r8 & r16 & r24;
}

static inline uint64_t icosa_route_update(uint64_t route, uint64_t isect) {
    uint64_t r = route ^ isect;
    if ((r & 0x0000FFFFFFFFFFFFULL) == 0)
        r ^= 0x9E3779B97F4A7C15ULL ^ isect;
    return r;
}

static inline uint64_t icosa_cpu_to_gpu(uint64_t cpu_val) {
    return cpu_val * ICOSA_GPU_WORLD / ICOSA_CPU_WORLD;
}

static inline uint64_t icosa_gpu_to_cpu(uint64_t gpu_val) {
    return gpu_val * ICOSA_CPU_WORLD / ICOSA_GPU_WORLD;
}

/* Per-op icosa lane compute (CPU fallback — mirrors GPU kernel) */
typedef struct {
    uint64_t route_addr;
    uint32_t drift_acc;
    uint16_t hop_count;
    uint8_t  _pad[2];
} IcosaFlowState;

static inline void icosa_flow_init(IcosaFlowState *s) {
    s->route_addr = 0;
    s->drift_acc  = 0;
    s->hop_count  = 0;
}

static inline uint8_t icosa_lane_process(
    IcosaFlowState *s,
    uint64_t        raw,
    uint64_t        baseline,
    uint64_t       *out_route,
    uint8_t        *out_offset)
{
    uint64_t h  = icosa_theta_mix64(raw);
    uint32_t hi = (uint32_t)(h >> 32);
    uint32_t lo = (uint32_t)(h & 0xFFFFFFFFu);

    uint64_t core_raw = ((uint64_t)hi << 32)
                      | ((uint64_t)lo & 0x000FFFFFFFFFFFFFULL);
    uint64_t isect = icosa_fast_intersect(core_raw);

    if ((core_raw & 7u) == 0u) {
        uint64_t _d = baseline & ~isect;
        _d = _d - ((_d >> 1) & 0x5555555555555555ULL);
        _d = (_d & 0x3333333333333333ULL) + ((_d >> 2) & 0x3333333333333333ULL);
        s->drift_acc += (uint32_t)(((_d + (_d >> 4)) & 0x0F0F0F0F0F0F0F0FULL) * 0x0101010101010101ULL >> 56);
    }

    uint64_t r_next = icosa_route_update(s->route_addr, isect);

    int at_end = (isect == 0)
              || (s->drift_acc > 72u)
              || (s->hop_count >= 144u);

    uint8_t ev = ICOSA_EV_NONE;
    if (at_end) {
        *out_offset = (uint8_t)(s->drift_acc & 0xFFu);
        *out_route  = r_next;
        ev = ICOSA_EV_BOUNDARY;
        s->route_addr = 0;
        s->drift_acc  = 0;
        s->hop_count  = 0;
    } else {
        s->route_addr = r_next;
        s->hop_count++;
        *out_route  = r_next;
        *out_offset = 0;
    }
    return ev;
}

/* Bridge context */
typedef struct {
    uint64_t   seed_gen2;
    uint64_t   seed_gen3;
    uint64_t   baseline;
    uint32_t   c144_tag;

    IcosaFlowState flow;

    uint32_t   batch_size;
    int        gpu_ready;

    uint64_t   ops_total;
    uint64_t   boundaries_total;

    void      *gpu_ctx;  /* opaque GPU context pointer */
} IcosaTwinCtx;

static inline void icosa_twin_init(IcosaTwinCtx *ctx,
                                    uint64_t gen2, uint64_t gen3,
                                    uint64_t baseline, int enable_gpu)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->seed_gen2  = gen2;
    ctx->seed_gen3  = gen3;
    ctx->baseline   = baseline;
    ctx->c144_tag   = 1;
    ctx->batch_size = 0;
    ctx->gpu_ready  = 0;
    icosa_flow_init(&ctx->flow);
    ctx->ops_total       = 0;
    ctx->boundaries_total = 0;
    ctx->gpu_ctx         = NULL;

    if (enable_gpu) {
        extern void *icosa_gpu_ctx_create(uint64_t gen2, uint64_t gen3);
        extern int   icosa_gpu_ctx_valid(void *gpu);
        extern void  icosa_gpu_ctx_destroy(void *gpu);

        ctx->gpu_ctx = icosa_gpu_ctx_create(gen2, gen3);
        if (ctx->gpu_ctx && icosa_gpu_ctx_valid(ctx->gpu_ctx))
            ctx->gpu_ready = 1;
        else
            printf("[icosa] GPU init failed — fallback CPU\n");
    }
}

static inline void icosa_twin_free(IcosaTwinCtx *ctx) {
    if (ctx->gpu_ctx) {
        extern void icosa_gpu_ctx_destroy(void *gpu);
        icosa_gpu_ctx_destroy(ctx->gpu_ctx);
    }
    memset(ctx, 0, sizeof(*ctx));
}

static inline void icosa_twin_set_c144(IcosaTwinCtx *ctx, uint32_t tag) {
    ctx->c144_tag = tag;
}

/* Write one op. Returns ICOSA_EV_BOUNDARY if flow flushed. */
static inline uint8_t icosa_twin_write(
    IcosaTwinCtx *ctx,
    uint64_t       addr,
    uint64_t       value)
{
    uint64_t raw = addr ^ value ^ ctx->seed_gen3 ^ (uint64_t)ctx->c144_tag;
    uint64_t out_route = 0;
    uint8_t  out_offset = 0;

    uint8_t ev = icosa_lane_process(&ctx->flow, raw,
                                     ctx->baseline,
                                     &out_route, &out_offset);
    ctx->ops_total++;
    if (ev & ICOSA_EV_BOUNDARY) ctx->boundaries_total++;
    return ev;
}

/* Batch write — queues on GPU if available, else CPU. */
static inline void icosa_twin_batch(IcosaTwinCtx   *ctx,
                                     const uint64_t *addrs,
                                     const uint64_t *values,
                                     uint32_t        n)
{
    if (n == 0) return;

    if (ctx->gpu_ready) {
        extern int icosa_gpu_dispatch(
            void *gpu_ctx,
            const uint64_t *addrs,
            const uint64_t *values,
            uint32_t        n,
            uint64_t        gen3,
            uint32_t        c144_tag,
            uint64_t        baseline,
            uint64_t       *out_routes,
            uint8_t        *out_events);

        uint64_t *routes = (uint64_t *)malloc(n * sizeof(uint64_t));
        uint8_t  *events = (uint8_t  *)malloc(n);
        if (!routes || !events) { free(routes); free(events); return; }

        if (icosa_gpu_dispatch(ctx->gpu_ctx, addrs, values, n,
                                ctx->seed_gen3, ctx->c144_tag,
                                ctx->baseline, routes, events) == 0)
        {
            uint32_t last_boundary = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (events[i] & ICOSA_EV_BOUNDARY)
                    last_boundary = i;
            }
            if (events[n-1] & ICOSA_EV_BOUNDARY) {
                icosa_flow_init(&ctx->flow);
            } else if (last_boundary < n) {
                /* Replay tail from last boundary on CPU for flow state */
                icosa_flow_init(&ctx->flow);
                for (uint32_t i = last_boundary + 1; i < n; i++) {
                    uint64_t raw = addrs[i] ^ values[i]
                                 ^ ctx->seed_gen3
                                 ^ (uint64_t)ctx->c144_tag;
                    uint64_t r; uint8_t o;
                    icosa_lane_process(&ctx->flow, raw, ctx->baseline, &r, &o);
                }
            }
            ctx->ops_total += n;
            ctx->boundaries_total += 0; /* TODO: count from GPU events */
        }
        free(routes);
        free(events);
    } else {
        for (uint32_t i = 0; i < n; i++)
            icosa_twin_write(ctx, addrs[i], values[i]);
    }
}

/* Flush current flow — returns route_addr if boundary, else 0. */
static inline uint64_t icosa_twin_flush(IcosaTwinCtx *ctx) {
    if (ctx->flow.route_addr == 0) return 0;
    uint64_t r = ctx->flow.route_addr;
    icosa_flow_init(&ctx->flow);
    ctx->boundaries_total++;
    return r;
}

typedef struct {
    uint64_t ops_total;
    uint64_t boundaries_total;
    int      gpu_ready;
    uint64_t route_addr;
    uint32_t drift_acc;
    uint16_t hop_count;
} IcosaTwinStats;

static inline IcosaTwinStats icosa_twin_stats(const IcosaTwinCtx *ctx) {
    IcosaTwinStats s;
    s.ops_total        = ctx->ops_total;
    s.boundaries_total = ctx->boundaries_total;
    s.gpu_ready        = ctx->gpu_ready;
    s.route_addr       = ctx->flow.route_addr;
    s.drift_acc        = ctx->flow.drift_acc;
    s.hop_count        = ctx->flow.hop_count;
    return s;
}

#endif /* ICOSA_TWIN_BRIDGE_H */
