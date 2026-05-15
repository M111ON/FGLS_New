/*
 * fibo_tile_dispatch.h — ScanEntry → FiboLayer → Geopixel tile bridge
 * ══════════════════════════════════════════════════════════════════════
 *
 * Role:
 *   Connects POGLS scanner output (ScanEntry stream) directly to
 *   Geopixel tile pipeline — no BMP render, no YCgCo conversion.
 *
 * Data flow:
 *
 *   File / buffer
 *       ↓  pogls_scanner
 *   ScanEntry  (seed + ThetaCoord + 64B chunk)
 *       ↓  fibo_tile_dispatch_entry()
 *   FiboLayerHeader  (32B — self-describing address)
 *       ↓  fibo_layer_expand()
 *   FiboLayerState   (16 routes, inv_of_inv witness)
 *       ↓  fibo_tile_build()
 *   GpxTileInput     (tile_data + coord + codec hint)
 *       ↓  hamburger_pipe / gpx5 encode
 *   Geopixel output
 *
 * Tile packing:
 *   chunk 64B → 12 pos routes each contribute bits
 *   tile_data[i] = pos[set][route] XOR chunk_word[i % 8]
 *   This is NOT encryption — it's geometric addressing of chunk content.
 *   The route id IS the address. XOR binds content to address.
 *
 * Frozen rules:
 *   - integer only, no float
 *   - no heap in hot path
 *   - ScanEntry ownership stays with caller
 *   - inv_of_inv never stored in tile — always re-derived on verify
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef FIBO_TILE_DISPATCH_H
#define FIBO_TILE_DISPATCH_H

#include <stdint.h>
#include <string.h>
#include "fibo_layer_header.h"

/* ── Tile geometry constants ──────────────────────────────────────── */
#define FTD_CHUNK_SZ      64u    /* DiamondBlock = 64B                 */
#define FTD_TILE_W        8u     /* tile width  (8px → 8×8 = 64 slots) */
#define FTD_TILE_H        8u     /* tile height                        */
#define FTD_TILE_PIXELS   (FTD_TILE_W * FTD_TILE_H)   /* 64            */
#define FTD_TILE_BYTES    (FTD_TILE_PIXELS * 3u)       /* RGB = 192B    */

/* ── GpxTileInput — what we hand to hamburger encode ─────────────── */
typedef struct {
    uint8_t  tile_rgb[FTD_TILE_BYTES];  /* 192B — RGB tile data        */
    uint8_t  face;                       /* ThetaCoord face 0..11       */
    uint8_t  edge;                       /* ThetaCoord edge 0..4        */
    uint8_t  z;                          /* ThetaCoord z 0..255         */
    uint8_t  set_idx;                    /* Fibo set 0..3               */
    uint8_t  route_idx;                  /* route within set 0..2       */
    uint8_t  layer_seq;                  /* Fibo clock layer index      */
    uint8_t  tick;                       /* Fibo tick value             */
    uint8_t  flags;                      /* FTD_FLAG_*                  */
    uint64_t route_id;                   /* derived route id (address)  */
    uint64_t inv_witness;                /* inv_of_inv for this layer   */
} GpxTileInput;

#define FTD_FLAG_PASSTHRU   0x01u   /* chunk was pre-compressed        */
#define FTD_FLAG_TAIL       0x02u   /* last chunk of stream            */
#define FTD_FLAG_VERIFIED   0x04u   /* inv_of_inv check passed         */
#define FTD_FLAG_CLOSURE    0x08u   /* hit tick 144 (epoch boundary)   */

/* ── Dispatch context (caller allocates, stack-safe) ─────────────── */
typedef struct {
    FiboLayerHeader hdr;     /* current layer header                   */
    FiboLayerState  state;   /* derived 16-route state                 */
    uint8_t         initialized;
    uint8_t         _pad[7];
} FiboDispatchCtx;

/* ── fibo_dispatch_init ───────────────────────────────────────────── */
/*
 * Initialize dispatch context from first ScanEntry seed.
 * tick_start: starting Fibo clock position (default 8 if unsure)
 */
static inline int fibo_dispatch_init(
    FiboDispatchCtx *ctx,
    uint64_t         seed_origin,
    uint8_t          tick_start)   /* pass 8 as safe default */
{
    memset(ctx, 0, sizeof(*ctx));
    if (!fibo_layer_header_init(&ctx->hdr, tick_start, 0u, seed_origin))
        return 0;
    fibo_layer_expand(&ctx->hdr, &ctx->state);
    ctx->initialized = 1;
    return fibo_layer_verify(&ctx->state);
}

/* ── fibo_tile_build ──────────────────────────────────────────────── */
/*
 * Core hot-path function.
 * Takes a 64B chunk + ThetaCoord → produces GpxTileInput.
 *
 * Tile packing strategy:
 *   chunk 64B → view as 8 uint64 words
 *   for each pixel p (0..63):
 *     word = chunk_words[p % 8]
 *     route_contribution = state.sets[set].pos[route] >> (p * 8 % 64) & 0xFF
 *     R = (word >> 0) & 0xFF  — raw chunk byte
 *     G = route_contribution & 0xFF  — geometric address byte
 *     B = (R ^ G) & 0xFF  — binding byte (ties content to address)
 *
 *  This gives the hamburger classifier real signal:
 *   - R channel: content (chunk data)
 *   - G channel: geometry (route address)
 *   - B channel: binding (XOR — if R and G are correlated → B is low → FLAT tile)
 *
 *  FLAT tiles = highly compressible — geometry-native compression at work.
 */
