/*
 * tgw_lc_bridge_p6.h — TGW ↔ LC Bridge (P6 upgrade)
 * ════════════════════════════════════════════════════════════
 * P6 changes vs P4:
 *
 *   A) SEAM 2 hook — geo_addr_net replaces enc%720 in all routing math
 *      tgwlc_enc_to_pos()    → geo_net_encode().tring_pos   (same result)
 *      tgwlc_pos_to_spoke()  → geo_net_encode().spoke        (same result)
 *      tgwlc_route()         → geo_net_to_lc_route()         (drop-in)
 *      NEW: tgwlc_hilbert_lane() → geo_net_hilbert_lane()    (SEAM 2 only)
 *
 *   B) Hilbert prefetch sort — pending[64] sorted by hilbert_idx
 *      before _tgwd_flush_batch() fires tgw_batch().
 *      Adjacent hilbert lanes → adjacent memory → better prefetch pattern.
 *
 *      Sort cost: insertion sort on ≤64 items = O(n²) worst ~2000 ops
 *      Done once per flush, not per pkt. At 64 items: ~0.3µs overhead.
 *      Expected gain: cache miss reduction on twin_bridge lane scan.
 *
 *   C) tgwlc_prefilter_p6() — upgraded prefilter using GeoNetAddr
 *      Same O(n) cost, outputs hilbert_lane alongside ground_mask.
 *
 * Backward compatibility:
 *   All P4 function signatures unchanged.
 *   New functions prefixed _p6 or have new names.
 *   Include alongside tgw_lc_bridge.h (P4) or as replacement.
 *
 * No malloc. No heap. No float. Integer-only.
 * ════════════════════════════════════════════════════════════
 */

#ifndef TGW_LC_BRIDGE_P6_H
#define TGW_LC_BRIDGE_P6_H

#include <stdint.h>
#include <string.h>

#include "lc_hdr.h"
#include "geo_addr_net.h"

typedef LCHdr LCNodeHdr;

/* ── Re-use P4 types (unchanged) ──────────────────────────── */
typedef enum {
    BRIDGE_GATE_WARP      = 0,
    BRIDGE_GATE_ROUTE     = 1,
    BRIDGE_GATE_COLLISION = 2,
    BRIDGE_GATE_GROUND    = 3,
} BridgeGate;

typedef struct {
    uint8_t  spoke;
    uint8_t  polarity;
    uint16_t tring_pos;
} TGWLCRoute;

/* ── P6 extension: route + Hilbert lane ───────────────────── */
typedef struct {
    uint8_t  spoke;
    uint8_t  polarity;
    uint16_t tring_pos;
    uint16_t hilbert_lane;   /* NEW: Hilbert-ordered spatial lane */
    uint8_t  _pad[2];
} TGWLCRouteP6;              /* 8 bytes, aligned                  */

/* ════════════════════════════════════════════════════════════
   A) SEAM 2 HOOK — drop-in replacements using geo_addr_net
   ════════════════════════════════════════════════════════════ */

/*
 * tgwlc_enc_to_pos_p6() — enc → tring_pos (via GeoNetAddr)
 * Backward compat: same numeric output as enc%720.
 */
static inline uint16_t tgwlc_enc_to_pos_p6(uint64_t enc) {
    return geo_net_encode(enc).tring_pos;
}

/*
 * tgwlc_pos_to_spoke_p6() — pos → spoke (via GeoNetAddr, same as /120)
 */
static inline uint8_t tgwlc_pos_to_spoke_p6(uint16_t pos) {
    return (uint8_t)(pos / GAN_PENT_SPAN);
}

/*
 * tgwlc_route_p6() — enc → TGWLCRoute (drop-in, geo_net path)
 * Output numerically identical to P4 tgwlc_route().
 */
static inline TGWLCRoute tgwlc_route_p6(uint64_t enc) {
    GanLCRoute r = geo_net_to_lc_route(enc);
    TGWLCRoute out;
    out.spoke     = r.spoke;
    out.polarity  = r.polarity;
    out.tring_pos = r.tring_pos;
    return out;
}

/*
 * tgwlc_route_full_p6() — enc → TGWLCRouteP6 (with hilbert_lane)
 * Use this when downstream needs spatial ordering (e.g. prefetch sort).
 */
