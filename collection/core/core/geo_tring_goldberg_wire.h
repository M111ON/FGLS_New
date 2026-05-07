/*
 * geo_tring_goldberg_wire.h — TRing ↔ Goldberg Pipeline Wire (Phase 12+)
 * ════════════════════════════════════════════════════════════════════════
 * Wires together:
 *   TRing  (geo_temporal_ring.h)       — temporal walk position 0..719
 *   TStream (geo_tring_stream.h)       — file chunk slicing + ordering
 *   GoldbergPipeline (pogls_goldberg_pipeline.h)
 *       → PoglsGoldbergBridge          — spatial fingerprint
 *       → TwinBridge                   — POGLS pipeline + diamond flow
 *
 * Usage:
 *   TGWCtx ctx;
 *   tgw_init(&ctx, seed, bundle);
 *   TGWResult r = tgw_write(&ctx, addr, value, slot_hot);
 *   tgw_flush(&ctx);
 *   TGWStats  s = tgw_stats(&ctx);
 *
 * File streaming:
 *   tgw_stream_file(&ctx, data, size)  — slice file → TRing → pipeline
 *   tgw_stream_reconstruct(&ctx, out, cap) → bytes written
 *
 * ════════════════════════════════════════════════════════════════════════
 */

#ifndef GEO_TRING_GOLDBERG_WIRE_H
#define GEO_TRING_GOLDBERG_WIRE_H

#include <stdint.h>
#include <string.h>

/* ── layer order matters: base types first ─────────────────────── */
#include "geo_config.h"
#include "geo_fibo_clock.h"
#include "geo_thirdeye.h"
#include "geo_diamond_field.h"
#include "geo_dodeca.h"
#include "theta_map.h"
#include "geo_route.h"
#include "geo_whe.h"
#include "geo_hardening_whe.h"

/* TRing layers */
#include "geo_temporal_lut.h"
#include "geo_temporal_ring.h"
#include "geo_tring_stream.h"

/* Goldberg spatial layers */
#include "geo_goldberg_lut.h"
#include "geo_goldberg_scan.h"
#include "geo_goldberg_tracker.h"
#include "geo_goldberg_tring_bridge.h"

/* Pipeline layers */
#include "pogls_platform.h"
#include "geo_radial_hilbert.h"
#include "geomatrix_shared.h"
#include "pogls_geomatrix.h"
#include "pogls_qrpn_phaseE.h"
#include "geo_pipeline_wire.h"
#include "pogls_pipeline_wire.h"
#include "geo_s11.h"
#include "pogls_twin_bridge.h"
#include "pogls_goldberg_bridge.h"
#include "pogls_goldberg_pipeline.h"

/* ── TGW context ────────────────────────────────────────────────── */
typedef struct {
    TwinBridge        tb;           /* owns diamond flow + POGLS pipeline  */
    GoldbergPipeline  gp;           /* Goldberg spatial+temporal pipeline  */

    /* TRing stream state */
    TRingCtx          tr;           /* temporal ring — file chunk ordering */
    TStreamChunk      store[TSTREAM_MAX_PKTS];

    /* lifetime counters */
    uint32_t          total_writes;
    uint32_t          blueprint_count;
    uint32_t          stream_pkts_rx;
    uint32_t          stream_gaps;

    /* internal bundle storage (twin_bridge needs stable pointer) */
    uint64_t          _bundle[GEO_BUNDLE_WORDS];
} TGWCtx;

/* ── per-op result ──────────────────────────────────────────────── */
typedef struct {
    GoldbergPipelineResult gpr;     /* fp + pw_res + blueprint if ready    */
    FiboEvent              ev;      /* fibo clock event flags              */
} TGWResult;

/* ── stats snapshot ─────────────────────────────────────────────── */
typedef struct {
    TwinBridgeStats tb;
    uint32_t        total_writes;
    uint32_t        blueprint_count;
    uint32_t        stream_pkts_rx;
    uint32_t        stream_gaps;
    uint32_t        tring_pos;      /* current TRing walk position         */
} TGWStats;

/* ── init ────────────────────────────────────────────────────────── */
static inline void tgw_init(TGWCtx         *ctx,
                             GeoSeed         seed,
                             const uint64_t *bundle)  /* NULL → zero bundle */
{
    memset(ctx, 0, sizeof(*ctx));

    if (bundle)
        memcpy(ctx->_bundle, bundle, GEO_BUNDLE_WORDS * sizeof(uint64_t));

    twin_bridge_init(&ctx->tb, seed, ctx->_bundle);
    goldberg_pipeline_init(&ctx->gp, &ctx->tb, seed);
    tring_init(&ctx->tr);
}

