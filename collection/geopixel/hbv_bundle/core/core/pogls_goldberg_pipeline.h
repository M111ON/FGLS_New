/*
 * pogls_goldberg_pipeline.h — Phase 13: Goldberg ↔ Pipeline Wire Integration
 * ═══════════════════════════════════════════════════════════════════════════
 * Wires Phase 12 Goldberg scanner stack into the existing TwinBridge pipeline.
 *
 * P1a: pgb_write() → pipeline_wire_process()   (spatiotemporal fingerprint)
 * P1b: GBBlueprint flush → dodeca_insert()      (structural blueprint record)
 * P1c: pgb_twin_raw() → geo_fast_intersect()   (twin geo spatial feed)
 *
 * Usage:
 *   GoldbergPipeline gp;
 *   goldberg_pipeline_init(&gp, &twin_bridge, seed);
 *
 *   GoldbergPipelineResult r = goldberg_pipeline_write(&gp, addr, value, slot_hot);
 *   // r.fp        — spatiotemporal fingerprint (GBTRingPrint)
 *   // r.pw_fail   — pipeline layer fail flag
 *   // r.blueprint_ready — 1 if GBBlueprint was extracted this op
 *   // r.bp        — valid if blueprint_ready == 1
 *
 * Include chain:
 *   pogls_goldberg_pipeline.h
 *     → pogls_goldberg_bridge.h (Phase 12)
 *         → geo_goldberg_tracker.h
 *         → geo_goldberg_tring_bridge.h → geo_goldberg_scan.h → geo_goldberg_lut.h
 *         → geo_temporal_ring.h → geo_temporal_lut.h
 *     → pogls_twin_bridge.h (TPOGLS_MASTER)
 *         → pogls_pipeline_wire.h → geo_pipeline_wire.h
 *         → geo_dodeca.h
 *
 * Sacred numbers preserved:
 *   144 = flush period | 720 = TRing cycle | 3456 = GB_ADDR_BIPOLAR
 *   60  = GB_N_TRIGAP  | 12  = GB_N_PENTAGON | 6 = GB_N_BIPOLAR_PAIRS
 * ═══════════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_GOLDBERG_PIPELINE_H
#define POGLS_GOLDBERG_PIPELINE_H

#include <stdint.h>
#include <string.h>

/* TPOGLS_MASTER — twin bridge (owns pipeline_wire, dodeca, diamond_flow) */
#include "pogls_twin_bridge.h"

/* Phase 12 — Goldberg scanner stack */
#include "pogls_goldberg_bridge.h"
#include "geo_goldberg_tracker.h"

/* ── GoldbergPipeline context ───────────────────────────────────────────── */
/*
 * Owns the Goldberg bridge + tracker.
 * Holds a pointer to an existing TwinBridge (caller owns it).
 *
 * Design: Goldberg sits BESIDE TwinBridge, not inside it.
 *   - pgb_write() captures spatiotemporal fingerprint
 *   - pipeline_wire_process() runs in TwinBridge.pw (existing path)
 *   - On blueprint flush: dodeca_insert() fires into TwinBridge.dodeca
 *   - pgb_twin_raw() feeds core_raw into TwinBridge's geo_fast_intersect path
 *     via the existing theta_mix64 → face/edge → isect chain
 */
typedef struct {
    PoglsGoldbergBridge  pgb;       /* Phase 12 Goldberg bridge + TRing     */
    GBTracker            tracker;   /* passive blueprint accumulator         */
    TwinBridge          *tb;        /* ref to caller's TwinBridge (not owned)*/
    uint32_t             bp_count;  /* blueprints extracted lifetime         */
} GoldbergPipeline;

/* ── per-op result ──────────────────────────────────────────────────────── */
typedef struct {
    GBTRingPrint  fp;               /* spatiotemporal fingerprint            */
    PipelineResult pw_res;          /* pipeline_wire_process result          */
    uint8_t        pw_fail;         /* any pipeline layer flagged fail       */
    uint8_t        blueprint_ready; /* 1 = GBBlueprint extracted this op     */
    GBBlueprint    bp;              /* valid when blueprint_ready == 1       */
    uint64_t       twin_raw;        /* pgb_twin_raw → fed to twin intersect  */
} GoldbergPipelineResult;

/* ── init ───────────────────────────────────────────────────────────────── */
static inline void goldberg_pipeline_init(GoldbergPipeline *gp,
                                           TwinBridge       *tb,
                                           GeoSeed           seed)
{
    (void)seed; /* TwinBridge already inited with seed by caller */
    memset(gp, 0, sizeof(*gp));

    /* Phase 12 init: tracker → bridge */
    gbt_tracker_init(&gp->tracker);
    pgb_init(&gp->pgb, gbt_tracker_record, &gp->tracker);

    gp->tb = tb;
}