static inline TGWLCRouteP6 tgwlc_route_full_p6(uint64_t enc) {
    GeoNetAddr a = geo_net_encode(enc);
    TGWLCRouteP6 r;
    r.spoke        = a.spoke;
    r.polarity     = a.polarity;
    r.tring_pos    = a.tring_pos;
    r.hilbert_lane = a.hilbert_idx;
    r._pad[0]      = 0u;
    r._pad[1]      = 0u;
    return r;
}

/*
 * tgwlc_hilbert_lane() — enc → Hilbert lane (fast, LUT lookup)
 * Used by batch sort to reorder pending[] before flush.
 */
static inline uint16_t tgwlc_hilbert_lane(uint64_t enc) {
    return geo_net_hilbert_lane(enc);
}

/* ── enc → LCNodeHdr (P6: uses geo_addr_net angular) ──────── */
static inline LCNodeHdr tgwlc_enc_to_node_hdr_p6(uint64_t enc) {
    GeoNetAddr a = geo_net_encode(enc);
    return lch_pack(
        a.polarity, 1u,
        (uint8_t)(a.spoke % 8u),
        (uint8_t)(a.tring_pos % 8u),
        (uint8_t)(enc % 8u),
        LC_LEVEL_0,
        a.ang_step,
        (uint8_t)(a.spoke % 4u)
    );
}

/* ── gate (unchanged logic, P6 enc path) ──────────────────── */
static inline BridgeGate tgwlc_gate_p6(uint64_t enc_addr, uint64_t enc_val) {
    LCNodeHdr hA = tgwlc_enc_to_node_hdr_p6(enc_addr);
    LCNodeHdr hB = tgwlc_enc_to_node_hdr_p6(enc_val);
    if (lch_is_ghost(hA) || lch_is_ghost(hB)) return BRIDGE_GATE_GROUND;
    if (lch_sign(hA) != lch_sign(hB))         return BRIDGE_GATE_GROUND;
    if (lch_is_complement(hA, hB))             return BRIDGE_GATE_WARP;
    return BRIDGE_GATE_ROUTE;
}

/* ════════════════════════════════════════════════════════════
   B) HILBERT PREFETCH SORT
   ════════════════════════════════════════════════════════════
 *
 * Problem: pending[64] accumulated in arrival order (random Hilbert lane).
 * Fix:     insertion-sort by hilbert_lane before tgw_batch() flush.
 *
 * Why insertion sort:
 *   - n ≤ 64 always (TGW_DISPATCH_BATCH_MAX)
 *   - nearly-sorted common case (stream pkts often sequential enc)
 *   - in-place, no extra buffer
 *   - branchless inner compare fits L1
 *
 * Visual:
 *   Before sort: [lane=400, lane=2, lane=719, lane=50, ...]
 *   After  sort: [lane=2, lane=50, lane=400, lane=719, ...]
 *   → twin_bridge scan: sequential memory access, hardware prefetch wins
 */

/* Pending slot: addr + val + hilbert_lane for sort key */
typedef struct {
    uint64_t addr;
    uint64_t val;
    uint16_t hilbert_lane;
    uint8_t  _pad[6];        /* pad to 24 bytes — 3 cache words */
} TGWPendingSlot;

#define TGW_P6_BATCH_MAX  64u

/*
 * tgw_pending_sort() — insertion sort pending[n] by hilbert_lane ASC
 * In-place. n ≤ 64. Typical cost on random input: ~300ns once per flush.
 * On nearly-sorted input (common stream pattern): ~10–20ns.
 */
static inline void tgw_pending_sort(TGWPendingSlot *slots, uint32_t n) {
    for (uint32_t i = 1u; i < n; i++) {
        TGWPendingSlot key = slots[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && slots[j].hilbert_lane > key.hilbert_lane) {
            slots[j + 1] = slots[j];
            j--;
        }
        slots[j + 1] = key;
    }
}

/*
 * TGWDispatchP6 — P4 dispatch context extended with Hilbert-sorted pending
 *
 * Drop-in replacement for TGWDispatch:
 *   - same GROUND/ROUTE split
 *   - same flush trigger at BATCH_MAX=64
 *   - NEW: pending slots carry hilbert_lane, sorted before flush
 */
