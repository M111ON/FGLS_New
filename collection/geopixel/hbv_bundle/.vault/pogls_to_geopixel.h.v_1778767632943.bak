/*
 * pogls_to_geopixel.h — POGLS H64 → Geopixel Bridge  (Path A: CODEC_SEED)
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Path A: Pure seed storage — every tile stores seed only (4B)
 *  No pixel synthesis. No content storage. Only geometric address.
 *
 *  Pipeline:
 *    ScanEntry stream
 *        ↓  pogls_hilbert64_encoder.h
 *    HilbertPacket64  (64 cells, RGB balanced, invert derived)
 *        ↓  THIS FILE
 *    H64TileIn[]  →  hamburger CODEC_SEED path
 *        ↓
 *    .gpx5 output:  4B × 64 tiles = 256B per packet
 *
 *  Storage per packet:
 *    positive tiles  : 48 × 4B = 192B
 *    invert tiles    : 12 × 4B =  48B  (derived, verify-only)
 *    ghost tiles     : 0..4 × 4B
 *    header          : 16B  (one per file)
 *    ──────────────────────────────────
 *    max per packet  : 256B  vs  4096B raw  →  16× theoretical ratio
 *
 *  Decoder re-derives content from seed + epoch + face_count.
 *  This is geometric fingerprinting, not content storage.
 *
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_TO_GEOPIXEL_H
#define POGLS_TO_GEOPIXEL_H

#include <stdint.h>
#include <string.h>
#include "pogls_hilbert64_encoder.h"

/* ── Header (16B) ───────────────────────────────────────────────────── */

#define H64_BRIDGE_MAGIC  0x48363450u   /* "H64P" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t epoch;
    uint32_t chunk_count;
    uint8_t  face_count;   /* always 12                                */
    uint8_t  ghost_phase;
    uint8_t  version;      /* 1                                        */
    uint8_t  flags;
} H64BridgeHeader;         /* 16B */

#define H64_BFLAG_GEOMETRIC    0x01u
#define H64_BFLAG_GHOST_LIVE   0x02u
#define H64_BFLAG_RGB_BALANCED 0x04u
#define H64_BFLAG_SEED_ONLY    0x08u   /* Path A active                */

/* ── H64TileIn — seed-only tile ─────────────────────────────────────── */

typedef struct {
    uint64_t seed64;     /* full fingerprint (epoch-mixed)             */
    uint32_t seed32;     /* lower 32b → hamburger CODEC_SEED input     */
    uint32_t tile_id;    /* slot 0..63                                 */
    uint8_t  channel;    /* 0=Y(R)  1=Cg(G)  2=Co(B)                  */
    uint8_t  face;       /* 0..11                                      */
    uint8_t  path_id;    /* 0..2 positive  3=invert  4=ghost           */
    uint8_t  flags;      /* H64T_FLAG_*                                */
} H64TileIn;             /* 20B */

#define H64T_FLAG_VALID    0x01u
#define H64T_FLAG_INVERT   0x02u
#define H64T_FLAG_GHOST    0x04u
#define H64T_FLAG_PASSTHRU 0x08u

/* ── Bridge output ───────────────────────────────────────────────────── */

typedef struct {
    H64BridgeHeader header;
    H64TileIn       tiles[H64_SLOTS];
    uint32_t        n_tiles;
    uint8_t         rgb_r, rgb_g, rgb_b;
    uint8_t         _pad;
} H64GeopixelBridge;

/* ── Convert ─────────────────────────────────────────────────────────── */

