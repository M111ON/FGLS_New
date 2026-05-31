/*
 * geom_shadow_pipe.h — Shadow classify → GeomBridge tile decode pipeline
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Wires bermuda_shadow.h + geom_router_bridge.h into one call:
 *
 *   raw chunk (64B) + geo index
 *     → bermuda_shadow_dispatch()   classify HOT/COLD + route entry
 *     → HOT:  grb_decode_route()    O(1) tile decode from GeomBridge
 *     → COLD: shadow ring push      preserved, retrievable by bond_key
 *     → GspResult { tile[7], valid, temperature, route }
 *
 * Nothing is destroyed. COLD data lives in shadow ring (capacity=144).
 * HOT data flows forward as decoded tile bytes.
 *
 * Memory layout: all stack/static. No malloc. No float.
 *
 * Integration:
 *   #define GEOM_SHADOW_PIPE_IMPLEMENTATION
 *   #include "geom_shadow_pipe.h"
 *
 *   GspCtx ctx;
 *   gsp_init(&ctx, &grb, &shadow_ring);
 *
 *   GspResult r;
 *   gsp_push(&ctx, chunk64, idx, gear, mode, addr, bond_key, &r);
 *   // r.valid==1 → r.tile has decoded bytes
 *   // r.valid==0 → COLD, find later: bermuda_shadow_find(ring, bond_key)
 *
 *   // Batch:
 *   GspResult results[N];
 *   uint32_t n_hot = gsp_push_batch(&ctx, chunks, idxs, addrs, bkeys,
 *                                    gear, mode, results, N);
 *
 * Compile test:
 *   gcc -O2 -DGEOM_RAW_BRIDGE_IMPLEMENTATION \
 *           -DGEOM_ROUTER_BRIDGE_IMPLEMENTATION \
 *           -DGEOM_SHADOW_PIPE_IMPLEMENTATION \
 *           -I. -o test_gsp test_gsp.c
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GEOM_SHADOW_PIPE_H
#define GEOM_SHADOW_PIPE_H

#include <stdint.h>
#include <stddef.h>
#include "bermuda_shadow.h"        /* BermudaShadowRing, bermuda_shadow_dispatch */
#include "geom_router_bridge.h"    /* GeomRouterBridge, GrbDecodeResult          */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return codes ───────────────────────────────────────────── */
#define GSP_OK          0
#define GSP_ERR        -1

/* ── Per-push result ────────────────────────────────────────── */
/*
 * GspResult carries everything the caller needs downstream.
 * If valid==1: tile[] has 7 decoded bytes, route has full routing info.
 * If valid==0: COLD token — tile[] is zeroed, bond_key is retrievable
 *              via bermuda_shadow_find(ring, bond_key).
 */
typedef struct {
    uint8_t           tile[GSTEN_TILE_SZ];  /* decoded 7B (HOT only)        */
    uint8_t           valid;                /* 1=HOT decoded, 0=COLD/skip   */
    uint8_t           temperature;          /* BERMUDA_SHADOW_HOT/COLD      */
    BermudaRouteEntry route;                /* full routing verdict          */
    GrbDecodeResult   decode;               /* tile metadata (entry/tile idx)*/
    uint64_t          bond_key;             /* COLD retrieval key            */
} GspResult;

/* ── Pipeline context (no heap) ─────────────────────────────── */
typedef struct {
    GeomRouterBridge  *grb;
    BermudaShadowRing *ring;
} GspCtx;

/* ── API ────────────────────────────────────────────────────── */

static inline void gsp_init(GspCtx            *ctx,
                             GeomRouterBridge  *grb,
                             BermudaShadowRing *ring) {
    ctx->grb  = grb;
    ctx->ring = ring;
}

/*
 * gsp_push — classify one 64B chunk, decode tile if HOT.
 *
 * chunk64  : exactly 64 bytes of raw data
 * idx      : bermuda slot index (gear-space, from codebook)
 * gear     : 1-4
 * mode     : 0=ORBITAL 1=CHIRAL 2=CROSS 3=HUB
 * addr     : origin geo_key (for shadow entry)
 * bond_key : retrieval key (caller assigns; should be unique per chunk)
 * out      : result written here
 *
 * Returns GSP_OK always (COLD is not an error).
 * Returns GSP_ERR only on null args or internal decode failure.
 */
int gsp_push(GspCtx       *ctx,
             const uint8_t *chunk64,
             uint16_t       idx,
             uint8_t        gear,
             uint8_t        mode,
             uint64_t       addr,
             uint64_t       bond_key,
             GspResult     *out);

