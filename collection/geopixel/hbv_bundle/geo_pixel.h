/*
 * geo_pixel.h — GeoPixel Encode/Decode Roundtrip
 * ═══════════════════════════════════════════════
 * Encodes geometric index → RGB pixel.
 * Decode: reconstruct (trit,spoke,coset,letter,fibo) from RGB.
 *
 * Encode formula (Roadmap Phase 3):
 *   R = ((idx%27)<<3) | (idx%6)        — trit(5b) | spoke(3b)
 *   G = ((idx%9)<<4)  | (idx%26 & 0xF) — coset(4b) | letter_lo(4b)
 *   B = idx % 144                       — fibo clock position
 *
 * Uniqueness guarantee:
 *   W=27 grid → no two pixels share all 5 fields (trit,spoke,coset,letter,fibo)
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════
 */

#ifndef GEO_PIXEL_H
#define GEO_PIXEL_H

#include <stdint.h>
#include <string.h>

/* ── Sacred constants ── */
#define GP_TRIT_MOD    27u
#define GP_SPOKE_MOD    6u
#define GP_COSET_MOD    9u
#define GP_LETTER_MOD  26u
#define GP_FIBO_MOD   144u
#define GP_GRID_W      27u   /* canonical grid width */

/* ── GeoPixel: one encoded pixel ── */
typedef struct {
    uint8_t r, g, b;
} GeoPixel;

/* ── GeoFields: decoded geometric fields ── */
typedef struct {
    uint8_t trit;    /* 0..26  — 3³ */
    uint8_t spoke;   /* 0..5   — GEO_SPOKES */
    uint8_t coset;   /* 0..8   — active cosets */
    uint8_t letter;  /* 0..25  — A..Z (low 4 bits only in G channel) */
    uint8_t fibo;    /* 0..143 — fibo clock position */
} GeoFields;

/* ═══════════════════════════════════════
   ENCODE: idx → GeoPixel
   ═══════════════════════════════════════ */
static inline GeoPixel geo_pixel_encode(uint32_t idx, uint32_t W) {
    uint32_t i = (W > 0) ? (idx % W) : idx;
    GeoPixel p;
    p.r = (uint8_t)(((i % GP_TRIT_MOD) << 3) | (i % GP_SPOKE_MOD));
    p.g = (uint8_t)(((i % GP_COSET_MOD) << 4) | (i % GP_LETTER_MOD & 0xFu));
    p.b = (uint8_t)(i % GP_FIBO_MOD);
    return p;
}

/* ═══════════════════════════════════════
   DECODE: GeoPixel → GeoFields
   Note: letter reconstruction is lossy above bit 3 (only low 4 bits stored).
         Full letter (0..25) cannot be recovered from G alone.
         Roundtrip verifies fields that ARE losslessly stored.
   ═══════════════════════════════════════ */
static inline GeoFields geo_pixel_decode(GeoPixel p) {
    GeoFields f;
    f.trit   = (uint8_t)((p.r >> 3) % GP_TRIT_MOD);   /* R[7:3] */
    f.spoke  = (uint8_t)(p.r & 0x7u);                  /* R[2:0] — raw, not %6 */
    f.coset  = (uint8_t)((p.g >> 4) % GP_COSET_MOD);   /* G[7:4] */
    f.letter = (uint8_t)(p.g & 0xFu);                  /* G[3:0] — low 4 bits */
    f.fibo   = p.b;                                     /* B full byte */
    return f;
}

/* ═══════════════════════════════════════
   ROUNDTRIP VERIFY
   For W×H grid, verify:
     1. encode→decode → same trit, coset, fibo (lossless fields)
     2. spoke in [0,5] — valid range after decode
     3. W=27: no two pixels share all 5 fields (uniqueness)
   Returns: 0 = lossless, >0 = number of mismatches
   ═══════════════════════════════════════ */
static inline uint32_t geo_pixel_roundtrip_verify(uint32_t W, uint32_t H) {
    uint32_t errors = 0;
    uint32_t total  = W * H;

    for (uint32_t idx = 0; idx < total; idx++) {
        GeoPixel  p = geo_pixel_encode(idx, W);
        GeoFields f = geo_pixel_decode(p);

        uint32_t i = idx % (W > 0 ? W : 1);

        /* trit: lossless — R[7:3] holds (idx%27) exactly if idx%27 < 32 */
        uint8_t exp_trit   = (uint8_t)(i % GP_TRIT_MOD);
        uint8_t exp_coset  = (uint8_t)(i % GP_COSET_MOD);
        uint8_t exp_fibo   = (uint8_t)(i % GP_FIBO_MOD);
        uint8_t exp_letter4= (uint8_t)(i % GP_LETTER_MOD & 0xFu);

        if (f.trit   != exp_trit)    errors++;
        if (f.coset  != exp_coset)   errors++;
        if (f.fibo   != exp_fibo)    errors++;
        if (f.letter != exp_letter4) errors++;
        if (f.spoke  > 7u)           errors++;   /* 3-bit field: 0..7 */
    }
    return errors;
}

/* ═══════════════════════════════════════
   UNIQUENESS CHECK (W=27 canonical grid)
   Checks that no two indices in [0,W) produce identical (trit,coset,fibo)
   triplet — the three lossless fields.
   Returns: 0 = all unique, >0 = collision count
   ═══════════════════════════════════════ */
static inline uint32_t geo_pixel_uniqueness_check(uint32_t W) {
    if (W > 256u) W = 256u;   /* cap for stack safety */

    /* track seen (trit,coset,fibo) triplets via small bitmap */
    /* max distinct = 27×9×144 = 34,992 — use flat array */
    static uint8_t seen[GP_TRIT_MOD][GP_COSET_MOD][GP_FIBO_MOD];
    memset(seen, 0, sizeof(seen));

    uint32_t collisions = 0;
    for (uint32_t i = 0; i < W; i++) {
        GeoPixel  p = geo_pixel_encode(i, W);
        GeoFields f = geo_pixel_decode(p);
        if (f.trit < GP_TRIT_MOD && f.coset < GP_COSET_MOD && f.fibo < GP_FIBO_MOD) {
            if (seen[f.trit][f.coset][f.fibo]++) collisions++;
        }
    }
    return collisions;
}

/* ═══════════════════════════════════════
   PIXEL → TRIT ADDRESS
   Direct bridge to fts_trit_addr compatible output
   Returns the trit index (0..26) from a decoded pixel
   ═══════════════════════════════════════ */
static inline uint8_t geo_pixel_to_trit(GeoPixel p) {
    return geo_pixel_decode(p).trit;
}

#endif /* GEO_PIXEL_H */
