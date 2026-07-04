/*
 * bond_to_geopixel.h — Bond Layer ↔ GeoPixel Bridge
 * ════════════════════════════════════════════════════════
 * Encodes bond data (PoglsPiece) into GeoPixel RGB.
 * Each pixel encodes one byte via geo_pixel_encode(byte, 256),
 * which is losslessly recoverable from (trit, fibo) CRT pair.
 *
 * Visual fingerprint: one pixel = unique RGB per piece.
 * Full state stripe: 27 pixels encode entire 25B piece + computed bond_key.
 * Address card: geo_key → 27×27 GeoPixel grid.
 *
 * No malloc, no float, O(1).
 * ════════════════════════════════════════════════════════
 */

#ifndef BOND_TO_GEOPIXEL_H
#define BOND_TO_GEOPIXEL_H

#include <stdint.h>
#include <string.h>
#include "pogls_bond.h"

/* ── GeoPixel include path: adjust if your tree differs ───────── */
#include "geopixel/geo_pixel.h"

/* ── Sacred constants ─────────────────────────────────────────── */
#define BGP_STRIPE_W     27u    /* stripe width = GP_GRID_W          */
#define BGP_STRIPE_BYTES 25u    /* bytes in a PoglsPiece             */
#define BGP_PAD_PIXELS   (BGP_STRIPE_W - BGP_STRIPE_BYTES)    /* 2 */
#define BGP_STRIPE_V2_PX   9u   /* V2: 9p x 3B = 27B (25 piece + 2 bond_key) */
#define BGP_STRIPE_V2_BUF 27u   /* V2: packed buffer size            */

/* Deterministic slot geometry reused by V2/V3 visual paths */
static inline GeoPixel _gp_slot_geo(uint32_t slot_idx) {
    return geo_pixel_encode(slot_idx, 27u);
}

/* ── CRT byte recovery from (trit, fibo) ─────────────────────────
 * Given x%27 and x%144, find x in [0,255].
 * CRT with moduli 27=3³, 144=2⁴·3², gcd=9.
 * Solution exists iff trit%9 == fibo%9.
 * One solution modulo 432 = lcm(27,144).
 * Linear search over hops = 27 is fine for 256 range.
 */
static inline uint8_t _bgp_crt_byte(uint8_t trit, uint8_t fibo) {
    for (uint16_t x = trit; x < 256u; x += 27u) {
        if (x % 144u == fibo) return (uint8_t)x;
    }
    return trit; /* fallback (should not happen for valid pairs) */
}

/* ════════════════════════════════════════════════════════════════
   PIECE → SINGLE FINGERPRINT PIXEL
   Maps a PoglsPiece to a unique GeoPixel for visual identification.
   Uses direct field encoding (not via geo_pixel_encode) for full
   information density.
   ════════════════════════════════════════════════════════════════ */
static inline GeoPixel bond_piece_fingerprint(const PoglsPiece *p) {
    uint64_t gk = p->geo_key;
    uint64_t bk = pogls_bond_key(p);
    GeoPixel px;
    /* R: shape (5 bits) | geo_key bits 0-2 (3 bits) */
    uint8_t shape_idx = (p->shape >= 'A') ? (uint8_t)(p->shape - 'A') : 0u;
    px.r = (uint8_t)((shape_idx << 3) | (gk & 0x7u));
    /* G: coset from geo_key nibble | letter from bond_key nibble */
    px.g = (uint8_t)((((gk >> 3) & 0xFu) << 4) | (bk & 0xFu));
    /* B: fibo mix of geo_key and bond_key */
    px.b = (uint8_t)(((gk >> 16) ^ (bk >> 8)) % 144u);
    return px;
}

/* ════════════════════════════════════════════════════════════════
   PIECE → 27-WIDE PIXEL STRIPE (lossless)
   Encodes the entire 25B PoglsPiece + 2 extra bytes from bond_key
   into a 27-pixel stripe. Each pixel uses geo_pixel_encode(byte, 256)
   which is losslessly recoverable via CRT.
   ════════════════════════════════════════════════════════════════ */
