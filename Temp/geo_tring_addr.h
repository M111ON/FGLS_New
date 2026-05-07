/*
 * geo_tring_addr.h — Tring Geometric Tile Address
 * ═══════════════════════════════════════════════════════════════════
 *
 * Maps image tile coordinates (tx, ty, ch) ↔ Tring address (0..6911)
 *
 * Tring structure:
 *   12 tetra-5 compounds × 144 fibo patterns = 1728
 *   × 2 (dodecahedron parallel pentagon invert)  = 3456  ← GEO_FULL_N
 *   × 6 frustum faces                            = 20736 ← Hilbert full (144²)
 *   27648 - 20736                                = 6912  ← residual zone
 *
 * Residual zone bijection:
 *   6912 = 48 × 48 × 3  (tiles of 1536² image at TILE=32, 3 channels)
 *   → every tile+channel maps to exactly one Tring residual slot
 *   → no collision, no gap, fully invertible
 *
 * Address decomposition (6912 residual slots):
 *   addr = compound_id * 576 + pattern_id * 4 + invert * 2 + ch_bit
 *
 *   compound_id : 0..11   (which pentagon face of dodecahedron)
 *   pattern_id  : 0..143  (fibo clock position within compound)
 *   invert      : 0..1    (parallel pentagon pair orientation)
 *   ch_bit      : 0..1    (fine channel grain)
 *
 * Tile → addr uses geo_pixel fields for compatibility with existing pipeline:
 *   trit   = addr % 27   → 3³ ternary index
 *   spoke  = addr % 6    → frustum face direction
 *   coset  = addr % 9    → GiantCube index
 *   fibo   = addr % 144  → pattern_id position
 *
 * No malloc. No float. No heap.
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef GEO_TRING_ADDR_H
#define GEO_TRING_ADDR_H

#include <stdint.h>
#include <string.h>

/* ── Sacred constants (DO NOT CHANGE) ──────────────────────────── */
#define TR_COMPOUNDS     12u    /* pentagon faces of dodecahedron   */
#define TR_PATTERNS     144u    /* fibo clock per compound          */
#define TR_INVERT         2u    /* parallel face pairs              */
#define TR_TRING       1728u    /* 12 × 144                        */
#define TR_FULL        3456u    /* 1728 × 2 = GEO_FULL_N           */
#define TR_HILBERT    20736u    /* 3456 × 6 = 144²                 */
#define TR_SYNC       27648u    /* convergence boundary             */
#define TR_RESIDUAL    6912u    /* 27648 - 20736 = residual zone    */
#define TR_TILE_DIM      48u    /* 1536 / 32 = tiles per axis       */
#define TR_CHANNELS       3u    /* RGB                              */

/* compile-time sanity */
_Static_assert(TR_TRING  == TR_COMPOUNDS * TR_PATTERNS,  "tring");
_Static_assert(TR_FULL   == TR_TRING * TR_INVERT,        "full");
_Static_assert(TR_HILBERT== TR_FULL * 6u,                "hilbert");
_Static_assert(TR_RESIDUAL== TR_SYNC - TR_HILBERT,       "residual");
_Static_assert(TR_RESIDUAL== TR_TILE_DIM*TR_TILE_DIM*TR_CHANNELS, "bijection");

/* ── TringAddr: full geometric decomposition ────────────────────── */
typedef struct {
    uint16_t addr;          /* 0..6911 — residual zone slot         */
    uint8_t  compound_id;   /* 0..11   — pentagon face              */
    uint8_t  pattern_id;    /* 0..143  — fibo clock position        */
    uint8_t  invert;        /* 0..1    — parallel pair orientation  */
    uint8_t  ch_bit;        /* 0..1    — fine channel grain         */
    /* geo_pixel compatible fields */
    uint8_t  trit;          /* addr % 27  → 3³ index                */
    uint8_t  spoke;         /* addr % 6   → frustum face            */
    uint8_t  coset;         /* addr % 9   → GiantCube               */
    uint8_t  fibo;          /* addr % 144 → pattern position        */
} TringAddr;

/* ── TringTile: tile coordinate ─────────────────────────────────── */
typedef struct {
    uint8_t tx;   /* 0..47 — tile column */
    uint8_t ty;   /* 0..47 — tile row    */
    uint8_t ch;   /* 0..2  — RGB channel */
} TringTile;

/* ════════════════════════════════════════════════════════════════
 * ENCODE: (tx, ty, ch) → TringAddr
 *
 * Mapping:
 *   linear = ty * 48 + tx          (0..2303, tile scan order)
 *   compound_id = linear / 48      (0..47 → but we fold into 0..11)
 *   — fold: compound = (linear / 192) % 12
 *   — pattern = linear % 144
 *   — invert  = ch >> 1
 *   — ch_bit  = ch & 1
 *   addr = compound * 576 + pattern * 4 + invert * 2 + ch_bit
 * ════════════════════════════════════════════════════════════════ */
