/*
 * pogls_hilbert64_encoder.h — POGLS Hilbert-64 RGB Encoder
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Input  : ScanEntry stream from pogls_scanner (coord_face 0..11, seed)
 *  Output : HilbertPacket64 — 64-slot Hilbert grid with RGB balanced
 *
 *  Design:
 *    - 12 faces × 4 hilbert paths (3 positive + 1 invert) = 48 slots
 *    - remaining 16 slots = invert accumulator (ghost/residual zone)
 *    - RGB channel = face % 3 → deterministic, no external state
 *    - Hilbert curve maps 64 slots → 8×8 grid (Z-order compatible)
 *    - invert = XOR of 3 positive paths per face group (4 faces per channel)
 *
 *  RGB balance rule:
 *    face 0,3,6,9   → R channel   (4 faces × 3 paths = 12 slots)
 *    face 1,4,7,10  → G channel   (4 faces × 3 paths = 12 slots)
 *    face 2,5,8,11  → B channel   (4 faces × 3 paths = 12 slots)
 *    slot 48..63    → invert accumulator (16 slots, shared RGB ghost)
 *    Total = 48 + 16 = 64 ✓
 *
 *  Hilbert slot assignment:
 *    slot = (face * 4) + path_id   (path_id 0..2 = positive, 3 = invert)
 *    invert slot 48..63 = face_group * 4 + invert_phase (0..3)
 *
 *  Frozen rules:
 *    - no float, no heap, no GPU touch
 *    - PHI constants from pogls_platform.h only
 *    - invert derived ONLY from XOR of 3 positive — never stored externally
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_HILBERT64_ENCODER_H
#define POGLS_HILBERT64_ENCODER_H

#include <stdint.h>
#include <string.h>
#include "pogls_scanner.h"   /* ScanEntry */

/* ── Constants ──────────────────────────────────────────────────────── */

#define H64_SLOTS          64u
#define H64_POSITIVE_PATHS  3u   /* R, G, B per face                  */
#define H64_INVERT_PATH     3u   /* path_id = 3 → invert              */
#define H64_FACE_COUNT     12u   /* dodecahedron faces                 */
#define H64_POSITIVE_SLOTS 48u   /* 12 faces × 4 paths (incl invert)  */
#define H64_GHOST_SLOTS    16u   /* slots 48..63 = residual/ghost zone */

/* ── Channel assignment (RGB balance) ──────────────────────────────── */

/*  face % 3 → channel
 *  0 = R,  1 = G,  2 = B
 *  guaranteed even: 4 faces per channel × 3 paths = 12 slots each = 36
 *  + 4 invert slots per channel = 48 total positive zone             */
static inline uint8_t h64_channel(uint8_t face) {
    return face % 3u;
}

/* ── Slot index ─────────────────────────────────────────────────────── */

/*  slot = face*4 + path_id
 *  path_id: 0,1,2 = positive R/G/B traverse, 3 = invert of this face
 *  ghost zone: slot 48..63 addressed separately                       */
static inline uint8_t h64_slot(uint8_t face, uint8_t path_id) {
    return (uint8_t)((face * 4u) + (path_id & 3u));
}

/* ── Ghost slot (residual zone) ─────────────────────────────────────── */

/*  ghost_slot = 48 + (face_group * 4) + phase
 *  face_group = face / 4  (0..2, one per RGB channel group)
 *  phase      = 0..3 (accumulates across reshape cycle)              */
static inline uint8_t h64_ghost_slot(uint8_t face, uint8_t phase) {
    return (uint8_t)(H64_POSITIVE_SLOTS + ((face / 4u) * 4u) + (phase & 3u));
}

/* ── HilbertCell — 1 slot in the 64-grid ───────────────────────────── */

typedef struct {
    uint64_t seed;        /* XOR-fold fingerprint of chunk            */
    uint32_t chunk_idx;   /* source chunk index                       */
    uint8_t  face;        /* coord_face 0..11                         */
    uint8_t  channel;     /* 0=R 1=G 2=B                              */
    uint8_t  path_id;     /* 0..2 positive, 3=invert, 4=ghost         */
    uint8_t  flags;       /* H64_FLAG_*                               */
} HilbertCell;            /* 16B */