static inline void bond_piece_to_stripe(const PoglsPiece *p,
                                         GeoPixel stripe[BGP_STRIPE_W])
{
    /* Pack 25 bytes: geo_key(8) + shape(1) + bond_L(8) + bond_R(8) */
    uint8_t buf[BGP_STRIPE_BYTES];
    /* geo_key: 8 bytes LE */
    for (int i = 0; i < 8; i++)
        buf[i]     = (uint8_t)(p->geo_key >> (i * 8u));
    /* shape */
    buf[8] = p->shape;
    /* bond_L: 8 bytes LE */
    for (int i = 0; i < 8; i++)
        buf[9 + i] = (uint8_t)(p->bond_L >> (i * 8u));
    /* bond_R: 8 bytes LE */
    for (int i = 0; i < 8; i++)
        buf[17 + i]= (uint8_t)(p->bond_R >> (i * 8u));

    /* stripe: pixel[i] = geo_pixel_encode(buf[i] (or derived), 256) */
    for (uint32_t i = 0; i < BGP_STRIPE_BYTES; i++)
        stripe[i] = geo_pixel_encode(buf[i], 256u);

    /* pixel 25: bond_key low byte */
    uint64_t bk = pogls_bond_key(p);
    stripe[25] = geo_pixel_encode((uint8_t)(bk & 0xFFu), 256u);

    /* pixel 26: bond_key middle byte */
    stripe[26] = geo_pixel_encode((uint8_t)((bk >> 8) & 0xFFu), 256u);
}

/* ════════════════════════════════════════════════════════════════
   27-WIDE PIXEL STRIPE → PIECE (decode)
   Losslessly recovers PoglsPiece from a 27-pixel stripe.
   Returns 0 on success, -1 if any pixel is corrupt.
   ════════════════════════════════════════════════════════════════ */
static inline int bond_stripe_to_piece(const GeoPixel stripe[BGP_STRIPE_W],
                                        PoglsPiece *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    uint8_t buf[BGP_STRIPE_BYTES];

    /* Decode each pixel via CRT */
    for (uint32_t i = 0; i < BGP_STRIPE_BYTES; i++) {
        GeoFields f = geo_pixel_decode(stripe[i]);
        buf[i] = _bgp_crt_byte(f.trit, f.fibo);
    }

    /* Unpack */
    out->geo_key = 0;
    for (int i = 0; i < 8; i++)
        out->geo_key |= (uint64_t)buf[i] << (i * 8u);

    out->shape = buf[8];

    out->bond_L = 0;
    for (int i = 0; i < 8; i++)
        out->bond_L |= (uint64_t)buf[9 + i] << (i * 8u);

    out->bond_R = 0;
    for (int i = 0; i < 8; i++)
        out->bond_R |= (uint64_t)buf[17 + i] << (i * 8u);

    /* Verify bond_key integrity */
    uint64_t expected_bk = pogls_bond_key(out);
    GeoFields f25 = geo_pixel_decode(stripe[25]);
    GeoFields f26 = geo_pixel_decode(stripe[26]);
    uint8_t bk_lo = _bgp_crt_byte(f25.trit, f25.fibo);
    uint8_t bk_hi = _bgp_crt_byte(f26.trit, f26.fibo);
    uint16_t stored_bk = (uint16_t)((uint16_t)bk_hi << 8) | bk_lo;

    /* Check low 16 bits match — return -1 on mismatch */
    if (((uint16_t)(expected_bk & 0xFFFFu)) != stored_bk) return -1;

    return 0;
}

/* ════════════════════════════════════════════════════════════════
   V2 (O4-style): PIECE → 9-PIXEL STRIPE (lossless, 1.08×)
   Packs 25B piece + 2B bond_key into 9 pixels via O4-style XOR
   with slot geometry. 3 bytes per pixel → 27 bytes for 25 bytes.
   Uses geo_pixel_encode(slot_idx, 27) as geometry base, then
   XORs data bytes into RGB — preserves geometric visual pattern
   while achieving ~1× storage ratio.
   ════════════════════════════════════════════════════════════════ */