static inline TringAddr tring_encode(uint8_t tx, uint8_t ty, uint8_t ch) {
    /* linear_ch: folds tile+channel into single 0..6911 index */
    uint32_t lc       = (uint32_t)ty * TR_TILE_DIM * TR_CHANNELS
                      + (uint32_t)tx * TR_CHANNELS
                      + ch;                              /* 0..6911 */
    uint32_t compound = lc / (TR_PATTERNS * 4u);        /* /576 → 0..11 */
    uint32_t rem      = lc % (TR_PATTERNS * 4u);
    uint32_t pattern  = rem / 4u;                       /* 0..143 */
    uint32_t rem2     = rem % 4u;
    uint32_t invert   = rem2 / 2u;                      /* 0..1   */
    uint32_t ch_bit   = rem2 % 2u;                      /* 0..1   */

    uint32_t addr = compound * (TR_PATTERNS * 4u)
                  + pattern  * 4u
                  + invert   * 2u
                  + ch_bit;                             /* == lc  */

    TringAddr a;
    a.addr        = (uint16_t)addr;
    a.compound_id = (uint8_t)compound;
    a.pattern_id  = (uint8_t)pattern;
    a.invert      = (uint8_t)invert;
    a.ch_bit      = (uint8_t)ch_bit;
    a.trit  = (uint8_t)(addr % 27u);
    a.spoke = (uint8_t)(addr % 6u);
    a.coset = (uint8_t)(addr % 9u);
    a.fibo  = (uint8_t)(addr % 144u);
    return a;
}

/* ════════════════════════════════════════════════════════════════
 * DECODE: TringAddr → TringTile
 *
 * Inverse of encode — fully lossless roundtrip
 * ════════════════════════════════════════════════════════════════ */
static inline TringTile tring_decode(TringAddr a) {
    /* recover (compound, pattern, invert, ch_bit) */
    uint32_t addr     = a.addr;
    uint32_t compound = addr / (TR_PATTERNS * TR_INVERT * 2u);  /* /576 */
    uint32_t rem      = addr % (TR_PATTERNS * TR_INVERT * 2u);
    uint32_t pattern  = rem  / (TR_INVERT * 2u);                /* /4   */
    uint32_t rem2     = rem  % (TR_INVERT * 2u);
    uint32_t invert   = rem2 / 2u;
    uint32_t ch_bit   = rem2 % 2u;

    /* reconstruct linear_ch from addr (addr == lc by construction) */
    uint32_t lc      = addr;
    uint32_t ty_val  = lc / (TR_TILE_DIM * TR_CHANNELS);
    uint32_t rem_lc  = lc % (TR_TILE_DIM * TR_CHANNELS);
    uint32_t tx_val  = rem_lc / TR_CHANNELS;
    uint32_t ch_val  = rem_lc % TR_CHANNELS;

    TringTile t;
    t.ty = (uint8_t)ty_val;
    t.tx = (uint8_t)tx_val;
    t.ch = (uint8_t)ch_val;
    return t;
}

/* ════════════════════════════════════════════════════════════════
 * ROUNDTRIP VERIFY
 * Tests all 6912 slots: encode→decode→re-encode → addr matches
 * Returns 0 = perfect bijection, >0 = collision count
 * ════════════════════════════════════════════════════════════════ */
static inline uint32_t tring_roundtrip_verify(void) {
    uint32_t errors = 0;
    uint16_t seen[TR_RESIDUAL];
    memset(seen, 0xFF, sizeof(seen));  /* 0xFFFF = unused */

    for(uint8_t ch = 0; ch < TR_CHANNELS; ch++) {
        for(uint8_t ty = 0; ty < TR_TILE_DIM; ty++) {
            for(uint8_t tx = 0; tx < TR_TILE_DIM; tx++) {
                TringAddr  a  = tring_encode(tx, ty, ch);
                TringTile  t  = tring_decode(a);
                TringAddr  a2 = tring_encode(t.tx, t.ty, t.ch);

                /* addr must survive roundtrip */
                if(a.addr != a2.addr)              errors++;
                /* tile must survive roundtrip */
                if(t.tx != tx || t.ty != ty)       errors++;
                if(t.ch != ch)                     errors++;
                /* addr must be in valid range */
                if(a.addr >= TR_RESIDUAL)          errors++;
                /* no two tiles share same addr (bijection) */
                if(seen[a.addr] != 0xFFFF)         errors++;
                else seen[a.addr] = (uint16_t)(ty*TR_TILE_DIM+tx);
            }
        }
    }
    return errors;
}

/* ════════════════════════════════════════════════════════════════
 * TRING SORT KEY
 * Returns a 32-bit key for tile ordering based on geometric position
 * Key structure: [compound:4][pattern:8][invert:1][spoke:3][trit:5][coset:4][fibo:8]
 * ════════════════════════════════════════════════════════════════ */
static inline uint32_t tring_sort_key(TringAddr a) {
    return ((uint32_t)a.compound_id << 25)
         | ((uint32_t)a.pattern_id  << 17)
         | ((uint32_t)a.invert      << 16)
         | ((uint32_t)a.spoke       << 13)
         | ((uint32_t)a.trit        <<  8)
         | ((uint32_t)a.fibo);
}

/* ════════════════════════════════════════════════════════════════
 * TRING DEDUP STAMP
 * XOR-fold of geometric fields → compact fingerprint
 * Two tiles with same stamp → geometric equivalents (dedup candidates)
 * ════════════════════════════════════════════════════════════════ */
static inline uint32_t tring_stamp(TringAddr a) {
    uint32_t s = (uint32_t)a.trit;
    s = (s << 7) ^ (uint32_t)a.fibo;
    s = (s << 4) ^ (uint32_t)a.coset;
    s = (s << 3) ^ (uint32_t)a.spoke;
    s = (s << 1) ^ (uint32_t)a.invert;
    return s;
}

#endif /* GEO_TRING_ADDR_H */
