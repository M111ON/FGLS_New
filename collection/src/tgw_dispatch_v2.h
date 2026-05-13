/*
 * tgw_dispatch_v2.h — TGWDispatchV2: P4 + P6 merged dispatch layer
 * ══════════════════════════════════════════════════════════════════
 * Merges:
 *   P4 tgw_dispatch.h     → full context (fts, ps, lc) + geomatrix verdict
 *   P6 tgw_lc_bridge_p6.h → TGWPendingSlot + Hilbert sort + callbacks
 *   geo_addr_net.h         → SEAM 2 enc path (replaces enc%720 direct)
 *
 * Key design decisions:
 *   - Pending buffer uses TGWPendingSlot (addr+val+hilbert_lane) from P6
 *   - Flush uses callback (TGWBatchFlushFn) so tgw_batch() stays decoupled
 *   - GROUND uses callback (TGWGroundFn) — but default wrappers provided
 *   - geomatrix_batch_verdict path from P4 kept intact
 *   - fts/ps/lc context from P4 embedded in TGWDispatchV2
 *   - geo_addr_net_init() MUST be called before first tgw_dispatch_v2()
 *
 * Sacred constants (frozen):
 *   720  = TRING_CYCLE   144 = GBT_WINDOW
 *   18   = GEOMATRIX_PATHS   64 = BATCH_MAX
 *
 * No malloc. No heap. Integer-only in hot path.
 * ══════════════════════════════════════════════════════════════════
 */

#ifndef TGW_DISPATCH_V2_H
#define TGW_DISPATCH_V2_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "geo_addr_net.h"          /* SEAM 2: GeoNetAddr, geo_net_encode()  */
#include "tgw_cardioid_express.h"  /* Hook A/B: cardioid gate + express lane */
#include "tgw_lc_bridge_p6.h"      /* P6: TGWPendingSlot, sort, callbacks   */
#include "geo_tring_goldberg_wire.h"
#include "fgls_twin_store.h"
#include "geo_payload_store.h"
#include "lc_twin_gate.h"

/* ── batch size (same as P4 + P6, frozen) ────────────────── */
#define TGW_V2_BATCH_MAX  64u