/* ── core write — P1a + P1b + P1c ──────────────────────────────────────── */
/*
 * Step 1 (P1a): pgb_write() → captures spatiotemporal fingerprint.
 *               on_record fires gbt_tracker_record() passively.
 *
 * Step 2 (P1a): pipeline_wire_process() in tb->pw — existing GeoNet/RH/QRPN
 *               path unchanged. Goldberg fp.stamp feeds as logical addr XOR.
 *
 * Step 3 (P1c): pgb_twin_raw() using fp.stamp (frozen before tick per spec).
 *               Result fed into twin_bridge_write() which internally runs
 *               theta_mix64 → face/edge → geo_fast_intersect() — existing path.
 *               We pass twin_raw as `value` override so isect reflects
 *               Goldberg spatial geometry rather than raw addr^value.
 *
 * Step 4 (P1b): If tracker has a blueprint ready (every 144 ops),
 *               extract GBBlueprint → dodeca_insert() with:
 *                 merkle_root = bp.stamp_hash (XOR chain of all stamps)
 *                 sha256_hi   = bp.spatial_xor >> 32
 *                 sha256_lo   = bp.spatial_xor & 0xFFFFFFFF
 *                 offset      = bp.circuit_fired (bitmask of fired pairs)
 *                 hop_count   = bp.tring_span    (temporal range covered)
 *                 segment     = top_gaps[0]       (highest tension gap_id)
 */
static inline GoldbergPipelineResult goldberg_pipeline_write(
    GoldbergPipeline *gp,
    uint32_t          addr,
    uint32_t          value,
    uint8_t           slot_hot)
{
    GoldbergPipelineResult r;
    memset(&r, 0, sizeof(r));

    /* ── P1a: Goldberg spatiotemporal fingerprint ── */
    int flush_ev = 0;
    r.fp = pgb_write(&gp->pgb, addr, value, &flush_ev);

    /* ── P1a: pipeline_wire_process in existing TwinBridge pw ── */
    /* Feed addr XOR fp.stamp so pipeline geometry reflects Goldberg state */
    uint64_t geo_addr  = (uint64_t)addr  ^ (uint64_t)(r.fp.stamp & 0xFFFFFFFFu);
    uint64_t geo_value = (uint64_t)value ^ (uint64_t)(r.fp.tring_enc);
    r.pw_fail = pipeline_wire_process(&gp->tb->pw,
                                       geo_addr, geo_value,
                                       slot_hot, &r.pw_res);

    /* ── P1c: pgb_twin_raw → twin_bridge_write (geo_fast_intersect path) ──
     * stamp_frozen = fp.stamp (captured BEFORE gbt tick — c144 freeze rule)
     * twin_raw encodes Goldberg spatial geometry into the intersect feed.
     * twin_bridge_write runs: raw → theta_mix64 → face/edge → core_raw
     *   → geo_fast_intersect() → diamond_route_update() → dodeca_insert
     * We override value with twin_raw so the intersect path sees
     * Goldberg geometry rather than plain addr^value. */
    uint32_t twin_raw_32 = pgb_twin_raw(addr, value, r.fp.stamp);
    r.twin_raw = (uint64_t)twin_raw_32;

    twin_bridge_write(gp->tb,
                      (uint64_t)addr,
                      r.twin_raw,
                      slot_hot,
                      NULL);  /* PipelineResult already captured above */

    /* ── P1b: Blueprint flush → dodeca_insert ── */
    if (gbt_tracker_ready(&gp->tracker)) {
        r.bp             = gbt_tracker_extract(&gp->tracker);
        r.blueprint_ready = 1;
        gp->bp_count++;

        /* Insert blueprint fingerprint into TwinBridge dodeca table.
         *
         * Field mapping (semantics preserved):
         *   merkle_root = bp.stamp_hash   — XOR chain of 144 stamps
         *   sha256_hi   = spatial_xor>>32 — upper half of spatial fold
         *   sha256_lo   = spatial_xor low — lower half of spatial fold
         *   offset      = circuit_fired   — which of 6 bipolar pairs fired
         *   hop_count   = tring_span      — temporal distance of window
         *   segment     = top_gaps[0]     — highest-tension tetra node
         *
         * Note: dodeca_insert uses open-addressing with LRU eviction (8 probe).
         * blueprint fingerprint is structural — no data content stored. */
        dodeca_insert(&gp->tb->dodeca,
                      (uint64_t)r.bp.stamp_hash,
                      (uint64_t)(r.bp.spatial_xor) >> 32,
                      (uint64_t)(r.bp.spatial_xor) & 0xFFFFFFFFu,
                      r.bp.circuit_fired,
                      r.bp.tring_span,
                      r.bp.top_gaps[0]);
    }

    return r;
}

/* ── batch write ────────────────────────────────────────────────────────── */
static inline void goldberg_pipeline_batch(
    GoldbergPipeline *gp,
    const uint32_t   *addrs,
    const uint32_t   *values,
    uint32_t          n,
    uint8_t           slot_hot)
{
    for (uint32_t i = 0; i < n; i++)
        goldberg_pipeline_write(gp, addrs[i], values ? values[i] : 0u, slot_hot);
}

/* ── status / stats ─────────────────────────────────────────────────────── */
#include <stdio.h>
static inline void goldberg_pipeline_status(const GoldbergPipeline *gp) {
    printf("[GoldbergPipeline] ops=%u  blueprints=%u  pgb_flush_period=%u\n",
           pgb_op_count(&gp->pgb), gp->bp_count, gp->pgb.flush_period);
    printf("  tracker: ready=%d  accumulated=%u/144\n",
           gbt_tracker_ready(&gp->tracker),
           gp->tracker.event_count);
    pipeline_wire_status(&gp->tb->pw);
}

#endif /* POGLS_GOLDBERG_PIPELINE_H */
