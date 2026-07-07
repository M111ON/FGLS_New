/*
 * pogls_to_geopixel.h — POGLS H64 → Geopixel Bridge
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Converts HilbertPacket64 output into HbTileIn[] array
 *  ready to feed directly into hamburger_encode().
 *
 *  Pipeline:
 *    ScanEntry stream
 *        ↓  pogls_hilbert64_encoder.h
 *    HilbertPacket64  (64 cells, RGB balanced, invert derived)
 *        ↓  THIS FILE
 *    HbTileIn[]       (tile array for hamburger_encode)
 *        ↓  hamburger_encode.h
 *    .gpx5 output
 *
 *  Mapping rules:
 *    HilbertCell → HbTileIn
 *      cell.seed       → tile.seed  (global_seed XOR cell.seed)
 *      cell.face       → tile pixel pattern via h64_cell_to_pixels()
 *      cell.channel    → YCgCo plane selection (0=Y 1=Cg 2=Co)
 *      cell.path_id    → tile_id slot (0..63)
 *      cell.flags      → passthru / skip flags
 *
 *  Tile pixel synthesis:
 *    Each HilbertCell generates HB_TILE_W × HB_TILE_H pixels
 *    from seed via hb_predict() — same function hamburger uses internally
 *    This ensures tile classifier sees the same distribution as
 *    seed-derived prediction → FLAT tiles compress to near-zero
 *
 *  RGB → YCgCo:
 *    channel 0 (R-faces) → Y  plane
 *    channel 1 (G-faces) → Cg plane
 *    channel 2 (B-faces) → Co plane
 *    ghost zone (slots 48..63) → Y plane, residual flag set
 *
 *  Header (16B):
 *    magic + epoch + chunk_count + face_count + ghost_phase + version + flags
 *    Everything else derives from encoder state — no additional storage.
 *
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_TO_GEOPIXEL_H
#define POGLS_TO_GEOPIXEL_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "pogls_hilbert64_encoder.h"

/* ── Tile geometry (must match hamburger_encode.h) ──────────────────── */

#ifndef HB_TILE_W
#define HB_TILE_W  8u
#endif
#ifndef HB_TILE_H
#define HB_TILE_H  8u
#endif
#define HB_TILE_PIXELS  (HB_TILE_W * HB_TILE_H)   /* 64 pixels = 1 DiamondBlock */

/* ── Header ─────────────────────────────────────────────────────────── */

#define H64_BRIDGE_MAGIC  0x48363450u   /* "H64P" */

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* H64_BRIDGE_MAGIC                         */
    uint32_t epoch;        /* reshape cycle count from encoder         */
    uint32_t chunk_count;  /* total ScanEntries fed                    */
    uint8_t  face_count;   /* always 12 (frozen)                       */
    uint8_t  ghost_phase;  /* ghost zone phase 0..3                    */
    uint8_t  version;      /* format version = 1                       */
    uint8_t  flags;        /* H64_BFLAG_*                              */
} H64BridgeHeader;         /* 16B exactly */

#define H64_BFLAG_GEOMETRIC  0x01u   /* FIDX_FLAG_GEOMETRIC compatible  */
#define H64_BFLAG_GHOST_LIVE 0x02u   /* ghost zone has valid residuals   */
#define H64_BFLAG_RGB_BALANCED 0x04u /* verified 12-12-12 balance        */

/* ── HbTileIn (minimal def if not included from hamburger_encode.h) ── */

#ifndef H64TILEIN_DEFINED
#define H64TILEIN_DEFINED
typedef struct {
    uint8_t  pixels[HB_TILE_PIXELS];  /* raw tile pixels (YCgCo plane)  */
    uint32_t seed;                     /* per-tile seed for prediction   */
    uint32_t tile_id;                  /* slot index 0..63               */
    uint8_t  channel;                  /* 0=Y 1=Cg 2=Co                  */
    uint8_t  flags;                    /* HB_TILE_FLAG_*                 */
    uint8_t  _pad[2];
} H64TileIn;  /* 72B */