/* ── P4 blueprint helpers (unchanged) ────────────────────── */
static inline GeoPacket _v2_bp_to_pkt(const GBBlueprint *bp,
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

static inline DodecaEntry _v2_bp_to_dodeca(const GBBlueprint *bp)
{
    DodecaEntry e;
    memset(&e, 0, sizeof(e));
    e.merkle_root = (uint64_t)bp->stamp_hash | ((uint64_t)bp->stamp_hash << 32);
    e.sha256_hi   = (uint64_t)bp->spatial_xor;
    e.sha256_lo   = bp->window_id;
    e.offset      = bp->circuit_fired;
    e.hop_count   = bp->tring_span;
    e.segment     = bp->top_gaps[0];
    return e;
}

/* ══════════════════════════════════════════════════════════
   CONTEXT
   ══════════════════════════════════════════════════════════ */
typedef struct {
    /* ── P4: full pipeline context ───────────────────────── */
    FtsTwinStore    fts;
    PayloadStore    ps;
    LCTwinGateCtx   lc;

    /* ── P6: Hilbert-keyed pending batch ─────────────────── */
    TGWPendingSlot  pending[TGW_V2_BATCH_MAX];
    uint32_t        pending_n;

    /* ── counters ─────────────────────────────────────────── */
    uint32_t        total_dispatched;
    uint32_t        route_count;
    uint32_t        ground_count;
    uint32_t        verdict_fail;
    uint32_t        no_blueprint;
    uint32_t        batch_flushes;
    uint32_t        sort_skipped;
    uint64_t        total_swaps;

    /* ── cardioid stats ───────────────────────────────────── */
    uint32_t        express_count;      /* Hook A+B: express lane hits */
    uint32_t        cusp_block_count;   /* Gate 1 blocks (noise/anomaly) */
    uint32_t        bucket_hist[GAN_HILBERT_BUCKETS]; /* distribution */
} TGWDispatchV2;

/* ── stats snapshot ──────────────────────────────────────── */
typedef struct {
    uint32_t total_dispatched;
    uint32_t route_count;
    uint32_t ground_count;
    uint32_t verdict_fail;
    uint32_t no_blueprint;
    uint32_t batch_flushes;
    uint32_t sort_skipped;
    uint64_t total_swaps;
    double   avg_swaps_per_flush;
    double   sort_efficiency;
    FtsTwinStats fts;
    /* cardioid */
    uint32_t express_count;
    uint32_t cusp_block_count;
    double   express_ratio;    /* express / total_dispatched */
} TGWDispatchV2Stats;

/* ══════════════════════════════════════════════════════════
   INIT
   NOTE: call geo_addr_net_init() before first dispatch call.
   ══════════════════════════════════════════════════════════ */
static inline void tgw_dispatch_v2_init(TGWDispatchV2 *d, uint64_t root_seed)
{
    memset(d, 0, sizeof(*d));
    fts_init(&d->fts, root_seed);
    pl_init(&d->ps);
    lc_twin_gate_init(&d->lc);
}

/* ══════════════════════════════════════════════════════════
   INTERNAL: FLUSH (sort by Hilbert → callback)
   ══════════════════════════════════════════════════════════ */
static inline void _v2_flush(TGWDispatchV2   *d,
                               TGWBatchFlushFn  flush_fn,
                               void            *flush_ctx)
{
    if (d->pending_n == 0u) return;

    /* counting sort by hilbert_lane (0..GAN_HILBERT_BUCKETS-1)
     * O(N + 120) vs O(N²) insertion — no malloc, stack only
     * total_swaps repurposed: counts output placements (moves) */
    uint64_t tmp_addrs[TGW_V2_BATCH_MAX];
    uint64_t tmp_vals [TGW_V2_BATCH_MAX];

    if (d->pending_n > 1u) {
        uint32_t cnt[GAN_HILBERT_BUCKETS];   /* 120 × 4B = 480B stack */
        memset(cnt, 0, sizeof(cnt));

        /* pass 1: count per bucket */
        for (uint32_t i = 0u; i < d->pending_n; i++)
            cnt[d->pending[i].hilbert_lane]++;

        /* pass 2: prefix sum → start offsets (in-place) */
        uint32_t acc = 0u;
        for (uint32_t b = 0u; b < GAN_HILBERT_BUCKETS; b++) {
            uint32_t c = cnt[b];
            cnt[b] = acc;
            acc += c;
        }

        /* pass 3: scatter into sorted tmp buffers */
        for (uint32_t i = 0u; i < d->pending_n; i++) {
            uint32_t pos = cnt[d->pending[i].hilbert_lane]++;
            tmp_addrs[pos] = d->pending[i].addr;
            tmp_vals [pos] = d->pending[i].val;
            d->total_swaps++;   /* track placements (was: swaps) */
        }
    } else {
        /* n==1: no sort needed */
        tmp_addrs[0] = d->pending[0].addr;
        tmp_vals [0] = d->pending[0].val;
        d->sort_skipped++;
    }

    flush_fn(tmp_addrs, tmp_vals, d->pending_n, flush_ctx);
    d->pending_n = 0u;
    d->batch_flushes++;
}

/* ── push one ROUTE slot into pending ────────────────────── */
static inline void _v2_push(TGWDispatchV2   *d,
                              uint64_t          addr,
                              uint64_t          val,
                              uint16_t          hilbert_lane,
                              TGWBatchFlushFn   flush_fn,
                              void             *flush_ctx)
{
    TGWPendingSlot *s = &d->pending[d->pending_n];
    s->addr         = addr;
    s->val          = val;
    s->hilbert_lane = hilbert_lane;
    s->_pad[0]      = 0u;
    d->pending_n++;
    if (d->pending_n >= TGW_V2_BATCH_MAX)
        _v2_flush(d, flush_fn, flush_ctx);
}

/* ══════════════════════════════════════════════════════════
   CORE DISPATCH
   ══════════════════════════════════════════════════════════
 * GROUND path: SEAM 2 polarity check → ground_fn callback
 *   (~5ns/pkt, bypasses tgw_write entirely)
 *
 * ROUTE path:
 *   - no blueprint: accumulate directly (pre-blueprint batch)
 *   - blueprint ready: geomatrix_batch_verdict
 *       verdict=true  → push to Hilbert pending + fts_write
 *       verdict=false → flush boundary + ground_fn fallback
 *
 * bundle: ctx->_bundle (pass NULL if no blueprint expected)
 */
static inline void tgw_dispatch_v2(TGWDispatchV2   *d,
                                     TGWCtx          *ctx,
                                     const TGWResult *r,
                                     uint64_t         addr,
                                     uint64_t         val,
                                     const uint64_t  *bundle,
                                     TGWBatchFlushFn  flush_fn,
                                     TGWGroundFn      ground_fn,
                                     void            *fn_ctx)
{
    d->total_dispatched++;

    /* ── SEAM 2 decode (O(1) LUT) ─────────────────────────── */
    GeoNetAddr geo = geo_net_encode(addr);

    /* ── GROUND early-exit (~5ns) ─────────────────────────── */
    if (geo.polarity) {
        /* HOOK A: 2-gate cardioid bypass */
        CardioidDecision cd = tgw_cardioid_decide(addr, (uint32_t)val,
                                                   CARDIOID_M_DEFAULT);
        if (cd.use_express) {
            uint16_t bucket = cd.next_pos / 6u;
            _v2_push(d, addr, (uint32_t)val, bucket, flush_fn, fn_ctx);
            d->bucket_hist[bucket]++;
            d->express_count++;
            d->route_count++;
            return;
        }
        /* cusp block: cardioid_pass returned CUSP → count anomaly */
        if (cardioid_pass(cd.pos, cd.r) == CARDIOID_PASS_CUSP)
            d->cusp_block_count++;
        ground_fn(addr, val, fn_ctx);
        d->lc.gate_counts[LC_GATE_GROUND]++;
        d->ground_count++;
        return;
    }

    /* ── no blueprint: accumulate for batch ──────────────── */
    if (!r->gpr.blueprint_ready) {
        d->no_blueprint++;
        d->route_count++;
        /* HOOK B: 2-gate cardioid bucket override */
        CardioidDecision cd = tgw_cardioid_decide(addr, (uint32_t)val,
                                                   CARDIOID_M_DEFAULT);
        uint16_t bucket;
        if (cd.use_express) {
            bucket = cd.next_pos / 6u;
            d->bucket_hist[bucket]++;   /* express-only distribution */
            d->express_count++;
        } else {
            if (cardioid_pass(cd.pos, cd.r) == CARDIOID_PASS_CUSP)
                d->cusp_block_count++;
            bucket = geo.hilbert_idx;
        }
        _v2_push(d, addr, val, bucket, flush_fn, fn_ctx);
        return;
    }

    /* ── blueprint ready: geomatrix verdict ──────────────── */
    const GBBlueprint *bp = &r->gpr.bp;
    GeoPacket batch[GEOMATRIX_PATHS];
    for (int p = 0; p < GEOMATRIX_PATHS; p++)
        batch[p] = _v2_bp_to_pkt(bp, bundle, (uint8_t)p);

    GeomatrixStatsV3 stats = {0};
    bool verdict = geomatrix_batch_verdict(batch, bundle, &stats);

    if (verdict) {
        /* ROUTE: push — HOOK B 2-gate cardioid override */
        CardioidDecision cd = tgw_cardioid_decide(addr, (uint32_t)val,
                                                   CARDIOID_M_DEFAULT);
        uint16_t bucket;
        if (cd.use_express) {
            bucket = cd.next_pos / 6u;
            d->bucket_hist[bucket]++;   /* express-only distribution */
            d->express_count++;
        } else {
            if (cardioid_pass(cd.pos, cd.r) == CARDIOID_PASS_CUSP)
                d->cusp_block_count++;
            bucket = geo.hilbert_idx;
        }
        _v2_push(d, addr, val, bucket, flush_fn, fn_ctx);

        /* fts_write independent of batch (blueprint fields) */
        DodecaEntry e       = _v2_bp_to_dodeca(bp);
        uint64_t route_addr = (uint64_t)bp->stamp_hash;
        uint64_t route_val  = (uint64_t)bp->spatial_xor;
        fts_write(&d->fts, route_addr, route_val, &e);
        d->route_count++;
    } else {
        /* verdict=false: flush boundary → GROUND fallback */
        _v2_flush(d, flush_fn, fn_ctx);
        ground_fn(addr, val, fn_ctx);
        d->verdict_fail++;
        d->ground_count++;
    }

    (void)ctx;  /* reserved: ctx used by tgw_write_dispatch_v2 wrapper */
}

/* ── explicit flush (call at end of stream) ──────────────── */
static inline void tgw_dispatch_v2_flush(TGWDispatchV2   *d,
                                           TGWBatchFlushFn  flush_fn,
                                           void            *fn_ctx)
{
    _v2_flush(d, flush_fn, fn_ctx);
}

/* ── convenience: tgw_write + dispatch in one call ────────── */
static inline TGWResult tgw_write_dispatch_v2(TGWCtx          *ctx,
                                                TGWDispatchV2   *d,
                                                uint64_t         addr,
                                                uint64_t         val,
                                                uint8_t          slot_hot,
                                                TGWBatchFlushFn  flush_fn,
                                                TGWGroundFn      ground_fn,
                                                void            *fn_ctx)
{
    TGWResult r = tgw_write(ctx, addr, val, slot_hot);
    tgw_dispatch_v2(d, ctx, &r, addr, val, ctx->_bundle,
                    flush_fn, ground_fn, fn_ctx);
    return r;
}

/* ══════════════════════════════════════════════════════════
   DEFAULT CALLBACKS (concrete wrappers for common case)
   ══════════════════════════════════════════════════════════
 * Usage:
 *   TGWBatchFlushFn flush = tgw_v2_default_flush_fn;
 *   TGWGroundFn     gnd   = tgw_v2_default_ground_fn;
 *   // fn_ctx = TGWDispatchV2* (ps + ctx both accessible)
 *
 * For real use: define your own wrappers that carry TGWCtx +
 * TGWDispatchV2* in a thin context struct.
 */

/* Internal context for default callbacks */
typedef struct {
    TGWCtx        *tgw_ctx;
    TGWDispatchV2 *dispatch;
} TGWDefaultCallbackCtx;

/* Some integration builds provide tgw_batch from external TGW wire layers. */
void tgw_batch(TGWCtx *ctx,
               const uint64_t *addrs,
               const uint64_t *vals,
               uint32_t n,
               int flush_mode);

static inline void tgw_v2_default_flush_fn(const uint64_t *addrs,
                                             const uint64_t *vals,
                                             uint32_t        n,
                                             void           *ctx)
{
    TGWDefaultCallbackCtx *c = (TGWDefaultCallbackCtx *)ctx;
    tgw_batch(c->tgw_ctx, addrs, vals, n, 0);
}

static inline void tgw_v2_default_ground_fn(uint64_t addr,
                                              uint64_t val,
                                              void    *ctx)
{
    TGWDefaultCallbackCtx *c = (TGWDefaultCallbackCtx *)ctx;
    pl_write(&c->dispatch->ps, addr, val);
}

/* ══════════════════════════════════════════════════════════
   STATS
   ══════════════════════════════════════════════════════════ */
static inline TGWDispatchV2Stats tgw_dispatch_v2_stats(const TGWDispatchV2 *d)
{
    TGWDispatchV2Stats s;
    s.total_dispatched = d->total_dispatched;
    s.route_count      = d->route_count;
    s.ground_count     = d->ground_count;
    s.verdict_fail     = d->verdict_fail;
    s.no_blueprint     = d->no_blueprint;
    s.batch_flushes    = d->batch_flushes;
    s.sort_skipped     = d->sort_skipped;
    s.total_swaps      = d->total_swaps;
    s.fts              = fts_stats(&d->fts);

    uint32_t real_flushes = d->batch_flushes - d->sort_skipped;
    if (real_flushes > 0u) {
        s.avg_swaps_per_flush = (double)d->total_swaps / (double)real_flushes;
        double max_swaps = (double)real_flushes * 2016.0;
        s.sort_efficiency = 1.0 - ((double)d->total_swaps / max_swaps);
    } else {
        s.avg_swaps_per_flush = 0.0;
        s.sort_efficiency     = 1.0;
    }
    s.express_count      = d->express_count;
    s.cusp_block_count   = d->cusp_block_count;
    s.express_ratio      = d->total_dispatched
                           ? (double)d->express_count / (double)d->total_dispatched
                           : 0.0;
    return s;
}

/* ── bucket distribution dump + skew check ──────────────── */
static inline void tgw_dump_bucket_dist(const TGWDispatchV2 *d)
{
    printf("bucket_hist (0..%u):\n", GAN_HILBERT_BUCKETS - 1u);
    for (uint32_t i = 0; i < GAN_HILBERT_BUCKETS; i++) {
        printf("%3u:%-4u", i, d->bucket_hist[i]);
        if ((i + 1u) % 12u == 0u) printf("\n");
    }
    printf("\n");
}

static inline double tgw_bucket_skew(const TGWDispatchV2 *d)
{
    uint32_t mn = UINT32_MAX, mx = 0;
    for (uint32_t i = 0; i < GAN_HILBERT_BUCKETS; i++) {
        if (d->bucket_hist[i] < mn) mn = d->bucket_hist[i];
        if (d->bucket_hist[i] > mx) mx = d->bucket_hist[i];
    }
    return (mn == 0u) ? 0.0 : (double)mx / (double)mn;
}

/* ── pending count (debug) ───────────────────────────────── */
static inline uint32_t tgw_dispatch_v2_pending(const TGWDispatchV2 *d)
{
    return d->pending_n;
}

#endif /* TGW_DISPATCH_V2_H */
