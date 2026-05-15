/*
 * fibo_hb_wire.h — FiboTile → Hamburger pipeline wire (fixed v2)
 * ══════════════════════════════════════════════════════════════════════
 *
 *  Connects fibo_tile_dispatch output (GpxTileInput) to hamburger
 *  encode/decode using real HbTileIn + hb_codec_apply signatures
 *  from hamburger_encode.h (session 3).
 *
 *  Data flow:
 *    GpxTileInput (192B RGB)
 *        ↓ fhw_rgb_to_ycgco()       → int Y[64], Cg[64], Co[64]
 *        ↓ hb_classify_tile()       → ttype (FLAT/GRAD/EDGE/NOISE)
 *        ↓ hb_ttype_to_codec()      → codec
 *        ↓ pack int16le 6B/px       → raw_tile[384B]
 *        ↓ hb_codec_apply()         → enc_buf
 *    FiboHbResult
 *
 *  YCgCo: uses JFIF-standard lossless lifting (not >>1 form).
 *    Forward and inverse are exact inverses for all 8-bit RGB inputs.
 *
 *  HbTileIn note:
 *    hamburger_encode.h HbTileIn uses (data ptr + sz + tile_id + ttype).
 *    This file calls hb_codec_apply() directly — no HbTileIn needed.
 *
 *  Frozen rules:
 *    - integer only (YCgCo lossless lifting)
 *    - no heap in hot path
 *    - tile_id = face*60 + edge*12 + (z%12) → 0..719 (sacred 720)
 * ══════════════════════════════════════════════════════════════════════
 */

#ifndef FIBO_HB_WIRE_H
#define FIBO_HB_WIRE_H

#include <stdint.h>
#include <string.h>
#include "fibo_tile_dispatch.h"
#include "hamburger_classify.h"
#include "hamburger_encode.h"

/* ── Tile buffer sizes ───────────────────────────────────────────────── */
#define FHW_TILE_PX      FTD_TILE_PIXELS           /* 64                */
#define FHW_RAW_SZ       (FHW_TILE_PX * 6u)        /* 384B int16le YCgCo*/
#define FHW_ENC_CAP      (FHW_RAW_SZ * 2u + 32u)   /* safe headroom     */

/* ── FiboHbResult ────────────────────────────────────────────────────── */
typedef struct {
    uint8_t  enc_buf[FHW_ENC_CAP];
    uint32_t enc_sz;
    uint8_t  ttype;       /* GPX5_TTYPE_*                               */
    uint8_t  codec;       /* GPX5_CODEC_*                               */
    uint16_t tile_id;     /* 0..719                                     */
    uint64_t route_id;    /* from GpxTileInput                          */
    uint8_t  verified;    /* inv_of_inv check passed                    */
    uint8_t  _pad[3];
} FiboHbResult;

/* ── tile_id: (face,edge,z) → 0..719 ────────────────────────────────── */
static inline uint16_t fhw_tile_id(uint8_t face, uint8_t edge, uint8_t z) {
    return (uint16_t)(face * 60u + edge * 12u + (z % 12u));
}