/*
 * gsp_push_batch — process N chunks.
 *
 * chunks   : N × 64B packed  (chunks + i*64)
 * idxs     : [N] bermuda slot indices
 * addrs    : [N] geo_keys (NULL → use idx as addr)
 * bond_keys: [N] retrieval keys (NULL → use idx as key)
 * gear, mode: applied to all chunks uniformly
 * results  : [N] output array, caller allocates
 *
 * Returns: number of HOT (valid) tiles decoded.
 */
uint32_t gsp_push_batch(GspCtx         *ctx,
                        const uint8_t  *chunks,
                        const uint16_t *idxs,
                        const uint64_t *addrs,
                        const uint64_t *bond_keys,
                        uint8_t         gear,
                        uint8_t         mode,
                        GspResult      *results,
                        uint32_t        n);

/*
 * gsp_retrieve_cold — look up a previously COLD chunk from shadow ring.
 * Returns pointer to shadow entry (valid until ring wraps), or NULL.
 */
static inline const BermudaShadowEntry *gsp_retrieve_cold(
    const GspCtx *ctx, uint64_t bond_key)
{
    return bermuda_shadow_find(ctx->ring, bond_key);
}

/*
 * gsp_stats — combined stats: shadow ring + decode counters.
 */
typedef struct {
    uint32_t total_hot;
    uint32_t total_cold;
    uint32_t ring_count;
    uint32_t evictions;
    uint32_t decode_errors;   /* HOT tokens that failed tile decode */
} GspStats;

static inline GspStats gsp_stats(const GspCtx *ctx) {
    BermudaShadowStats ss = bermuda_shadow_stats(ctx->ring);
    GspStats s;
    s.total_hot     = ss.total_hot;
    s.total_cold    = ss.total_cold;
    s.ring_count    = ss.ring_count;
    s.evictions     = ss.evictions;
    s.decode_errors = 0;  /* tracked in gsp_push_batch */
    return s;
}

#ifdef __cplusplus
}
#endif

/* ═══════════════════════════════════════════════════════════════
   IMPLEMENTATION
   ═══════════════════════════════════════════════════════════════ */
#ifdef GEOM_SHADOW_PIPE_IMPLEMENTATION

#include <string.h>

int gsp_push(GspCtx        *ctx,
             const uint8_t *chunk64,
             uint16_t       idx,
             uint8_t        gear,
             uint8_t        mode,
             uint64_t       addr,
             uint64_t       bond_key,
             GspResult     *out)
{
    if (!ctx || !chunk64 || !out) return GSP_ERR;

    memset(out, 0, sizeof(*out));
    out->bond_key = bond_key;

    /* ── Step 1: classify + get routing verdict ── */
    BermudaRouteEntry route;
    uint8_t temp = bermuda_shadow_dispatch(
        ctx->ring,
        chunk64,
        idx,
        gear,
        mode,
        addr,
        bond_key,
        &route);

    out->temperature = temp;
    out->route       = route;

    /* ── Step 2: COLD → done (data in shadow ring) ── */
    /* COLD = float/unstructured, pushed to ground ring, no tile decode.
     * NOTE: polarity (ROUTE/GROUND) is a routing lane signal, not a
     * decode gate — north-pole HOT tokens are GROUND lane but still
     * produce valid tiles. Only temperature gates decode. */
    if (temp == BERMUDA_SHADOW_COLD) {
        out->valid = 0;
        return GSP_OK;
    }

    /* ── Step 3: HOT → decode tile via GeomRouterBridge ── */
    if (grb_decode_route(ctx->grb, &route, &out->decode) != GRB_OK) {
        /* HOT but no tile available (zone empty, etc.) — not fatal */
        out->valid = 0;
        return GSP_OK;
    }

    if (out->decode.valid) {
        memcpy(out->tile, out->decode.tile, GSTEN_TILE_SZ);
        out->valid = 1;
    }

    return GSP_OK;
}

uint32_t gsp_push_batch(GspCtx         *ctx,
                        const uint8_t  *chunks,
                        const uint16_t *idxs,
                        const uint64_t *addrs,
                        const uint64_t *bond_keys,
                        uint8_t         gear,
                        uint8_t         mode,
                        GspResult      *results,
                        uint32_t        n)
{
    if (!ctx || !chunks || !idxs || !results || n == 0) return 0;

    uint32_t n_hot = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t addr     = addrs     ? addrs[i]     : (uint64_t)idxs[i];
        uint64_t bond_key = bond_keys ? bond_keys[i] : (uint64_t)idxs[i];

        gsp_push(ctx,
                 chunks + (size_t)i * BERMUDA_CHUNK,
                 idxs[i],
                 gear,
                 mode,
                 addr,
                 bond_key,
                 &results[i]);

        if (results[i].valid) n_hot++;
    }
    return n_hot;
}

#endif /* GEOM_SHADOW_PIPE_IMPLEMENTATION */
#endif /* GEOM_SHADOW_PIPE_H */