#define HB_TILE_FLAG_PASSTHRU  0x01u
#define HB_TILE_FLAG_GHOST     0x02u
#define HB_TILE_FLAG_INVERT    0x04u
#define HB_TILE_FLAG_SKIP      0x08u   /* FLAT tile — seed only, no pixels */
#endif

/* ── Pixel synthesis from seed ──────────────────────────────────────── */

/*
 * h64_cell_to_pixels(seed, out_pixels)
 *
 * Synthesizes 64 pixels from cell seed using same hb_predict() formula
 * that hamburger uses internally. This guarantees that tiles derived from
 * seed-only cells will classify as FLAT → compress to ~4B (seed only).
 *
 * For cells with actual chunk data, caller should XOR predict with
 * delta from original chunk to preserve content.
 */
static inline void h64_cell_to_pixels(uint32_t seed, uint8_t *out)
{
    for (uint32_t i = 0u; i < HB_TILE_PIXELS; i++) {
        uint32_t s = seed ^ (i * 0x9e3779b9u);
        s ^= s >> 16; s *= 0x85ebca6bu; s ^= s >> 13;
        out[i] = (uint8_t)(s & 0xFFu);
    }
}

/* ── Bridge result ───────────────────────────────────────────────────── */

typedef struct {
    H64BridgeHeader header;
    H64TileIn tiles[H64_SLOTS];   /* 64 tiles ready for hamburger  */
    uint32_t        n_tiles;            /* valid tile count              */
    uint8_t         rgb_r;              /* balance check: R slot count   */
    uint8_t         rgb_g;              /* balance check: G slot count   */
    uint8_t         rgb_b;              /* balance check: B slot count   */
    uint8_t         _pad;
} H64GeopixelBridge;

/* ── Main conversion function ───────────────────────────────────────── */

/*
 * h64_to_geopixel(enc, out_bridge)
 *
 * Converts finalized H64Encoder state into HbTileIn[] array.
 * Caller must have called h64_finalize(enc) first.
 *
 * Steps:
 *  1. Build header from encoder state
 *  2. For each valid cell → synthesize pixels from seed
 *  3. Map channel: R→Y, G→Cg, B→Co
 *  4. Set passthru/ghost/invert flags
 *  5. Verify RGB balance → set H64_BFLAG_RGB_BALANCED
 *
 * Returns number of tiles written (0..64).
 */
static inline uint32_t h64_to_geopixel(const H64Encoder *enc,
                                         H64GeopixelBridge *out)
{
    if (!enc || !out) return 0u;
    memset(out, 0, sizeof(*out));

    /* ── header ── */
    out->header.magic       = H64_BRIDGE_MAGIC;
    out->header.epoch       = enc->pkt.epoch;
    out->header.chunk_count = enc->pkt.filled;
    out->header.face_count  = H64_FACE_COUNT;
    out->header.ghost_phase = enc->pkt.ghost_phase;
    out->header.version     = 1u;
    out->header.flags       = H64_BFLAG_GEOMETRIC;

    /* ── convert each valid cell ── */
    uint32_t n = 0u;
    for (uint8_t s = 0u; s < H64_SLOTS; s++) {
        if (!enc->slot_map[s]) continue;

        const HilbertCell *cell = &enc->pkt.cells[s];
        if (!(cell->flags & H64_FLAG_VALID)) continue;

        H64TileIn *tile = &out->tiles[n];

        /* seed: mix global epoch into cell seed for uniqueness per cycle */
        tile->seed    = (uint32_t)(cell->seed ^ ((uint64_t)enc->pkt.epoch * 0x9e3779b97f4a7c15ULL));
        tile->tile_id = s;

        /* channel: R→Y(0), G→Cg(1), B→Co(2), ghost→Y(0) */
        if (cell->flags & H64_FLAG_GHOST)
            tile->channel = 0u;   /* ghost lands on Y plane */
        else
            tile->channel = cell->channel;   /* 0=R=Y, 1=G=Cg, 2=B=Co */

        /* flags */
        tile->flags = 0u;
        if (cell->flags & H64_FLAG_PASSTHRU) tile->flags |= HB_TILE_FLAG_PASSTHRU;
        if (cell->flags & H64_FLAG_GHOST)    tile->flags |= HB_TILE_FLAG_GHOST;
        if (cell->flags & H64_FLAG_INVERT)   tile->flags |= HB_TILE_FLAG_INVERT;

        /* invert cells → SKIP pixels, hamburger reads seed only */
        if (cell->flags & H64_FLAG_INVERT) {
            tile->flags |= HB_TILE_FLAG_SKIP;
            memset(tile->pixels, 0, HB_TILE_PIXELS);
        } else {
            /* synthesize pixels from seed */
            h64_cell_to_pixels(tile->seed, tile->pixels);
        }

        n++;
    }

    out->n_tiles = n;

    /* ── RGB balance check ── */
    h64_rgb_balance(enc, &out->rgb_r, &out->rgb_g, &out->rgb_b);
    if (out->rgb_r == 12u && out->rgb_g == 12u && out->rgb_b == 12u)
        out->header.flags |= H64_BFLAG_RGB_BALANCED;

    /* ── ghost live flag ── */
    for (uint8_t s = H64_POSITIVE_SLOTS; s < H64_SLOTS; s++) {
        if (enc->slot_map[s]) {
            out->header.flags |= H64_BFLAG_GHOST_LIVE;
            break;
        }
    }

    return n;
}