/* ── single write (hot path) ────────────────────────────────────── */
static inline TGWResult tgw_write(TGWCtx  *ctx,
                                   uint64_t addr,
                                   uint64_t value,
                                   uint8_t  slot_hot)
{
    TGWResult r;
    r.gpr = goldberg_pipeline_write(&ctx->gp, addr, value, slot_hot);
    r.ev  = twin_bridge_write(&ctx->tb, addr, value, slot_hot, NULL);

    if (r.gpr.blueprint_ready)
        ctx->blueprint_count++;

    ctx->total_writes++;
    return r;
}

/* ── batch write ────────────────────────────────────────────────── */
/*
 * goldberg_pipeline_batch uses uint32_t (GB_ADDR_BIPOLAR=3456 space).
 * twin_bridge_batch uses uint64_t. Truncate for goldberg layer.
 */
static inline void tgw_batch(TGWCtx         *ctx,
                              const uint64_t *addrs,
                              const uint64_t *values,
                              uint32_t        n,
                              uint8_t         slot_hot)
{
    uint32_t gb_addrs[n], gb_vals[n];
    for (uint32_t i = 0; i < n; i++) {
        gb_addrs[i] = (uint32_t)(addrs[i] & 0xFFFFFFFFu);
        gb_vals[i]  = (uint32_t)(values ? (values[i] & 0xFFFFFFFFu) : 0u);
    }
    goldberg_pipeline_batch(&ctx->gp, gb_addrs, gb_vals, n, slot_hot);
    twin_bridge_batch(&ctx->tb, addrs, values, n, slot_hot);
    ctx->total_writes += n;
}

/* ── file streaming: slice file → TRing + pipeline ─────────────── */
/*
 * Slices up to TSTREAM_MAX_PKTS packets from [data,size].
 * Each packet's enc encodes TRing position → fed into tgw_write
 * so temporal ordering is preserved in the goldberg fingerprint.
 * Returns number of packets ingested.
 */
static inline uint16_t tgw_stream_file(TGWCtx        *ctx,
                                        const uint8_t *data,
                                        uint32_t       size)
{
    TStreamPkt pkts[TSTREAM_MAX_PKTS];
    uint16_t n = tstream_slice_file(pkts, data, size);

    for (uint16_t i = 0; i < n; i++) {
        int snap = tstream_recv_pkt(&ctx->tr, ctx->store, &pkts[i]);
        if (snap < 0) { ctx->stream_gaps++; continue; }

        /* feed chunk enc as addr, crc as value → goldberg gets
         * temporal fingerprint from TRing walk position embedded in enc */
        uint64_t addr  = (uint64_t)pkts[i].enc;
        uint64_t value = (uint64_t)pkts[i].crc16
                       | ((uint64_t)pkts[i].size << 16);
        tgw_write(ctx, addr, value, 0);
        ctx->stream_pkts_rx++;
    }

    return n;
}

/* ── reconstruct output from TRing store ────────────────────────── */
static inline uint32_t tgw_stream_reconstruct(const TGWCtx *ctx,
                                               uint8_t      *out,
                                               uint16_t      n_pkts)
{
    return tstream_reconstruct(&ctx->tr, ctx->store, n_pkts, out);
}

/* ── flush remaining diamond flow ───────────────────────────────── */
static inline void tgw_flush(TGWCtx *ctx)
{
    twin_bridge_flush(&ctx->tb);
}

/* ── gap report ─────────────────────────────────────────────────── */
static inline uint16_t tgw_gap_count(const TGWCtx *ctx, uint16_t to_pos)
{
    return tring_gap_count(&ctx->tr, to_pos);
}

/* ── stats ──────────────────────────────────────────────────────── */
static inline TGWStats tgw_stats(const TGWCtx *ctx)
{
    TGWStats s;
    s.tb              = twin_bridge_stats(&ctx->tb);
    s.total_writes    = ctx->total_writes;
    s.blueprint_count = ctx->blueprint_count;
    s.stream_pkts_rx  = ctx->stream_pkts_rx;
    s.stream_gaps     = ctx->stream_gaps;
    s.tring_pos       = ctx->tr.head;
    return s;
}

/* ── status print ───────────────────────────────────────────────── */
#include <stdio.h>
static inline void tgw_status(const TGWCtx *ctx)
{
    TGWStats s = tgw_stats(ctx);
    printf("[TGW] writes=%u  blueprints=%u  tring_pos=%u\n",
           s.total_writes, s.blueprint_count, s.tring_pos);
    printf("  stream: rx=%u  gaps=%u\n",
           s.stream_pkts_rx, s.stream_gaps);
    printf("  twin: ops=%u  dodeca_writes=%u  flush_count=%u\n",
           s.tb.total_ops, s.tb.twin_writes, s.tb.flush_count);
}

#endif /* GEO_TRING_GOLDBERG_WIRE_H */
