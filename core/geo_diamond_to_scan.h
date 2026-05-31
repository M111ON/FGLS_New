/*
 * geo_diamond_to_scan.h — DiamondBlock → ScanEntry Bridge
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Missing junction between geo_diamond_field.h and pogls_hilbert64_encoder.h
 *
 *  Pipeline (complete):
 *    raw file bytes
 *        ↓  64B chunks
 *    DiamondBlock  [geo_diamond_field.h / pogls_fold.h]
 *        ↓  THIS FILE  diamond_to_scan_entry()
 *    ScanEntry     [pogls_scanner.h compatible]
 *        ↓  h64_feed()
 *    HilbertPacket64  [pogls_hilbert64_encoder.h]
 *        ↓  h64_to_geopixel()
 *    H64GeopixelBridge → hamburger encode
 *
 *  Mapping rules (DiamondBlock → ScanEntry):
 *
 *    seed:
 *      fold_fibo_intersect(block) → 64-bit fingerprint
 *      XOR with invert for pair-verification seed
 *      = content-dependent but geometry-stable
 *
 *    ThetaCoord (face, edge, z):
 *      face  = core_face_id(core) % 12       → dodecahedron face 0..11
 *      edge  = core_engine_id(core) % 5      → face edge 0..4
 *      z     = (vector_pos >> 16) & 0xFF     → depth 0..255
 *
 *    chunk_idx:
 *      hop_count from DiamondFlowCtx (position in stream)
 *
 *    flags:
 *      drift > 0             → SCAN_FLAG_DRIFT  (non-intact cell)
 *      world == WORLD_B      → SCAN_FLAG_SHADOW (witness path)
 *      fibo_gear >= 9        → SCAN_FLAG_PASSTHRU (G3 blast = pre-compressed)
 *
 *  4-field integration (1440 = 4 × 360):
 *    field assigned from fibo_gear:
 *      G1 (0-3)  → FIELD_SPATIAL   (direct geometry address)
 *      G2 (4-8)  → FIELD_TEMPORAL  (batch sequence event)
 *      G3 (9-15) → FIELD_CHROMA    (blast = residual color)
 *      WORLD_B   → FIELD_GHOST     (shadow/witness path)
 *
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GEO_DIAMOND_TO_SCAN_H
#define GEO_DIAMOND_TO_SCAN_H

#include <stdint.h>
#include <string.h>
#include "pogls_fold.h"          /* DiamondBlock, CoreSlot, fold_fibo_intersect */
#include "geo_diamond_field.h"   /* DiamondFlowCtx, diamond_drift_score         */

/* ── ScanEntry (self-contained — no pogls_scanner.h needed) ────────── */

/* Use canonical ScanEntry from pogls_scanner.h (64B, DiamondBlock-aligned)  */
/* field_id stored in reserved[0]; tail/drift/shadow use upper flag bits      */
#include "pogls_scanner.h"

/* flags used by geo_diamond_to_scan (extend SCAN_FLAG_* namespace)           */
#define SCAN_FLAG_PASSTHRU  0x02u   /* pre-compressed — skip codec      */
#define SCAN_FLAG_TAIL      0x04u   /* last entry in stream             */
#define SCAN_FLAG_DRIFT     0x08u   /* cell has drift from baseline     */
#define SCAN_FLAG_SHADOW    0x10u   /* World B / witness path           */

/* field_id accessor — stored in ScanEntry.reserved[0]                        */
#define scan_field_id(e)        ((e)->reserved[0])
#define scan_field_id_set(e,v)  ((e)->reserved[0] = (v))

/* ── Field assignment from DiamondBlock ─────────────────────────────── */
/*
 *  fibo_gear maps directly to 4-field clock:
 *    G1 (0..3)  → FIELD_SPATIAL   — position dominant
 *    G2 (4..8)  → FIELD_TEMPORAL  — sequence dominant
 *    G3 (9..15) → FIELD_CHROMA    — residual / blast
 *    World B    → FIELD_GHOST     — shadow/witness
 */
