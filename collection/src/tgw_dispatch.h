/*
 * tgw_dispatch.h — TGW Full Dispatch Layer
 * =========================================
 * Wires tgw_write() result through the complete pipeline:
 *
 *   TGWResult (blueprint_ready)
 *       │
 *       ▼
 *   blueprint_to_geopkt × 18 → geomatrix_batch_verdict
 *       │
 *       ├─ verdict=true  (ROUTE) → blueprint_to_dodeca → fts_write
 *       └─ verdict=false (GROUND via lch_gate sign check) → pl_write
 *
 * Usage:
 *   TGWDispatch d;
 *   tgw_dispatch_init(&d, root_seed);
 *
 *   TGWResult r = tgw_write(&ctx, addr, value, 0);
 *   tgw_dispatch(&d, &r, addr, value, bundle);
 *
 *   TGWDispatchStats s = tgw_dispatch_stats(&d);
 *
 * No malloc. No heap.
 */

#ifndef TGW_DISPATCH_H
#define TGW_DISPATCH_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "geo_tring_goldberg_wire.h"
#include "fgls_twin_store.h"
#include "geo_payload_store.h"
#include "lc_twin_gate.h"

/* ── blueprint → GeoPacket (same logic as test_geomatrix_verdict) ── */
static inline GeoPacket _tgwd_bp_to_pkt(const GBBlueprint *bp,
                                          const uint64_t    *bundle,
                                          uint8_t            path_id)
{
    GeoPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.phase = (uint8_t)((bp->circuit_fired + path_id) & 0x3u);
    pkt.sig   = geo_compute_sig64(bundle, pkt.phase);

    uint16_t h_end   = (uint16_t)(bp->tring_end   % GEO_SLOTS);
    uint16_t h_start = (uint16_t)(bp->tring_start % GEO_SLOTS);

    pkt.hpos = h_end;
    bool hb = (h_end   >= GEO_BLOCK_BOUNDARY);
    bool ib = (h_start >= GEO_BLOCK_BOUNDARY);
    if (hb != ib)
        pkt.idx = hb ? (uint16_t)(h_start + GEO_BLOCK_BOUNDARY)
                     : (uint16_t)(h_start % GEO_BLOCK_BOUNDARY);
    else
        pkt.idx = h_start;

    pkt.bit = (uint8_t)(bp->stamp_hash & 0x1u);
    return pkt;
}

/* ── blueprint → DodecaEntry (ROUTE path) ─────────────────────── */
static inline DodecaEntry _tgwd_bp_to_dodeca(const GBBlueprint *bp)
{
    DodecaEntry e;
    memset(&e, 0, sizeof(e));
    e.merkle_root = (uint64_t)bp->stamp_hash
                  | ((uint64_t)bp->stamp_hash << 32);
    e.sha256_hi   = (uint64_t)bp->spatial_xor;
    e.sha256_lo   = bp->window_id;
    e.offset      = bp->circuit_fired;
    e.hop_count   = bp->tring_span;
    e.segment     = bp->top_gaps[0];
    return e;
}

/* ── Dispatch context ─────────────────────────────────────────── */
typedef struct {
    FtsTwinStore    fts;        /* ROUTE → structured geo store         */
    PayloadStore    ps;         /* GROUND → direct payload store        */
    LCTwinGateCtx   lc;         /* polarity gate (ROUTE vs GROUND)      */

    /* counters */
    uint32_t        total_dispatched;
    uint32_t        route_count;
    uint32_t        ground_count;
    uint32_t        verdict_fail;   /* blueprint present but verdict=false */
    uint32_t        no_blueprint;   /* tgw_write fired but no blueprint    */
} TGWDispatch;

typedef struct {
    uint32_t total_dispatched;
    uint32_t route_count;
    uint32_t ground_count;
    uint32_t verdict_fail;
    uint32_t no_blueprint;
    FtsTwinStats fts;
} TGWDispatchStats;

/* ── Init ─────────────────────────────────────────────────────── */
static inline void tgw_dispatch_init(TGWDispatch *d, uint64_t root_seed)
{
    memset(d, 0, sizeof(*d));
    fts_init(&d->fts, root_seed);
    pl_init(&d->ps);
    lc_twin_gate_init(&d->lc);
}

/* ── Core dispatch ────────────────────────────────────────────── */
/*
 * Call after every tgw_write().
 * addr/value: same pair passed to tgw_write (used for GROUND pl_write key).
 * bundle: the GEO_BUNDLE_WORDS seed used in TGWCtx.
 */
static inline void tgw_dispatch(TGWDispatch    *d,
                                  const TGWResult *r,
                                  uint64_t         addr,
                                  uint64_t         value,
                                  const uint64_t  *bundle)
{
    d->total_dispatched++;

    /* ── GROUND gate: check polarity regardless of blueprint ─── */
    {
        LCHdr hA = lc_hdr_encode_addr(addr);
        LCHdr hB = lc_hdr_encode_value(value);
        LCGate gate = lch_gate(hA, hB, &d->lc.palette_a, &d->lc.palette_b);
        if (gate == LC_GATE_GROUND) {
            pl_write(&d->ps, addr, value);
            d->lc.gate_counts[LC_GATE_GROUND]++;
            d->ground_count++;
            return;  /* bypass geo — done */
        }
    }

    /* ── Blueprint path: only when blueprint_ready ──────────── */
    if (!r->gpr.blueprint_ready) {
        d->no_blueprint++;
        return;
    }

    const GBBlueprint *bp = &r->gpr.bp;

    /* Build 18-packet batch */
    GeoPacket batch[GEOMATRIX_PATHS];
    for (int p = 0; p < GEOMATRIX_PATHS; p++)
        batch[p] = _tgwd_bp_to_pkt(bp, bundle, (uint8_t)p);

    /* Run verdict */
    GeomatrixStatsV3 stats = {0};
    bool verdict = geomatrix_batch_verdict(batch, bundle, &stats);

    if (verdict) {
        /* ROUTE: blueprint → DodecaEntry → fts_write */
        DodecaEntry e       = _tgwd_bp_to_dodeca(bp);
        uint64_t route_addr = (uint64_t)bp->stamp_hash;
        uint64_t route_val  = (uint64_t)bp->spatial_xor;
        fts_write(&d->fts, route_addr, route_val, &e);
        d->route_count++;
    } else {
        /* verdict=false → GROUND fallback */
        pl_write(&d->ps, addr, value);
        d->verdict_fail++;
        d->ground_count++;
    }
}

/* ── Convenience: tgw_write + dispatch in one call ───────────── */
static inline TGWResult tgw_write_dispatch(TGWCtx      *ctx,
                                             TGWDispatch *d,
                                             uint64_t     addr,
                                             uint64_t     value,
                                             uint8_t      slot_hot)
{
    TGWResult r = tgw_write(ctx, addr, value, slot_hot);
    tgw_dispatch(d, &r, addr, value, ctx->_bundle);
    return r;
}

/* ── Stats ────────────────────────────────────────────────────── */
static inline TGWDispatchStats tgw_dispatch_stats(const TGWDispatch *d)
{
    TGWDispatchStats s;
    s.total_dispatched = d->total_dispatched;
    s.route_count      = d->route_count;
    s.ground_count     = d->ground_count;
    s.verdict_fail     = d->verdict_fail;
    s.no_blueprint     = d->no_blueprint;
    s.fts              = fts_stats(&d->fts);
    return s;
}

#endif /* TGW_DISPATCH_H */