/* ── Convenience: full pipeline from ScanEntry stream ───────────────── */

/*
 * h64_pipeline(entries, n_entries, path_ids, out_bridge)
 *
 * Full pipeline:
 *   ScanEntry[] → H64Encoder → h64_finalize → H64GeopixelBridge
 *
 * path_ids[i] = which path_id (0..2) to assign to entry[i]
 * If path_ids is NULL → path_id = entry[i].chunk_idx % 3
 *
 * Returns n_tiles written into out_bridge.
 */
static inline uint32_t h64_pipeline(const ScanEntry  *entries,
                                     uint32_t          n_entries,
                                     const uint8_t    *path_ids,
                                     H64GeopixelBridge *out_bridge)
{
    H64Encoder enc;
    h64_encoder_init(&enc);

    for (uint32_t i = 0u; i < n_entries; i++) {
        uint8_t pid = path_ids ? path_ids[i] : (uint8_t)(entries[i].chunk_idx % 3u);
        h64_feed(&enc, &entries[i], pid);
    }

    h64_finalize(&enc);
    return h64_to_geopixel(&enc, out_bridge);
}

/* ── Usage example (compile-time disabled) ──────────────────────────── */
#if 0
void example_usage(void) {
    /* 1. scan a file */
    ScanEntry entries[1024];
    uint32_t  n = pogls_scan_file("input.bin", entries, 1024);

    /* 2. pipeline → bridge */
    H64GeopixelBridge bridge;
    uint32_t n_tiles = h64_pipeline(entries, n, NULL, &bridge);

    /* 3. feed into hamburger */
    HbEncodeCtx ctx;
    hb_encode_init(&ctx, bridge.header.epoch, 8, 8, pipes, 3);
    hamburger_encode("output.gpx5", &ctx, bridge.tiles, n_tiles);

    /* bridge.header is 16B — only thing needed to reconstruct everything */
}
#endif

/* ── Test invariants ────────────────────────────────────────────────── */
/*
 * B01: h64_to_geopixel on fully fed encoder → n_tiles == 64
 * B02: all invert cells → HB_TILE_FLAG_SKIP set, pixels all zero
 * B03: ghost cells → channel == 0 (Y plane), HB_TILE_FLAG_GHOST set
 * B04: H64_BFLAG_RGB_BALANCED set iff rgb_r==rgb_g==rgb_b==12
 * B05: tile.seed == cell.seed XOR epoch-mix (deterministic, no float)
 * B06: h64_pipeline with NULL path_ids → path_id = chunk_idx % 3
 * B07: header.chunk_count == n valid non-ghost non-invert tiles
 */

#endif /* POGLS_TO_GEOPIXEL_H */