#define H64_FLAG_VALID    0x01u
#define H64_FLAG_INVERT   0x02u   /* this cell is derived (invert)     */
#define H64_FLAG_GHOST    0x04u   /* residual zone cell                */
#define H64_FLAG_PASSTHRU 0x08u   /* pre-compressed, skip tile codec   */

/* ── HilbertPacket64 — full 64-slot frame ──────────────────────────── */

typedef struct {
    HilbertCell cells[H64_SLOTS];  /* 64 × 16B = 1024B                */
    uint64_t    invert_xor[3];     /* accumulated XOR per channel R/G/B */
    uint32_t    filled;            /* count of valid cells             */
    uint32_t    epoch;             /* reshape cycle counter            */
    uint8_t     ghost_phase;       /* current ghost slot phase 0..3    */
    uint8_t     _pad[7];
} HilbertPacket64;                 /* 1024 + 40 = 1064B               */

/* ── Encoder state ──────────────────────────────────────────────────── */

typedef struct {
    HilbertPacket64 pkt;
    uint8_t         slot_map[H64_SLOTS];   /* slot → filled flag       */
} H64Encoder;

/* ── Init ───────────────────────────────────────────────────────────── */

static inline void h64_encoder_init(H64Encoder *enc) {
    memset(enc, 0, sizeof(*enc));
}

/* ── Feed one ScanEntry into encoder ────────────────────────────────── */

/*
 * h64_feed(enc, entry, path_id)
 *
 * path_id 0..2 = positive traversal (caller decides R/G/B route order)
 * path_id 3    = not fed manually — derived by h64_derive_invert()
 *
 * Returns slot index written, or 0xFF on error.
 */
static inline uint8_t h64_feed(H64Encoder   *enc,
                                const ScanEntry *entry,
                                uint8_t          path_id)
{
    if (!entry || path_id > 2u) return 0xFFu;

    uint8_t face = entry->coord.face;
    if (face >= H64_FACE_COUNT) return 0xFFu;

    uint8_t slot = h64_slot(face, path_id);
    if (slot >= H64_POSITIVE_SLOTS) return 0xFFu;

    HilbertCell *cell = &enc->pkt.cells[slot];
    cell->seed      = entry->seed;
    cell->chunk_idx = entry->chunk_idx;
    cell->face      = face;
    cell->channel   = h64_channel(face);
    cell->path_id   = path_id;
    cell->flags     = H64_FLAG_VALID;

    if (entry->flags & SCAN_FLAG_PASSTHRU)
        cell->flags |= H64_FLAG_PASSTHRU;

    /* accumulate invert XOR per channel */
    enc->pkt.invert_xor[cell->channel] ^= entry->seed;

    enc->slot_map[slot] = 1u;
    enc->pkt.filled++;

    return slot;
}

/* ── Derive invert path (path_id = 3) per face ──────────────────────── */

/*
 * h64_derive_invert(enc, face)
 *
 * Computes invert cell for a face from XOR of its 3 positive paths.
 * invert.seed = cell[face,0].seed ^ cell[face,1].seed ^ cell[face,2].seed
 *
 * This is the "17 does not exist" moment — invert is derived, never stored
 * externally. If all 3 positive paths present → invert is deterministic.
 *
 * Returns slot index of invert cell, or 0xFF if positive paths incomplete.
 */
static inline uint8_t h64_derive_invert(H64Encoder *enc, uint8_t face)
{
    if (face >= H64_FACE_COUNT) return 0xFFu;

    /* check all 3 positive slots present */
    for (uint8_t p = 0u; p < 3u; p++) {
        if (!enc->slot_map[h64_slot(face, p)]) return 0xFFu;
    }

    uint64_t inv_seed = enc->pkt.cells[h64_slot(face, 0)].seed
                      ^ enc->pkt.cells[h64_slot(face, 1)].seed
                      ^ enc->pkt.cells[h64_slot(face, 2)].seed;

    uint8_t slot = h64_slot(face, H64_INVERT_PATH);
    HilbertCell *cell = &enc->pkt.cells[slot];
    cell->seed      = inv_seed;
    cell->chunk_idx = 0xFFFFFFFFu;   /* derived — no source chunk     */
    cell->face      = face;
    cell->channel   = h64_channel(face);
    cell->path_id   = H64_INVERT_PATH;
    cell->flags     = H64_FLAG_VALID | H64_FLAG_INVERT;

    enc->slot_map[slot] = 1u;
    return slot;
}

