/*
 * tgw_stream_dispatch.h — Stream-Aware Dispatch (P1 SEAM 1 fix)
 * ==============================================================
 * Wires tgw_stream_file() through ROUTE/GROUND dispatch.
 *
 * Problem solved:
 *   tgw_stream_file() calls tgw_write() directly — stream data
 *   bypasses ROUTE/GROUND split entirely (SEAM 1 in POGLS_WIRE_DRAFT)
 *
 *   Root cause: stream addr = pkts[i].enc (uint32_t, bit63=0 always)
 *   → lch_gate sign check never fires → no GROUND from stream ever
 *
 * Fix:
 *   polarity derived from TRing walk position via enc (stateless, O(1))
 *   pos   = tring_pos(enc)          → 0..719
 *   pentagon = pos / 60             → 0..11  (which of 12 pentagons)
 *   spoke    = pentagon % 6         → 0..5
 *   polarity = (pos % 60) >= 30     → live(0) / mirror(1)
 *
 *   polarity=1 → GROUND (pl_write)
 *   polarity=0 → normal blueprint path (same as tgw_dispatch)
 *
 * Usage:
 *   TGWCtx ctx;  TGWDispatch d;
 *   tgw_init(&ctx, seed, bundle);
 *   tgw_dispatch_init(&d, root_seed);
 *
 *   tgw_stream_dispatch(&ctx, &d, data, size);
 *
 *   TGWDispatchStats s = tgw_dispatch_stats(&d);
 *
 * No malloc. No heap. No changes to TGWCtx or TGWResult or GBBlueprint.
 *
 * Sacred constants (frozen):
 *   720  = TEMPORAL_WALK_LEN  (5 tetra × 12 pentagons × 12)
 *   60   = positions per pentagon (5 tetra × 12 orientations)
 *   30   = half of 60 → polarity boundary (live / mirror)
 *   6    = spokes (pentagon count mod 6)
 */

#ifndef TGW_STREAM_DISPATCH_H
#define TGW_STREAM_DISPATCH_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "geo_tring_goldberg_wire.h"
#include "tgw_dispatch.h"

/* ── TRing geometry constants ────────────────────────────────── */
#define TRING_PENT_SPAN   60u   /* positions per pentagon: 5 tetra × 12 */
#define TRING_MIRROR_HALF 30u   /* polarity boundary within pentagon     */
#define TRING_SPOKES       6u   /* pentagon % 6 = spoke                  */

/* ── derive routing info from enc (stateless, O(1) LUT) ─────── */
typedef struct {
    uint16_t pos;       /* walk position 0..719            */
    uint8_t  pentagon;  /* 0..11  (which pentagon face)    */
    uint8_t  spoke;     /* 0..5   (lane in pipeline)       */
    uint8_t  polarity;  /* 0=live(ROUTE candidate)
                           1=mirror(GROUND)                */
} TRingRoute;

static inline TRingRoute tring_route_from_enc(uint32_t enc)
{
    TRingRoute rt;
    rt.pos      = tring_pos(enc);                         /* O(1) LUT   */
    if (rt.pos == 0xFFFFu) {
        /* unknown enc — treat as GROUND (safe fallback) */
        rt.pentagon = 0;
        rt.spoke    = 0;
        rt.polarity = 1;
        return rt;
    }
    rt.pentagon = (uint8_t)(rt.pos / TRING_PENT_SPAN);   /* 0..11      */
    rt.spoke    = (uint8_t)(rt.pentagon % TRING_SPOKES);  /* 0..5       */
    rt.polarity = (uint8_t)((rt.pos % TRING_PENT_SPAN) >= TRING_MIRROR_HALF);
    return rt;
}

/* ── stream dispatch stats (separate from TGWDispatchStats) ─── */
typedef struct {
    uint32_t pkts_rx;       /* total packets received from stream   */
    uint32_t pkts_ground;   /* routed to GROUND via mirror polarity */
    uint32_t pkts_route;    /* passed to blueprint path             */
    uint32_t pkts_gap;      /* dropped (unknown enc / CRC fail)     */
    uint32_t pkts_no_bp;    /* blueprint path but no blueprint yet  */
} TGWStreamStats;