#define D2S_FIELD_SPATIAL   0u
#define D2S_FIELD_TEMPORAL  1u
#define D2S_FIELD_CHROMA    2u
#define D2S_FIELD_GHOST     3u

static inline uint8_t d2s_field(const DiamondBlock *b) {
    if (core_world(b->core) == WORLD_B) return D2S_FIELD_GHOST;
    uint8_t gear = core_fibo_gear(b->core);
    if (gear <= 3u)  return D2S_FIELD_SPATIAL;
    if (gear <= 8u)  return D2S_FIELD_TEMPORAL;
    return D2S_FIELD_CHROMA;
}

/* ── Main bridge function ────────────────────────────────────────────── */
/*
 * diamond_to_scan_entry(block, ctx, baseline, chunk_idx, out)
 *
 * Converts 1 DiamondBlock → 1 ScanEntry.
 * ctx      : DiamondFlowCtx (accumulated route_addr + hop_count)
 * baseline : reference intersect for drift check
 * chunk_idx: position in file stream (or use ctx->hop_count)
 *
 * Returns 1 if cell passed gate (should be encoded),
 *         0 if cell dropped (drift too high or XOR audit fail).
 *
 * Note: does NOT call diamond_gate() — caller decides threshold.
 *       Use diamond_drift_score() + diamond_xor_ok() for custom policy.
 */
static inline int diamond_to_scan_entry(
    const DiamondBlock  *b,
    const DiamondFlowCtx *ctx,
    uint64_t             baseline,
    uint32_t             chunk_idx,
    ScanEntry           *out)
{
    if (!b || !out) return 0;

    /* ── seed: fibo intersect XOR invert ── */
    uint64_t intersect = fold_fibo_intersect(b);
    out->seed = intersect ^ b->invert;   /* pair-stable fingerprint      */

    /* ── ThetaCoord from CoreSlot bits ── */
    uint8_t  face_raw = core_face_id(b->core);
    uint8_t  eng_raw  = core_engine_id(b->core);
    uint32_t vpos     = core_vector_pos(b->core);

    out->coord.face = face_raw % 12u;           /* 0..11 dodecahedron    */
    out->coord.edge = eng_raw  %  5u;           /* 0..4  face edge       */
    out->coord.z    = (uint8_t)((vpos >> 16) & 0xFFu);  /* depth 0..255 */

    /* ── chunk_idx ── */
    out->chunk_idx = chunk_idx > 0u ? chunk_idx : (uint32_t)ctx->hop_count;

    /* ── field assignment ── */
    scan_field_id_set(out, d2s_field(b));

    /* ── flags ── */
    out->flags = 0u;

    uint32_t drift = diamond_drift_score(b, baseline);
    if (drift > 0u)
        out->flags |= SCAN_FLAG_DRIFT;

    if (core_world(b->core) == WORLD_B)
        out->flags |= SCAN_FLAG_SHADOW;

    if (core_fibo_gear(b->core) >= 9u)
        out->flags |= SCAN_FLAG_PASSTHRU;   /* G3 blast = pre-compressed */

    /* ── checksum ── */
    out->checksum = (uint32_t)(out->seed & 0xFFFFFFFFu);

    /* _pad removed: using canonical ScanEntry */
    return 1;
}

/* ── Batch converter: raw bytes → ScanEntry stream ──────────────────── */
/*
 * diamond_scan_stream(data, data_sz, baseline, out_entries, out_cap)
 *
 * Full pipeline: raw bytes → DiamondBlock (64B chunks) → ScanEntry[]
 * Returns number of ScanEntries written.
 *
 * data     : raw file bytes
 * data_sz  : byte count (must be multiple of 64 for full blocks)
 * baseline : from diamond_batch_baseline() — call once per session
 * out_entries: caller-allocated ScanEntry array
 * out_cap  : max entries
 *
 * Partial last block (< 64B): zero-padded, SCAN_FLAG_TAIL set.
 */