static inline void bond_piece_to_stripe_v2(const PoglsPiece *p,
                                            GeoPixel stripe[BGP_STRIPE_V2_PX])
{
    uint8_t buf[BGP_STRIPE_V2_BUF];
    for (int i = 0; i < 8; i++)
        buf[i]     = (uint8_t)(p->geo_key >> (i * 8u));
    buf[8] = p->shape;
    for (int i = 0; i < 8; i++)
        buf[9 + i] = (uint8_t)(p->bond_L >> (i * 8u));
    for (int i = 0; i < 8; i++)
        buf[17 + i]= (uint8_t)(p->bond_R >> (i * 8u));
    uint64_t bk = pogls_bond_key(p);
    buf[25] = (uint8_t)(bk & 0xFFu);
    buf[26] = (uint8_t)((bk >> 8) & 0xFFu);

    for (uint32_t i = 0; i < BGP_STRIPE_V2_PX; i++) {
        GeoPixel geo = _gp_slot_geo(i);
        stripe[i].r = geo.r ^ buf[i * 3 + 0];
        stripe[i].g = geo.g ^ buf[i * 3 + 1];
        stripe[i].b = geo.b ^ buf[i * 3 + 2];
    }
}

static inline int bond_stripe_to_piece_v2(const GeoPixel stripe[BGP_STRIPE_V2_PX],
                                           PoglsPiece *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    uint8_t buf[BGP_STRIPE_V2_BUF];
    for (uint32_t i = 0; i < BGP_STRIPE_V2_PX; i++) {
        GeoPixel geo = geo_pixel_encode(i, 27u);
        buf[i * 3 + 0] = stripe[i].r ^ geo.r;
        buf[i * 3 + 1] = stripe[i].g ^ geo.g;
        buf[i * 3 + 2] = stripe[i].b ^ geo.b;
    }

    out->geo_key = 0;
    for (int i = 0; i < 8; i++)
        out->geo_key |= (uint64_t)buf[i] << (i * 8u);
    out->shape = buf[8];
    out->bond_L = 0;
    for (int i = 0; i < 8; i++)
        out->bond_L |= (uint64_t)buf[9 + i] << (i * 8u);
    out->bond_R = 0;
    for (int i = 0; i < 8; i++)
        out->bond_R |= (uint64_t)buf[17 + i] << (i * 8u);

    uint64_t expected_bk = pogls_bond_key(out);
    uint16_t stored_bk = (uint16_t)((uint16_t)buf[26] << 8) | buf[25];
    if (((uint16_t)(expected_bk & 0xFFFFu)) != stored_bk) return -1;
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   ADDRESS → 27×27 GEOPIXEL GRID (visual card)
   Maps a 64-bit address to a 27×27 GeoPixel grid.
   Each cell uses geo_pixel_encode(derived_idx, 27) for canonical
   GeoPixel encoding. This creates a visually distinctive pattern
   that is deterministic per address.
   ════════════════════════════════════════════════════════════════ */
static inline void bond_addr_to_grid(uint64_t addr,
                                     GeoPixel grid[GP_GRID_W * GP_GRID_W])
{
    for (uint32_t y = 0; y < GP_GRID_W; y++) {
        for (uint32_t x = 0; x < GP_GRID_W; x++) {
            /* Mix address into the pixel index for visual diversity */
            uint64_t mix = addr ^ ((uint64_t)y * GP_GRID_W + x);
            uint32_t idx = (uint32_t)((mix ^ (mix >> 16)) % 65536u);
            grid[y * GP_GRID_W + x] = geo_pixel_encode(idx, 27u);
        }
    }
}

/* ════════════════════════════════════════════════════════════════
   PIECE → 27×27 PIXEL GRID (bond card)
   Uses the piece's geo_key as the seed for a grid, then overlays
   shape and bond information as edge pixels for visual context.
   ════════════════════════════════════════════════════════════════ */
static inline void bond_piece_to_grid(const PoglsPiece *p,
                                      GeoPixel grid[GP_GRID_W * GP_GRID_W])
{
    /* Core pattern from geo_key */
    bond_addr_to_grid(p->geo_key, grid);

    /* Overlay shape as row 0 (all pixels = shape fingerprint) */
    uint64_t bk = pogls_bond_key(p);
    GeoPixel shape_px = bond_piece_fingerprint(p);
    for (uint32_t x = 0; x < GP_GRID_W; x++)
        grid[x] = shape_px;

    /* Overlay bond_key as column 0 (gradient from bond_key) */
    for (uint32_t y = 0; y < GP_GRID_W; y++) {
        uint32_t mix = (uint32_t)((bk >> (y % 8u) * 8u) & 0xFFu);
        grid[y * GP_GRID_W] = geo_pixel_encode(mix, 27u);
    }

    /* Corner = XOR of geo_key and bond_key */
    grid[0] = geo_pixel_encode((uint32_t)((p->geo_key ^ bk) % 65536u), 27u);
}

/* ════════════════════════════════════════════════════════════════
   V3 (Frame-Predictive): Compress/Decompress (0.36× ratio)
   ════════════════════════════════════════════════════════════════
   *
   * PoglsPiece is fully deterministic from (geo_key, fold_axis):
   *   geo_key          = stored directly
   *   shape            = POGLS_AXIS_SHAPE[fold_axis]
   *   bond_L           = fibo_addr(geo_key ^ SALT_L)
   *   bond_R           = fibo_addr(geo_key ^ SALT_R)
   *   bond_key         = bond_L ^ bond_R
   *
   * Storage: [geo_key(8B) + fold_axis(1B)] = 9B for 25B = 0.36×
   * RGB/BMP = NEVER STORED — generated on-demand via visualize()
   *
   * This is the fundamental insight: geometry is deterministic,
   * so only the routing address (geo_key + fold_axis) needs storage.
   * The "image" is a deterministic projection, not real data.
   * ════════════════════════════════════════════════════════════════ */

#define GPV3_SEED_BYTES  9u     /* geo_key(8) + fold_axis(1) */
#define GPV3_VIS_PX      9u     /* 9 GeoPixel for visualization */

/* Reverse lookup: shape → fold_axis (0..7) */
static inline uint8_t _gpv3_shape_to_axis(uint8_t shape) {
    for (uint8_t a = 0; a < 8; a++)
        if (POGLS_AXIS_SHAPE[a] == shape) return a;
    return 1; /* default to axis 1 (I) */
}

/* Compress: 25B PoglsPiece → 9B [geo_key LE + fold_axis] */
static inline uint32_t bond_piece_compress_v3(const PoglsPiece *p,
                                                uint8_t out[GPV3_SEED_BYTES]) {
    for (int i = 0; i < 8; i++)
        out[i] = (uint8_t)(p->geo_key >> (i * 8u));
    out[8] = _gpv3_shape_to_axis(p->shape);
    return GPV3_SEED_BYTES;
}

/* Decompress: 9B [geo_key LE + fold_axis] → PoglsPiece
 * NOTE: does NOT call pogls_make_piece() because that would
 * double-hash geo_key through fibo_addr(). Instead we manually
 * reconstruct fields — matching pogls_make_piece logic but
 * preserving geo_key as-is (it IS the geo_key, not origin_seed). */
static inline PoglsPiece bond_piece_decompress_v3(const uint8_t seed[GPV3_SEED_BYTES]) {
    uint64_t geo_key = 0;
    for (int i = 0; i < 8; i++)
        geo_key |= (uint64_t)seed[i] << (i * 8u);
    uint8_t axis = seed[8];
    PoglsPiece p;
    p.geo_key = geo_key;
    p.shape   = (axis < 8) ? POGLS_AXIS_SHAPE[axis] : SHAPE_I;
    p.bond_L  = pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_L);
    p.bond_R  = pogls_fibo_addr(geo_key ^ POGLS_BOND_SALT_R);
    return p;
}