/* ── Commit ghost/residual delta to ghost zone ───────────────────────── */

/*
 * h64_commit_ghost(enc, face, delta_seed)
 *
 * Writes noise delta that pairs with wireless hilbert into ghost zone.
 * ghost_phase auto-advances per commit.
 * This is the residual zone storage that "has no body but has shape."
 */
static inline uint8_t h64_commit_ghost(H64Encoder *enc,
                                        uint8_t     face,
                                        uint64_t    delta_seed)
{
    uint8_t slot = h64_ghost_slot(face, enc->pkt.ghost_phase);
    if (slot >= H64_SLOTS) return 0xFFu;

    HilbertCell *cell = &enc->pkt.cells[slot];
    cell->seed      = delta_seed;
    cell->chunk_idx = 0xFFFFFFFEu;   /* ghost — no direct source      */
    cell->face      = face;
    cell->channel   = h64_channel(face);
    cell->path_id   = 4u;            /* ghost path                    */
    cell->flags     = H64_FLAG_VALID | H64_FLAG_GHOST;

    enc->slot_map[slot] = 1u;
    enc->pkt.ghost_phase = (enc->pkt.ghost_phase + 1u) & 3u;
    enc->pkt.filled++;

    return slot;
}

/* ── Reconstruct missing path from invert + 2 positive ─────────────── */

/*
 * h64_reconstruct_path(enc, face, missing_path_id, out_seed)
 *
 * If 1 positive path is missing but invert + other 2 are present:
 *   missing.seed = invert.seed ^ path_A.seed ^ path_B.seed
 *
 * Returns true if reconstruction succeeded.
 */
static inline bool h64_reconstruct_path(H64Encoder *enc,
                                         uint8_t     face,
                                         uint8_t     missing_path,
                                         uint64_t   *out_seed)
{
    if (face >= H64_FACE_COUNT || missing_path > 2u || !out_seed)
        return false;

    /* need invert cell */
    uint8_t inv_slot = h64_slot(face, H64_INVERT_PATH);
    if (!enc->slot_map[inv_slot]) return false;

    /* need the other 2 positive paths */
    uint64_t xor_others = enc->pkt.cells[inv_slot].seed;
    for (uint8_t p = 0u; p < 3u; p++) {
        if (p == missing_path) continue;
        uint8_t s = h64_slot(face, p);
        if (!enc->slot_map[s]) return false;
        xor_others ^= enc->pkt.cells[s].seed;
    }

    *out_seed = xor_others;
    return true;
}

/* ── RGB balance check ──────────────────────────────────────────────── */

/*
 * h64_rgb_balance(enc, out_r, out_g, out_b)
 *
 * Counts valid positive cells per channel.
 * Balanced = out_r == out_g == out_b == 12
 */
static inline void h64_rgb_balance(const H64Encoder *enc,
                                    uint8_t *out_r,
                                    uint8_t *out_g,
                                    uint8_t *out_b)
{
    uint8_t cnt[3] = {0, 0, 0};
    for (uint8_t s = 0u; s < H64_POSITIVE_SLOTS; s++) {
        if (!enc->slot_map[s]) continue;
        const HilbertCell *c = &enc->pkt.cells[s];
        if (c->path_id < 3u && (c->flags & H64_FLAG_VALID))
            cnt[c->channel & 2u]++;
    }
    if (out_r) *out_r = cnt[0];
    if (out_g) *out_g = cnt[1];
    if (out_b) *out_b = cnt[2];
}

/* ── Finalize packet (derive all inverts) ───────────────────────────── */

static inline void h64_finalize(H64Encoder *enc)
{
    for (uint8_t f = 0u; f < H64_FACE_COUNT; f++)
        h64_derive_invert(enc, f);
}

/* ── Test invariant reference ───────────────────────────────────────── */
/*
 * H01: feed 3 paths per face × 12 faces → rgb_balance == 12,12,12
 * H02: h64_derive_invert on complete face → slot 48..3 set + INVERT flag
 * H03: corrupt 1 path → h64_reconstruct_path recovers seed correctly
 * H04: h64_commit_ghost × 4 on same face → ghost_phase wraps 0..3
 * H05: invert_xor[channel] == XOR of all seeds in that channel
 */

#endif /* POGLS_HILBERT64_ENCODER_H */
