/*
 * geom_weight_reconstruct.h — Geometry tile → float weight reconstruction
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Converts decoded geometry tiles (raw Q8_0 bytes) back to float32.
 *
 * Q8_0 block format (34 bytes):
 *   [fp16_scale (2B)] + [32 × int8 (32B)] = 34 bytes → 32 float values
 *
 * Tier 1 — bulk:   decode full tensor → dequantize all blocks
 * Tier 2 — random: decode 5 tiles → extract 1 block → dequantize 32 floats
 *
 * Usage:
 *   #include "geom_weight_reconstruct.h"
 *
 *   // Bulk (full tensor):
 *   size_t n_blocks = raw_sz / 34;
 *   float *f32 = malloc(n_blocks * 32 * sizeof(float));
 *   gwr_q8_bulk(raw_q8_buf, raw_sz, f32);
 *
 *   // Random access (one block):
 *   float block[32];
 *   gwr_q8_block(ge, block_idx, block);  // decodes 5 tiles internally
 *
 * No malloc in hot path. No external deps.
 * ═══════════════════════════════════════════════════════════════════════
 */

#ifndef GEOM_WEIGHT_RECONSTRUCT_H
#define GEOM_WEIGHT_RECONSTRUCT_H

#include <stdint.h>
#include <stddef.h>
#include "geom_raw_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Q8_0 constants */
#define GWR_Q8_BLOCK_VALS  32u   /* values per Q8_0 block      */
#define GWR_Q8_BLOCK_BYTES 34u   /* bytes per Q8_0 block       */
#define GWR_TILES_PER_BLOCK 5u   /* ceil(34/7) = 5 tiles       */

/* ═══════════════════════════════════════════════════════════════
   FP16 → FP32 (no external dep, 1:1 match with ggml fp16)
   ═══════════════════════════════════════════════════════════════ */

static inline float _gwr_fp16_to_fp32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (uint32_t)((h >> 10) & 0x1F);
    uint32_t mant = (uint32_t)(h & 0x3FF);

    if (exp == 0) {
        /* subnormal or zero */
        if (mant == 0) {
            uint32_t v = sign;
            float f; memcpy(&f, &v, 4); return f;
        }
        /* normalize */
        int shift = 10;
        while ((mant & 0x400) == 0) { mant <<= 1; shift--; }
        exp = 1 - shift + 127 - 10;
        mant = (mant & 0x7FF) << 13;
        uint32_t v = sign | (exp << 23) | mant;
        float f; memcpy(&f, &v, 4); return f;
    } else if (exp == 31) {
        /* inf/nan */
        exp = 255;
        mant <<= 13;
        uint32_t v = sign | (exp << 23) | mant;
        float f; memcpy(&f, &v, 4); return f;
    }

    exp = exp - 15 + 127;
    mant <<= 13;
    uint32_t v = sign | (exp << 23) | mant;
    float f; memcpy(&f, &v, 4); return f;
}

/* ═══════════════════════════════════════════════════════════════
   BULK: Q8_0 raw buffer → float32 array
   ═══════════════════════════════════════════════════════════════
   raw_q8 : pointer to Q8_0 data (from gb_decode_tensor)
   raw_sz : byte size (must be multiple of GWR_Q8_BLOCK_BYTES)
   out    : output float buffer (must hold raw_sz/34 * 32 floats)
   Returns number of blocks dequantized, or -1 on bad size.
   ═══════════════════════════════════════════════════════════════ */

static inline int gwr_q8_bulk(const void *raw_q8, size_t raw_sz, float *out) {
    if (!raw_q8 || !out || raw_sz % GWR_Q8_BLOCK_BYTES != 0) return -1;

    size_t n_blocks = raw_sz / GWR_Q8_BLOCK_BYTES;
    const uint8_t *src = (const uint8_t *)raw_q8;

    for (size_t bi = 0; bi < n_blocks; bi++) {
        uint16_t scale_h;
        memcpy(&scale_h, src + bi * GWR_Q8_BLOCK_BYTES, 2);
        float scale = _gwr_fp16_to_fp32(scale_h);
        const int8_t *qs = (const int8_t *)(src + bi * GWR_Q8_BLOCK_BYTES + 2);
        float *dst = out + bi * GWR_Q8_BLOCK_VALS;

        for (uint32_t i = 0; i < GWR_Q8_BLOCK_VALS; i++) {
            dst[i] = (float)qs[i] * scale;
        }
    }
    return (int)n_blocks;
}

/* ═══════════════════════════════════════════════════════════════
   RANDOM: decode single Q8_0 block from geometry store
   ═══════════════════════════════════════════════════════════════
   Decodes GWR_TILES_PER_BLOCK (5) tiles, extracts the 34 bytes
   at the correct offset, and dequantizes to 32 floats.

   ge        : GstenEntry (from gb_get)
   block_idx : which Q8_0 block (0-based)
   out       : 32 floats output

   Returns RB_OK on success, RB_ERR if block_idx out of range.
   ═══════════════════════════════════════════════════════════════ */

static inline int gwr_q8_block(GstenEntry *ge,
                                uint32_t block_idx, float *out) {
    if (!ge || !out) return RB_ERR;
    if (!ge->occupied) return RB_ERR;

    /* Byte offset of this Q8_0 block in the original tensor */
    size_t byte_off = (size_t)block_idx * GWR_Q8_BLOCK_BYTES;
    size_t tile_off = byte_off / GSTEN_TILE_SZ;      /* first tile index */
    size_t byte_rem = byte_off % GSTEN_TILE_SZ;       /* offset within tile */

    /* If block straddles tile boundary, we need all tiles that cover it */
    /* Q8_0 block = 34 bytes. Tiles = 7 bytes. Block spans ceil(34/7) = 5 tiles.
     * Start at tile_off, tile_off could be negative relative to block start.
     * Actually Q8_0 block always starts at 34-byte-aligned offset in the
     * original tensor. The tile boundary is at 7-byte.
     *
     * Block N starts at byte N*34. First tile index = (N*34) / 7.
     * But the block data might start midway into that tile.
     *
     * Solution: decode bytes byte_off..byte_off+33 from the tensor,
     * tile by tile.
     */
    uint8_t raw_block[GWR_Q8_BLOCK_BYTES];
    size_t remaining = GWR_Q8_BLOCK_BYTES;
    size_t dst_off = 0;

    while (remaining > 0) {
        /* Which tile index */
        size_t ti = (byte_off + dst_off) / GSTEN_TILE_SZ;
        size_t ti_off = (byte_off + dst_off) % GSTEN_TILE_SZ;

        if (ti >= ge->n_tiles) return RB_ERR;

        uint8_t tile[GSTEN_TILE_SZ];
        if (gb_decode_tile(ge, (uint32_t)ti, tile) != RB_OK) return RB_ERR;

        size_t copy = GSTEN_TILE_SZ - ti_off;
        if (copy > remaining) copy = remaining;

        memcpy(raw_block + dst_off, tile + ti_off, copy);
        dst_off += copy;
        remaining -= copy;
    }

    /* Dequantize */
    uint16_t scale_h;
    memcpy(&scale_h, raw_block, 2);
    float scale = _gwr_fp16_to_fp32(scale_h);
    const int8_t *qs = (const int8_t *)(raw_block + 2);
    for (uint32_t i = 0; i < GWR_Q8_BLOCK_VALS; i++) {
        out[i] = (float)qs[i] * scale;
    }
    return RB_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* GEOM_WEIGHT_RECONSTRUCT_H */