/* Visualize: PoglsPiece → 9 GeoPixel RGB (O4-style, on-demand, never stored) */
static inline void bond_piece_visualize_v3(const PoglsPiece *p,
                                            GeoPixel stripe[GPV3_VIS_PX]) {
    uint8_t buf[27];
    for (int i = 0; i < 8; i++)
        buf[i]     = (uint8_t)(p->geo_key >> (i * 8u));
    buf[8] = p->shape;
    for (int i = 0; i < 8; i++)
        buf[9 + i] = (uint8_t)(p->bond_L >> (i * 8u));
    for (int i = 0; i < 8; i++)
        buf[17 + i]= (uint8_t)(p->bond_R >> (i * 8u));
    uint64_t bk = pogls_bond_key(p);
    buf[25] = (uint8_t)(bk & 0xFFu);
    buf[26] = (uint8_t)((bk >> 8) & 0xFFu);

    for (uint32_t i = 0; i < GPV3_VIS_PX; i++) {
        GeoPixel geo = _gp_slot_geo(i);
        stripe[i].r = geo.r ^ buf[i * 3 + 0];
        stripe[i].g = geo.g ^ buf[i * 3 + 1];
        stripe[i].b = geo.b ^ buf[i * 3 + 2];
    }
}

/* V3 roundtrip: compress → decompress → compare */
static inline int bond_geopixel_roundtrip_v3(const PoglsPiece *original) {
    uint8_t seed[GPV3_SEED_BYTES];
    bond_piece_compress_v3(original, seed);
    PoglsPiece decoded = bond_piece_decompress_v3(seed);
    int errors = 0;
    if (decoded.geo_key != original->geo_key) errors++;
    if (decoded.shape   != original->shape)    errors++;
    if (decoded.bond_L  != original->bond_L)   errors++;
    if (decoded.bond_R  != original->bond_R)   errors++;
    return errors;
}