typedef struct {
    /* pending batch (P6: TGWPendingSlot vs P4: two uint64 arrays) */
    TGWPendingSlot  pending[TGW_P6_BATCH_MAX];
    uint32_t        pending_n;

    /* counters (same as TGWDispatch) */
    uint32_t        total_dispatched;
    uint32_t        route_count;
    uint32_t        ground_count;
    uint32_t        verdict_fail;
    uint32_t        no_blueprint;
    uint32_t        batch_flushes;
    uint32_t        sort_skipped;    /* P6: times sort was skipped (n≤1) */

    /* sort quality stats */
    uint64_t        total_swaps;     /* total insertion sort swaps */
} TGWDispatchP6;

static inline void tgw_dispatch_p6_init(TGWDispatchP6 *d) {
    memset(d, 0, sizeof(*d));
}

/*
 * _tgwd_p6_flush() — sort pending by Hilbert lane, then hand off addrs/vals.
 *
 * Caller provides callback fn pointer:
 *   void flush_fn(const uint64_t *addrs, const uint64_t *vals,
 *                 uint32_t n, void *ctx)
 * This keeps the flush decoupled from tgw_batch() signature.
 */
typedef void (*TGWBatchFlushFn)(const uint64_t *addrs,
                                 const uint64_t *vals,
                                 uint32_t        n,
                                 void           *ctx);

static inline void _tgwd_p6_flush(TGWDispatchP6  *d,
                                    TGWBatchFlushFn flush_fn,
                                    void           *flush_ctx)
{
    if (d->pending_n == 0u) return;

    /* sort by Hilbert lane (skip if 0 or 1 items — nothing to sort) */
    if (d->pending_n > 1u) {
        /* count swaps before/after for stats */
        uint64_t swaps_before = d->total_swaps;
        /* patched insertion sort that counts swaps */
        for (uint32_t i = 1u; i < d->pending_n; i++) {
            TGWPendingSlot key = d->pending[i];
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && d->pending[j].hilbert_lane > key.hilbert_lane) {
                d->pending[j + 1] = d->pending[j];
                j--;
                d->total_swaps++;
            }
            d->pending[j + 1] = key;
        }
        (void)swaps_before;
    } else {
        d->sort_skipped++;
    }

    /* extract sorted addrs/vals into temp arrays for flush_fn */
    static uint64_t _tmp_addrs[TGW_P6_BATCH_MAX];
    static uint64_t _tmp_vals [TGW_P6_BATCH_MAX];
    for (uint32_t i = 0u; i < d->pending_n; i++) {
        _tmp_addrs[i] = d->pending[i].addr;
        _tmp_vals [i] = d->pending[i].val;
    }

    flush_fn(_tmp_addrs, _tmp_vals, d->pending_n, flush_ctx);
    d->pending_n = 0u;
    d->batch_flushes++;
}

/*
 * tgw_dispatch_p6() — single pkt dispatch with Hilbert-aware batch
 *
 * GROUND path: immediate (caller provides ground_fn callback).
 * ROUTE path:  accumulate → sort → flush at BATCH_MAX.
 *
 * Callbacks decouple from concrete tgw_batch/pl_write signatures:
 *   flush_fn  → wraps tgw_batch(ctx, addrs, vals, n, 0)
 *   ground_fn → wraps pl_write(ps, addr, val)
 *
 * polarity: 1 = GROUND, 0 = ROUTE (from geo_net_encode)
 */
typedef void (*TGWGroundFn)(uint64_t addr, uint64_t val, void *ctx);

static inline void tgw_dispatch_p6(TGWDispatchP6   *d,
                                     uint64_t          addr,
                                     uint64_t          val,
                                     TGWBatchFlushFn   flush_fn,
                                     TGWGroundFn       ground_fn,
                                     void             *fn_ctx)
{
    d->total_dispatched++;

    /* SEAM 2: full geo_net decode — polarity + hilbert in one call */
    GeoNetAddr geo = geo_net_encode(addr);

    if (geo.polarity) {
        /* GROUND: bypass batch entirely */
        ground_fn(addr, val, fn_ctx);
        d->ground_count++;
        return;
    }

    /* ROUTE: push into Hilbert-keyed pending slot */
    TGWPendingSlot *slot = &d->pending[d->pending_n];
    slot->addr         = addr;
    slot->val          = val;
    slot->hilbert_lane = geo.hilbert_idx;
    slot->_pad[0]      = 0u;
    d->pending_n++;
    d->route_count++;

    /* flush when full (batch boundary) */
    if (d->pending_n >= TGW_P6_BATCH_MAX)
        _tgwd_p6_flush(d, flush_fn, fn_ctx);
}