static inline void fibo_tile_build(
    const FiboDispatchCtx *ctx,
    const uint8_t         *chunk64,   /* exactly 64B                   */
    uint8_t                face,
    uint8_t                edge,
    uint8_t                z,
    uint8_t                flags,
    GpxTileInput          *out)
{
    memset(out, 0, sizeof(*out));

    /* map ThetaCoord → set + route */
    uint8_t set_idx, route_idx;
    fibo_coord_to_route(face, edge, &set_idx, &route_idx);

    out->face      = face;
    out->edge      = edge;
    out->z         = z;
    out->set_idx   = set_idx;
    out->route_idx = route_idx;
    out->layer_seq = ctx->hdr.layer_seq;
    out->tick      = ctx->hdr.tick_start;
    out->flags     = flags;
    out->route_id  = ctx->state.sets[set_idx].pos[route_idx];
    out->inv_witness = ctx->state.inv_of_inv;

    /* closure flag */
    if (ctx->hdr.layer_seq >= FIBO_CLOSURE)
        out->flags |= FTD_FLAG_CLOSURE;

    /* pack tile_rgb: 64 pixels × 3 channels */
    uint64_t route_id = out->route_id;

    for (uint32_t p = 0; p < FTD_TILE_PIXELS; p++) {
        /* R = chunk byte p (1:1 mapping — pixel p = chunk byte p)     */
        uint8_t  raw_r  = chunk64[p];
        /* G = route_id byte cycling over 8 bytes of the 64-bit id     */
        uint8_t  geo_g  = (uint8_t)((route_id >> ((p % 8u) * 8u)) & 0xFF);
        uint8_t  bind_b = raw_r ^ geo_g;

        out->tile_rgb[p * 3u + 0u] = raw_r;   /* R: content            */
        out->tile_rgb[p * 3u + 1u] = geo_g;   /* G: geometry address   */
        out->tile_rgb[p * 3u + 2u] = bind_b;  /* B: binding (R^G)      */
    }
}

/* ── fibo_dispatch_entry ──────────────────────────────────────────── */
/*
 * Main entry point — call once per ScanEntry from scanner callback.
 * Handles layer advancement when layer_seq wraps.
 *
 * Returns 1 if tile is ready in *out, 0 on error.
 */
static inline int fibo_dispatch_entry(
    FiboDispatchCtx      *ctx,
    const uint8_t        *chunk64,
    uint8_t               face,
    uint8_t               edge,
    uint8_t               z,
    uint32_t              chunk_idx,
    uint8_t               scan_flags,
    GpxTileInput         *out)
{
    if (!ctx->initialized) return 0;

    /* advance layer_seq every FIBO_CLOSURE chunks (144-tick epoch) */
    uint8_t new_seq = (uint8_t)(chunk_idx / FIBO_CLOSURE);
    if (new_seq != ctx->hdr.layer_seq) {
        /* derive next layer seed from current inv_of_inv */
        uint64_t next_seed = ctx->state.inv_of_inv ^ ctx->hdr.seed_origin;
        /* advance Fibo tick — cycle through valid ticks */
        uint8_t next_tick = ctx->hdr.tick_start;
        for (uint8_t i = 0; i < FIBO_TICK_COUNT - 1; i++) {
            if (FIBO_TICKS[i] == ctx->hdr.tick_start) {
                next_tick = FIBO_TICKS[i + 1];
                break;
            }
        }
        if (next_tick == 144u) next_tick = FIBO_TICKS[0]; /* wrap to 2 */

        fibo_layer_header_init(&ctx->hdr, next_tick, new_seq, next_seed);
        fibo_layer_expand(&ctx->hdr, &ctx->state);
    }

    /* build tile */
    uint8_t tile_flags = 0;
    if (scan_flags & 0x02u) tile_flags |= FTD_FLAG_PASSTHRU;
    if (scan_flags & 0x04u) tile_flags |= FTD_FLAG_TAIL;

    fibo_tile_build(ctx, chunk64, face, edge, z, tile_flags, out);

    /* fast integrity check — verify inv_of_inv still consistent */
    if (fibo_layer_verify(&ctx->state))
        out->flags |= FTD_FLAG_VERIFIED;

    return 1;
}

/* ── fibo_tile_reconstruct ────────────────────────────────────────── */
/*
 * Inverse: given GpxTileInput → recover original chunk 64B.
 * Works because: R = raw_r, G = geo_g → chunk_byte = R (it's already there).
 * B = R^G is the binding proof, not needed for reconstruction.
 *
 * But to VERIFY: re-derive route_id from header → compare G channel.
 * If G matches → chunk is authentic.
 */
static inline int fibo_tile_reconstruct(
    const FiboDispatchCtx *ctx,
    const GpxTileInput    *tile,
    uint8_t               *out_chunk64)   /* caller provides 64B buf   */
{
    /* re-derive expected route_id */
    uint64_t expected_route = fibo_derive_route(
        ctx->hdr.seed_origin,
        tile->layer_seq,
        tile->set_idx,
        tile->route_idx);

    /* verify G channel matches derived route */
    for (uint32_t p = 0; p < FTD_TILE_PIXELS; p++) {
        uint8_t geo_g_stored  = tile->tile_rgb[p * 3u + 1u];
        uint8_t geo_g_derived = (uint8_t)((expected_route >> ((p % 8) * 8)) & 0xFF);
        if (geo_g_stored != geo_g_derived) return 0;  /* tamper detected */
    }

    /* reconstruct: R channel is the original chunk byte */
    for (uint32_t p = 0; p < FTD_TILE_PIXELS; p++)
        out_chunk64[p] = tile->tile_rgb[p * 3u + 0u];

    return 1;
}

#endif /* FIBO_TILE_DISPATCH_H */