/* ════════════════════════════════════════════════════════════════
   PIECE → RGB888 (24-bit display color)
   Packs a single fingerprint pixel into a 24-bit RGB value
   suitable for display or storage.
   ════════════════════════════════════════════════════════════════ */
static inline uint32_t bond_pixel_to_rgb888(GeoPixel p) {
    return ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
}

/* ════════════════════════════════════════════════════════════════
   ROUNDTRIP VERIFY (V1 — 27px stripe, 3× ratio)
   Encode a piece → stripe → decode → compare.
   Returns 0 on success, mismatch count on failure.
   ════════════════════════════════════════════════════════════════ */
static inline int bond_geopixel_roundtrip(const PoglsPiece *original) {
    GeoPixel stripe[BGP_STRIPE_W];
    bond_piece_to_stripe(original, stripe);

    PoglsPiece decoded;
    bond_stripe_to_piece(stripe, &decoded);

    int errors = 0;
    if (decoded.geo_key != original->geo_key) errors++;
    if (decoded.shape   != original->shape)    errors++;
    if (decoded.bond_L  != original->bond_L)   errors++;
    if (decoded.bond_R  != original->bond_R)   errors++;
    return errors;
}

/* ════════════════════════════════════════════════════════════════
   ROUNDTRIP VERIFY (V2 — 9px stripe, 1.08× ratio)
   ════════════════════════════════════════════════════════════════ */
static inline int bond_geopixel_roundtrip_v2(const PoglsPiece *original) {
    GeoPixel stripe[BGP_STRIPE_V2_PX];
    bond_piece_to_stripe_v2(original, stripe);

    PoglsPiece decoded;
    bond_stripe_to_piece_v2(stripe, &decoded);

    int errors = 0;
    if (decoded.geo_key != original->geo_key) errors++;
    if (decoded.shape   != original->shape)    errors++;
    if (decoded.bond_L  != original->bond_L)   errors++;
    if (decoded.bond_R  != original->bond_R)   errors++;
    return errors;
}

#endif /* BOND_TO_GEOPIXEL_H */