/* ── stream dispatch context ─────────────────────────────────── */
typedef struct {
    TGWStreamStats ss;
} TGWStreamDispatch;

static inline void tgw_stream_dispatch_init(TGWStreamDispatch *sd)
{
    memset(sd, 0, sizeof(*sd));
}

/* ── core: slice file → TRing → ROUTE/GROUND dispatch ───────── */
/*
 * Replaces tgw_stream_file() for dispatch-aware ingress.
 * Each packet's polarity is derived from its TRing walk position.
 * GROUND packets go directly to pl_write (bypass blueprint).
 * ROUTE candidates go through geomatrix_batch_verdict → fts_write.
 *
 * addr  = enc  (TRing walk tuple, embeds spoke + polarity)
 * value = crc16 | (size << 16)  — same encoding as tgw_stream_file
 */
static inline uint16_t tgw_stream_dispatch(TGWCtx            *ctx,
                                            TGWDispatch       *d,
                                            TGWStreamDispatch *sd,
                                            const uint8_t     *data,
                                            uint32_t           size)
{
    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, data, size);

    for (uint16_t i = 0; i < n; i++) {

        /* ── TRing receive (ordering + CRC) ────────────────── */
        int snap = tstream_recv_pkt(&ctx->tr, ctx->store, &pkts[i]);
        if (snap < 0) {
            /* -1=unknown enc, -2=CRC fail → drop */
            sd->ss.pkts_gap++;
            ctx->stream_gaps++;
            continue;
        }

        ctx->stream_pkts_rx++;

        /* ── derive polarity from enc (stateless) ─────────── */
        TRingRoute rt = tring_route_from_enc(pkts[i].enc);

        uint64_t addr  = (uint64_t)pkts[i].enc;
        uint64_t value = (uint64_t)pkts[i].crc16
                       | ((uint64_t)pkts[i].size << 16);

        /* ── GROUND: mirror side → pl_write directly ──────── */
        if (rt.polarity == 1) {
            pl_write(&d->ps, addr, value);
            d->ground_count++;
            d->total_dispatched++;
            sd->ss.pkts_ground++;
            sd->ss.pkts_rx++;
            continue;
        }

        /* ── ROUTE candidate: feed into goldberg pipeline ─── */
        TGWResult r = tgw_write(ctx, addr, value, 0);

        /* dispatch through verdict → fts_write or pl_write */
        tgw_dispatch(d, &r, addr, value, ctx->_bundle);

        if (!r.gpr.blueprint_ready)
            sd->ss.pkts_no_bp++;
        else
            sd->ss.pkts_route++;

        sd->ss.pkts_rx++;
    }

    return n;
}

/* ── convenience: stats merge ────────────────────────────────── */
static inline TGWStreamStats tgw_stream_dispatch_stats(
        const TGWStreamDispatch *sd)
{
    return sd->ss;
}

/* ── verify: stream coverage across spokes ──────────────────── */
/*
 * Debug helper: count how many packets landed in each spoke (0..5).
 * Even distribution = healthy walk. Skew = enc distribution issue.
 * Call after tgw_stream_dispatch() with the pkts slice to inspect.
 */
static inline void tgw_stream_spoke_coverage(
        const TStreamPkt *pkts,
        uint16_t          n,
        uint32_t          counts[TRING_SPOKES])
{
    for (uint8_t s = 0; s < TRING_SPOKES; s++) counts[s] = 0;
    for (uint16_t i = 0; i < n; i++) {
        TRingRoute rt = tring_route_from_enc(pkts[i].enc);
        if (rt.pos != 0xFFFFu)
            counts[rt.spoke]++;
    }
}

#endif /* TGW_STREAM_DISPATCH_H */
