/*
 * geo_o4_connector.h — O4 Connector: GeoPixel v18 tile → TRing → GeoPixel grid
 * ══════════════════════════════════════════════════════════════════════════════
 * Bridges geopixel_v18 encode output → POGLS geometric storage.
 *
 * Pipeline (O4):
 *   encode_tile() blob  (v18 output, variable size)
 *       ↓
 *   slice blob → 3B chunks (GV_CHUNK_BYTES)
 *       ↓
 *   for each chunk: trit/spoke/coset/letter/fibo fields via geo_pixel_encode()
 *       ↓
 *   assign to TringAddr slot via tring_encode(tx, ty, ch)
 *       ↓
 *   write into 27×N pixel grid (W=27 canonical width)
 *       ↓
 *   ready for PNG lossless encode (geometric pattern → high compression)
 *
 * Read path (O4 reverse):
 *   PNG decode → 27×N pixel grid
 *       ↓
 *   geo_pixel_decode() → fields
 *       ↓
 *   slot lookup via tring addr → reconstruct blob
 *       ↓
 *   decode_tile_blob() → pixels
 *
 * Grid layout:
 *   Width = 27 (trit space = 3³, canonical W for geo_pixel)
 *   Height = ceil(n_chunks / 27)
 *   Pixel at (x, y): slot_idx = y*27 + x
 *   slot_idx maps to blob chunk at blob[slot_idx * 3 .. +3]
 *
 * No malloc in hot path. No float. Sacred numbers: 27, 144, 3456, 6912.
 * ══════════════════════════════════════════════════════════════════════════════
 */

#ifndef GEO_O4_CONNECTOR_H
#define GEO_O4_CONNECTOR_H

#include <stdint.h>
#include <string.h>
#include "geo_pixel.h"
#include "geo_tring_addr.h"

/* ── constants ──────────────────────────────────────────────────── */
#define O4_CHUNK_BYTES    3u      /* bytes per GeoPixel slot           */
#define O4_GRID_W        27u      /* canonical grid width (trit space) */
#define O4_MAX_SLOTS   6912u      /* TRING_TOTAL                       */
#define O4_MAX_GRID_H   256u      /* max rows: 256×27 = 6912 slots     */
#define O4_GRID_PIXELS (O4_GRID_W * O4_MAX_GRID_H)  /* 186,624 B max  */

/* ── O4GridCtx: one tile's geometric pixel grid ─────────────────── */
typedef struct {
    GeoPixel grid[O4_MAX_GRID_H][O4_GRID_W];  /* 27×H pixel buffer    */
    uint32_t n_slots;    /* chunks written                              */
    uint32_t grid_h;     /* ceil(n_slots / 27)                          */
    uint8_t  tile_x;     /* source tile coords (for tring routing)      */
    uint8_t  tile_y;
} O4GridCtx;

/* ── O4SlotMap: maps tring addr → slot index in grid ────────────── */
typedef struct {
    uint16_t slot[O4_MAX_SLOTS];   /* tring_addr → grid slot           */
    uint8_t  used[O4_MAX_SLOTS];   /* 1 if this addr was written        */
} O4SlotMap;

/* ════════════════════════════════════════════════════════════════════
 * ENCODE: tile blob → O4GridCtx
 *
 * Slices blob into 3B chunks, encodes each as GeoPixel,
 * places into 27×N grid in tring address order.
 *
 * Parameters:
 *   blob     — raw tile blob bytes (from encode_tile)
 *   blob_sz  — byte count
 *   tx, ty   — tile position in image grid
 *   ctx      — output: filled grid ready for PNG encode
 * ════════════════════════════════════════════════════════════════════ */
static inline void o4_encode(const uint8_t *blob, uint32_t blob_sz,
                               uint8_t tx, uint8_t ty,
                               O4GridCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->tile_x = tx;
    ctx->tile_y = ty;

    /* chunk count: pad last chunk with zeros if needed */
    uint32_t n_chunks = (blob_sz + O4_CHUNK_BYTES - 1) / O4_CHUNK_BYTES;
    if (n_chunks > O4_MAX_SLOTS) n_chunks = O4_MAX_SLOTS;

    for (uint32_t i = 0; i < n_chunks; i++) {
        /* read 3-byte chunk (zero-pad at end) */
        uint32_t byte0 = (i * 3 + 0 < blob_sz) ? blob[i * 3 + 0] : 0u;
        uint32_t byte1 = (i * 3 + 1 < blob_sz) ? blob[i * 3 + 1] : 0u;
        uint32_t byte2 = (i * 3 + 2 < blob_sz) ? blob[i * 3 + 2] : 0u;

        /* derive geometric index: fold 3 bytes into slot_idx */
        /* slot_idx = i: sequential placement in 27-wide grid */
        uint32_t slot_idx = i % O4_MAX_SLOTS;

        /* encode slot_idx as GeoPixel (W=27 canonical) */
        GeoPixel px = geo_pixel_encode(slot_idx, O4_GRID_W);

        /* embed actual data bytes by XOR-ing into RGB channels
         * This preserves the geometric field structure while
         * encoding payload: decode recovers geometry + data */
        px.r ^= (uint8_t)byte0;
        px.g ^= (uint8_t)byte1;
        px.b ^= (uint8_t)byte2;

        /* place into grid: row = slot_idx/27, col = slot_idx%27 */
        uint32_t row = slot_idx / O4_GRID_W;
        uint32_t col = slot_idx % O4_GRID_W;
        ctx->grid[row][col] = px;
    }

    ctx->n_slots = n_chunks;
    ctx->grid_h  = (n_chunks + O4_GRID_W - 1) / O4_GRID_W;
    if (ctx->grid_h == 0) ctx->grid_h = 1;
}