static inline uint32_t h64_to_geopixel(const H64Encoder  *enc,
                                         H64GeopixelBridge *out)
{
    if (!enc || !out) return 0u;
    memset(out, 0, sizeof(*out));

    out->header.magic       = H64_BRIDGE_MAGIC;
    out->header.epoch       = enc->pkt.epoch;
    out->header.chunk_count = enc->pkt.filled;
    out->header.face_count  = H64_FACE_COUNT;
    out->header.ghost_phase = enc->pkt.ghost_phase;
    out->header.version     = 1u;
    out->header.flags       = H64_BFLAG_GEOMETRIC | H64_BFLAG_SEED_ONLY;

    uint32_t n = 0u;
    for (uint8_t s = 0u; s < H64_SLOTS; s++) {
        if (!enc->slot_map[s]) continue;
        const HilbertCell *cell = &enc->pkt.cells[s];
        if (!(cell->flags & H64_FLAG_VALID)) continue;

        H64TileIn *t  = &out->tiles[n++];
        t->seed64     = cell->seed ^ ((uint64_t)enc->pkt.epoch * 0x9e3779b97f4a7c15ULL);
        t->seed32     = (uint32_t)(t->seed64 & 0xFFFFFFFFu);
        t->tile_id    = s;
        t->face       = cell->face;
        t->path_id    = cell->path_id;
        t->channel    = (cell->flags & H64_FLAG_GHOST) ? 0u : cell->channel;
        t->flags      = H64T_FLAG_VALID;
        if (cell->flags & H64_FLAG_INVERT)   t->flags |= H64T_FLAG_INVERT;
        if (cell->flags & H64_FLAG_GHOST)    t->flags |= H64T_FLAG_GHOST;
        if (cell->flags & H64_FLAG_PASSTHRU) t->flags |= H64T_FLAG_PASSTHRU;
    }

    out->n_tiles = n;

    h64_rgb_balance(enc, &out->rgb_r, &out->rgb_g, &out->rgb_b);
    if (out->rgb_r == 12u && out->rgb_g == 12u && out->rgb_b == 12u)
        out->header.flags |= H64_BFLAG_RGB_BALANCED;

    for (uint8_t s = H64_POSITIVE_SLOTS; s < H64_SLOTS; s++)
        if (enc->slot_map[s]) { out->header.flags |= H64_BFLAG_GHOST_LIVE; break; }

    return n;
}

/* ── Full pipeline (one call) ────────────────────────────────────────── */

static inline uint32_t h64_pipeline(const ScanEntry   *entries,
                                     uint32_t           n_entries,
                                     const uint8_t     *path_ids,
                                     H64GeopixelBridge *out)
{
    H64Encoder enc;
    h64_encoder_init(&enc);
    for (uint32_t i = 0u; i < n_entries; i++) {
        uint8_t pid = path_ids ? path_ids[i]
                               : (uint8_t)(entries[i].chunk_idx % 3u);
        h64_feed(&enc, &entries[i], pid);
    }
    h64_finalize(&enc);
    return h64_to_geopixel(&enc, out);
}

/* ── Storage helpers ─────────────────────────────────────────────────── */

/* Path A: 4B seed per tile + 16B header */
static inline uint32_t h64_storage_bytes(uint32_t n_tiles) {
    return n_tiles * 4u + 16u;
}

/* ratio vs raw (n_tiles × 64B) */
static inline float h64_ratio(uint32_t n_tiles) {
    return (float)(n_tiles * 64u) / (float)h64_storage_bytes(n_tiles);
}
/* h64_ratio(64) = 4096 / 272 ≈ 15.06× */

/* ── Test invariants ─────────────────────────────────────────────────── */
/*
 * C01: n_tiles == 64 after full feed + finalize on 12-face input
 * C02: all tiles H64T_FLAG_VALID set
 * C03: invert tiles: H64T_FLAG_INVERT, path_id == 3
 * C04: ghost tiles:  H64T_FLAG_GHOST,  channel == 0
 * C05: H64_BFLAG_RGB_BALANCED when rgb_r==rgb_g==rgb_b==12
 * C06: H64_BFLAG_SEED_ONLY always set
 * C07: h64_ratio(64) ≈ 15.06×
 * C08: invert tile seed64 == XOR(3 positive seeds for that face) ^ epoch_mix
 */

#endif /* POGLS_TO_GEOPIXEL_H */