static inline uint32_t diamond_scan_stream(
    const uint8_t  *data,
    uint32_t        data_sz,
    uint64_t        baseline,
    ScanEntry      *out_entries,
    uint32_t        out_cap)
{
    if (!data || !out_entries || out_cap == 0u) return 0u;

    DiamondFlowCtx ctx;
    diamond_flow_init(&ctx);

    uint32_t n_blocks = (data_sz + 63u) / 64u;
    uint32_t written  = 0u;

    for (uint32_t i = 0u; i < n_blocks && written < out_cap; i++) {
        DiamondBlock b;
        uint32_t offset = i * 64u;
        uint32_t avail  = data_sz - offset;

        if (avail >= 64u) {
            /* full block — direct cast (aligned copy) */
            memcpy(&b, data + offset, 64u);
        } else {
            /* partial last block — zero-pad */
            memset(&b, 0, sizeof(b));
            memcpy(&b, data + offset, avail);
        }

        /* accumulate route */
        uint64_t intersect = fold_fibo_intersect(&b);
        ctx.route_addr = diamond_route_update(ctx.route_addr, intersect);
        ctx.hop_count++;

        ScanEntry *entry = &out_entries[written];
        if (diamond_to_scan_entry(&b, &ctx, baseline, i, entry)) {
            /* last block flag */
            if (i == n_blocks - 1u)
                entry->flags |= SCAN_FLAG_TAIL;
            /* route_addr XOR into seed for stream-position uniqueness */
            entry->seed ^= ctx.route_addr;
            entry->checksum = (uint32_t)(entry->seed & 0xFFFFFFFFu);
            written++;
        }
    }

    return written;
}

/* ── Reverse: ScanEntry → DiamondBlock reconstruct ──────────────────── */
/*
 * scan_to_diamond(entry, baseline, out_block)
 *
 * Reconstructs DiamondBlock from ScanEntry.
 * Used for decode path: given seed + coord → rebuild CoreSlot.
 *
 * Note: quad_mirror and honeycomb are re-derived (not stored in ScanEntry).
 * This gives you the logical DiamondBlock, not the exact original bytes.
 * For exact byte recovery, use hb_codec_invert() on the encoded payload.
 */
static inline void scan_to_diamond(
    const ScanEntry *entry,
    uint64_t         baseline,
    DiamondBlock    *out)
{
    if (!entry || !out) return;
    memset(out, 0, sizeof(*out));

    /* rebuild CoreSlot from coord */
    uint8_t  face_id   = entry->coord.face;          /* 0..11          */
    uint8_t  engine_id = entry->coord.edge;           /* 0..4 as engine */
    uint32_t vpos      = (uint32_t)entry->coord.z << 16;

    /* field_id → fibo_gear representative value */
    uint8_t gear = 0u;
    switch (scan_field_id(entry)) {
        case D2S_FIELD_SPATIAL:  gear = 1u;  break;
        case D2S_FIELD_TEMPORAL: gear = 5u;  break;
        case D2S_FIELD_CHROMA:   gear = 10u; break;
        case D2S_FIELD_GHOST:    gear = 1u; engine_id |= 0x40u; break;
    }

    out->core = core_slot_build(face_id, engine_id, vpos, gear, 0u);
    out->invert = ~out->core.raw;

    /* rebuild quad_mirror */
    fold_build_quad_mirror(out);

    (void)baseline;  /* reserved for drift verify on reconstruct */
}

/* ── Test invariants ─────────────────────────────────────────────────── */
/*
 * D01: diamond_to_scan_entry on intact block → returns 1
 * D02: coord.face in [0..11], coord.edge in [0..4], coord.z in [0..255]
 * D03: seed = fold_fibo_intersect XOR invert XOR route_addr (after stream)
 * D04: G3 gear block → SCAN_FLAG_PASSTHRU set
 * D05: World B block → SCAN_FLAG_SHADOW + field_id == D2S_FIELD_GHOST
 * D06: diamond_scan_stream on 64B input → 1 entry, SCAN_FLAG_TAIL set
 * D07: diamond_scan_stream on 128B input → 2 entries, last has TAIL
 * D08: scan_to_diamond + fold_build_quad_mirror → XOR audit passes
 */

#endif /* GEO_DIAMOND_TO_SCAN_H */