/* ════════════════════════════════════════════════════════════════════
 * DECODE: O4GridCtx → blob bytes
 *
 * Reverses o4_encode: XOR-recovers payload bytes from grid pixels.
 *
 * Parameters:
 *   ctx      — grid from o4_encode or PNG decode
 *   out_blob — caller-allocated buffer (size ≥ n_chunks * 3)
 *   n_chunks — number of chunks to decode (stored externally)
 *   out_sz   — actual original blob size (stored externally)
 * ════════════════════════════════════════════════════════════════════ */
static inline void o4_decode(const O4GridCtx *ctx,
                              uint8_t *out_blob, uint32_t n_chunks,
                              uint32_t out_sz) {
    for (uint32_t i = 0; i < n_chunks; i++) {
        uint32_t slot_idx = i % O4_MAX_SLOTS;
        uint32_t row = slot_idx / O4_GRID_W;
        uint32_t col = slot_idx % O4_GRID_W;

        GeoPixel px = ctx->grid[row][col];

        /* recover geometry pixel for this slot (same formula as encode) */
        GeoPixel geo = geo_pixel_encode(slot_idx, O4_GRID_W);

        /* XOR back to get original data bytes */
        uint8_t b0 = px.r ^ geo.r;
        uint8_t b1 = px.g ^ geo.g;
        uint8_t b2 = px.b ^ geo.b;

        uint32_t base = i * 3;
        if (base + 0 < out_sz) out_blob[base + 0] = b0;
        if (base + 1 < out_sz) out_blob[base + 1] = b1;
        if (base + 2 < out_sz) out_blob[base + 2] = b2;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * TRING ROUTE: assign tile → TRing slot address
 *
 * Given tile (tx, ty) and channel, returns the geometric TRing address.
 * Used to route encoded tiles into the correct POGLS storage slot.
 * ════════════════════════════════════════════════════════════════════ */
static inline TringAddr o4_tile_route(uint8_t tx, uint8_t ty, uint8_t ch) {
    return tring_encode(tx, ty, ch);
}

/* ════════════════════════════════════════════════════════════════════
 * GRID → FLAT PIXEL BUFFER (for PNG encode)
 *
 * Converts O4GridCtx grid to interleaved RGB bytes (row-major).
 * Output buffer must be ≥ 27 × grid_h × 3 bytes.
 * ════════════════════════════════════════════════════════════════════ */
static inline uint32_t o4_grid_to_rgb(const O4GridCtx *ctx,
                                       uint8_t *out_rgb, uint32_t buf_sz) {
    uint32_t needed = ctx->grid_h * O4_GRID_W * 3u;
    if (buf_sz < needed) return 0;

    for (uint32_t y = 0; y < ctx->grid_h; y++) {
        for (uint32_t x = 0; x < O4_GRID_W; x++) {
            uint32_t off = (y * O4_GRID_W + x) * 3u;
            out_rgb[off + 0] = ctx->grid[y][x].r;
            out_rgb[off + 1] = ctx->grid[y][x].g;
            out_rgb[off + 2] = ctx->grid[y][x].b;
        }
    }
    return needed;
}

/* ════════════════════════════════════════════════════════════════════
 * FLAT PIXEL BUFFER → GRID (for PNG decode)
 * ════════════════════════════════════════════════════════════════════ */
static inline void o4_rgb_to_grid(const uint8_t *rgb, uint32_t grid_h,
                                   O4GridCtx *ctx) {
    ctx->grid_h = grid_h;
    for (uint32_t y = 0; y < grid_h && y < O4_MAX_GRID_H; y++) {
        for (uint32_t x = 0; x < O4_GRID_W; x++) {
            uint32_t off = (y * O4_GRID_W + x) * 3u;
            ctx->grid[y][x].r = rgb[off + 0];
            ctx->grid[y][x].g = rgb[off + 1];
            ctx->grid[y][x].b = rgb[off + 2];
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * ROUNDTRIP VERIFY
 *
 * Encodes a test blob, decodes back, checks byte-exact match.
 * Returns 0 = pass, >0 = mismatch count.
 * ════════════════════════════════════════════════════════════════════ */
static inline uint32_t o4_roundtrip_verify(void) {
    /* build test blob: 256 bytes of known pattern */
    static const uint32_t TEST_SZ = 256u;
    uint8_t orig[256], recovered[256];
    for (uint32_t i = 0; i < TEST_SZ; i++)
        orig[i] = (uint8_t)((i * 37u + 13u) % 256u);

    O4GridCtx ctx;
    o4_encode(orig, TEST_SZ, 3, 5, &ctx);  /* tx=3, ty=5 */

    uint32_t n_chunks = (TEST_SZ + 2u) / 3u;
    o4_decode(&ctx, recovered, n_chunks, TEST_SZ);

    uint32_t errs = 0;
    for (uint32_t i = 0; i < TEST_SZ; i++)
        if (orig[i] != recovered[i]) errs++;
    return errs;
}

#endif /* GEO_O4_CONNECTOR_H */