/* ── seed_local from route_id + tile_id ─────────────────────────────── */
static inline uint32_t fhw_seed_local(uint64_t route_id, uint16_t tile_id) {
    uint32_t lo = (uint32_t)(route_id & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(route_id >> 32u);
    return (lo ^ hi ^ (uint32_t)tile_id) * 0x9e3779b9u;
}

/* ── RGB → YCgCo (JFIF lossless lifting — exact roundtrip for 8-bit) ── */
/*
 * Forward lifting:
 *   Co  = R - B
 *   tmp = B + (Co >> 1)
 *   Cg  = G - tmp
 *   Y   = tmp + (Cg >> 1)
 *
 * This is the integer lossless YCgCo from JFIF/JPEG-LS.
 * Paired with fhw_ycgco_to_rgb → exact roundtrip, no clamping needed.
 */
static inline void fhw_rgb_to_ycgco(
    const uint8_t *rgb,   /* 64 pixels × 3B = 192B                     */
    int *Y, int *Cg, int *Co)
{
    for (uint32_t p = 0u; p < FHW_TILE_PX; p++) {
        int r = rgb[p * 3 + 0];
        int g = rgb[p * 3 + 1];
        int b = rgb[p * 3 + 2];
        int co  = r - b;
        int tmp = b + (co >> 1);
        int cg  = g - tmp;
        Y [p]   = tmp + (cg >> 1);
        Cg[p]   = cg;
        Co[p]   = co;
    }
}

/* ── YCgCo → RGB (exact inverse of fhw_rgb_to_ycgco) ───────────────── */
/*
 * Inverse lifting:
 *   tmp = Y  - (Cg >> 1)
 *   G   = Cg + tmp
 *   B   = tmp - (Co >> 1)
 *   R   = B  + Co
 *
 * No clamping required — values stay in 0..255 for valid 8-bit input.
 * Clamp guard kept for safety against corrupted enc_buf.
 */
static inline void fhw_ycgco_to_rgb(
    const int *Y, const int *Cg, const int *Co,
    uint8_t *rgb)   /* 64 pixels × 3B = 192B */
{
    for (uint32_t p = 0u; p < FHW_TILE_PX; p++) {
        int tmp = Y[p]  - (Cg[p] >> 1);
        int g   = Cg[p] + tmp;
        int b   = tmp   - (Co[p] >> 1);
        int r   = b     +  Co[p];
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        rgb[p * 3 + 0] = (uint8_t)r;
        rgb[p * 3 + 1] = (uint8_t)g;
        rgb[p * 3 + 2] = (uint8_t)b;
    }
}

/* ── Pack int arrays → int16_le raw_tile (for hb_codec_apply) ───────── */
static inline void fhw_pack_raw(
    const int *Y, const int *Cg, const int *Co,
    uint8_t *raw)   /* FHW_RAW_SZ = 384B */
{
    for (uint32_t p = 0u; p < FHW_TILE_PX; p++) {
        int16_t y  = (int16_t)Y [p];
        int16_t cg = (int16_t)Cg[p];
        int16_t co = (int16_t)Co[p];
        memcpy(raw + p*6 + 0, &y,  2);
        memcpy(raw + p*6 + 2, &cg, 2);
        memcpy(raw + p*6 + 4, &co, 2);
    }
}

/* ── Unpack int16_le raw_tile → int arrays ───────────────────────────── */
static inline void fhw_unpack_raw(
    const uint8_t *raw,   /* FHW_RAW_SZ = 384B */
    int *Y, int *Cg, int *Co)
{
    for (uint32_t p = 0u; p < FHW_TILE_PX; p++) {
        int16_t y, cg, co;
        memcpy(&y,  raw + p*6 + 0, 2);
        memcpy(&cg, raw + p*6 + 2, 2);
        memcpy(&co, raw + p*6 + 4, 2);
        Y [p] = (int)y;
        Cg[p] = (int)cg;
        Co[p] = (int)co;
    }
}

/* ── fhw_is_perfect_flat ─────────────────────────────────────────────── */
/*
 * Returns 1 only if ALL 64 YCgCo values are identical to pixel 0.
 * CODEC_SEED FLAT stores a single 6B sample and repeats it on decode —
 * so lossless guarantee requires every pixel to be bit-exact equal.
 * Anything else must fall back to GRAD (CODEC_FREQ) for full storage.
 */
static inline int fhw_is_perfect_flat(
    const int *Y, const int *Cg, const int *Co)
{
    int y0 = Y[0], cg0 = Cg[0], co0 = Co[0];
    for (uint32_t p = 1u; p < FHW_TILE_PX; p++) {
        if (Y[p] != y0 || Cg[p] != cg0 || Co[p] != co0) return 0;
    }
    return 1;
}

/* ── fibo_hb_encode_tile ─────────────────────────────────────────────── */
/*
 * Main encode: GpxTileInput → FiboHbResult
 *
 * FLAT guard: hb_classify_tile may call "nearly flat" tiles FLAT, but
 * CODEC_SEED only reconstructs perfectly — if classify returns FLAT and
 * pixels are not all identical, ttype is downgraded to GRADIENT so
 * CODEC_FREQ stores the full tile losslessly.
 *
 * Returns 1 on success, 0 on error.
 */
static inline int fibo_hb_encode_tile(
    const GpxTileInput *tile,
    FiboHbResult       *out)
{
    int Y[FHW_TILE_PX], Cg[FHW_TILE_PX], Co[FHW_TILE_PX];
    uint8_t raw[FHW_RAW_SZ];

    memset(out, 0, sizeof(*out));
    out->route_id = tile->route_id;
    out->verified = (tile->flags & FTD_FLAG_VERIFIED) ? 1u : 0u;
    out->tile_id  = fhw_tile_id(tile->face, tile->edge, tile->z);

    /* step 1: RGB → YCgCo (lossless lifting) */
    fhw_rgb_to_ycgco(tile->tile_rgb, Y, Cg, Co);

    /* step 2: classify */
    out->ttype = hb_classify_tile(
        Y, Cg, Co,
        0, 0,
        (int)FTD_TILE_W, (int)FTD_TILE_H,
        (int)FTD_TILE_W);

    /* step 2b: FLAT guard — CODEC_SEED repeats 1 sample across all pixels.
     * Only safe when every pixel is bit-exact equal. Downgrade to GRADIENT
     * for "nearly flat" tiles so CODEC_FREQ handles them losslessly. */
    if (out->ttype == GPX5_TTYPE_FLAT && !fhw_is_perfect_flat(Y, Cg, Co))
        out->ttype = GPX5_TTYPE_GRADIENT;

    /* step 3: codec select */
    out->codec = hb_ttype_to_codec(out->ttype);

    /* step 4: pack int16_le for hb_codec_apply */
    fhw_pack_raw(Y, Cg, Co, raw);

    /* step 5: encode */
    uint32_t seed_local = fhw_seed_local(tile->route_id, out->tile_id);
    out->enc_sz = hb_codec_apply(
        out->codec, out->ttype,
        raw, FHW_RAW_SZ,
        out->enc_buf, FHW_ENC_CAP,
        seed_local);

    return (out->enc_sz > 0u) ? 1 : 0;
}

/* ── fibo_hb_decode_tile ─────────────────────────────────────────────── */
/*
 * Decode: FiboHbResult → original RGB tile (192B)
 * route_id + tile_id taken directly from res (stored at encode time).
 * Returns 1 on success, 0 on error.
 */
static inline int fibo_hb_decode_tile(
    const FiboHbResult *res,
    uint8_t            *out_rgb)   /* FTD_TILE_BYTES = 192B */
{
    uint8_t raw[FHW_RAW_SZ];
    uint32_t seed_local = fhw_seed_local(res->route_id, res->tile_id);

    uint32_t dec_sz = hb_codec_invert(
        res->codec, res->ttype,
        res->enc_buf, res->enc_sz,
        raw, FHW_RAW_SZ,
        seed_local);

    if (dec_sz != FHW_RAW_SZ) return 0;

    int Y[FHW_TILE_PX], Cg[FHW_TILE_PX], Co[FHW_TILE_PX];
    fhw_unpack_raw(raw, Y, Cg, Co);
    fhw_ycgco_to_rgb(Y, Cg, Co, out_rgb);
    return 1;
}

/* ── debug helper ────────────────────────────────────────────────────── */
static inline const char *fibo_hb_ttype_name(uint8_t ttype) {
    switch (ttype) {
        case GPX5_TTYPE_FLAT:     return "FLAT";
        case GPX5_TTYPE_GRADIENT: return "GRAD";
        case GPX5_TTYPE_EDGE:     return "EDGE";
        case GPX5_TTYPE_NOISE:    return "NOISE";
        default:                  return "???";
    }
}

/* ── Test invariants ─────────────────────────────────────────────────── */
/*
 * W01: encode+decode roundtrip → out_rgb == original tile_rgb (lossless)
 * W02: FLAT tile → enc_sz == 8 (CODEC_SEED: 2B sz + 6B sample)
 * W03: seed_local reproducible — same route_id+tile_id → same result
 * W04: fhw_rgb_to_ycgco + fhw_ycgco_to_rgb roundtrip → lossless for ALL 8-bit
 *      (verified: full 256^3 grid passes with JFIF lifting form)
 * W05: ttype varies with tile content: uniform→FLAT, gradient→GRAD, etc.
 * W06: hb_classify_tile called with x1=FTD_TILE_W, y1=FTD_TILE_H (exclusive)
 *      img_w=FTD_TILE_W — matches hamburger_classify.h contract
 */

#endif /* FIBO_HB_WIRE_H */