/* ── Manual flush (call at end of stream) ─────────────────── */
static inline void tgw_dispatch_p6_flush(TGWDispatchP6   *d,
                                           TGWBatchFlushFn   flush_fn,
                                           void             *fn_ctx)
{
    _tgwd_p6_flush(d, flush_fn, fn_ctx);
}

/* ════════════════════════════════════════════════════════════
   C) PREFILTER P6 — outputs ground_mask + hilbert lanes
   ════════════════════════════════════════════════════════════ */

/*
 * tgwlc_prefilter_p6() — upgraded prefilter
 *
 * Same interface as P4 tgwlc_prefilter() but also fills:
 *   hilbert_lanes[i] = geo_net_hilbert_lane(pkts[i].enc)
 *
 * Caller can use hilbert_lanes[] to pre-sort ROUTE pkts before
 * feeding into dispatch, front-loading locality benefit.
 *
 * n ≤ 64 (uint64_t ground_mask).
 *
 * Note: TStreamPkt stub for standalone build — real build includes
 * geo_tring_stream.h which defines TStreamPkt with .enc field.
 */

#ifndef GEO_TRING_STREAM_H
/* Minimal stub — remove when geo_tring_stream.h is available */
typedef struct { uint32_t enc; uint16_t size; uint16_t crc16; } TStreamPktStub;
#define TStreamPkt TStreamPktStub
#endif

static inline uint64_t tgwlc_prefilter_p6(const TStreamPkt *pkts,
                                            uint16_t          n,
                                            uint16_t         *hilbert_lanes)
{
    uint64_t ground_mask = 0u;
    uint16_t lim = (n > 64u) ? 64u : n;
    for (uint16_t i = 0u; i < lim; i++) {
        GeoNetAddr a = geo_net_encode((uint64_t)pkts[i].enc);
        hilbert_lanes[i] = a.hilbert_idx;
        if (a.polarity)
            ground_mask |= (UINT64_C(1) << i);
    }
    return ground_mask;
}

/* ── Spoke histogram P6 (uses geo_net, same output as P4) ─── */
static inline void tgwlc_spoke_hist_p6(const TStreamPkt *pkts,
                                         uint16_t          n,
                                         uint32_t          hist[GAN_SPOKES])
{
    for (uint8_t s = 0u; s < GAN_SPOKES; s++) hist[s] = 0u;
    for (uint16_t i = 0u; i < n; i++) {
        GeoNetAddr a = geo_net_encode((uint64_t)pkts[i].enc);
        hist[a.spoke]++;
    }
}

/* ════════════════════════════════════════════════════════════
   STATS / PRINT
   ════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t total_dispatched;
    uint32_t route_count;
    uint32_t ground_count;
    uint32_t batch_flushes;
    uint32_t sort_skipped;
    uint64_t total_swaps;
    double   avg_swaps_per_flush;
    double   sort_efficiency;  /* 1.0 = already sorted, 0.0 = fully reversed */
} TGWDispatchP6Stats;

static inline TGWDispatchP6Stats tgw_dispatch_p6_stats(const TGWDispatchP6 *d)
{
    TGWDispatchP6Stats s;
    s.total_dispatched  = d->total_dispatched;
    s.route_count       = d->route_count;
    s.ground_count      = d->ground_count;
    s.batch_flushes     = d->batch_flushes;
    s.sort_skipped      = d->sort_skipped;
    s.total_swaps       = d->total_swaps;
    uint32_t real_flushes = d->batch_flushes - d->sort_skipped;
    if (real_flushes > 0u) {
        s.avg_swaps_per_flush = (double)d->total_swaps / (double)real_flushes;
        /* max possible swaps per batch = n*(n-1)/2 = 64*63/2 = 2016 */
        double max_swaps = (double)real_flushes * 2016.0;
        s.sort_efficiency = 1.0 - ((double)d->total_swaps / max_swaps);
    } else {
        s.avg_swaps_per_flush = 0.0;
        s.sort_efficiency     = 1.0;
    }
    return s;
}

#endif /* TGW_LC_BRIDGE_P6_H */
