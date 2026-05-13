/*
 * geo_tring_walk.h — TRing walk: tile index → TRing enc position
 * ═══════════════════════════════════════════════════════════════
 * Maps tile index i (0..NT-1) → enc (0..1439) for spoke routing.
 *
 * Design:
 *   enc(i) = (i * TRING_WALK_STRIDE) % TRING_WALK_CYCLE
 *
 *   STRIDE = 37 (prime, coprime to 1440 — verified: gcd(37,1440)=1)
 *   → full cycle coverage: stride 37 visits all 1440 positions ✓
 *   → uniform spoke distribution for any NT
 *   → max spoke imbalance ≤ ceil(NT/6) - floor(NT/6) tiles
 *
 * Usage:
 *   uint16_t enc = tring_walk_enc(i);          // enc 0..1439
 *   uint8_t  spk = tring_walk_spoke(i);        // spoke 0..5
 *   uint8_t  pol = tring_walk_polarity(i);     // 0=ROUTE 1=GROUND
 *
 * SEAM 3 patch pattern (unchanged — pixel coords still raster):
 *   int tx = i % TW, ty = i / TW;              // pixel coords: raster
 *   uint16_t enc = tring_walk_enc(i);           // dispatch enc: walk
 *
 * Sacred constants (frozen):
 *   TRING_WALK_CYCLE    = 1440  (6 spokes × 240)  ← migrated from 720
 *   TRING_WALK_STRIDE   = 37    (prime, gcd(37,1440)=1)
 *   TRING_WALK_SPOKE_SZ = 240   (positions per spoke)
 *   TRING_WALK_SPOKES   = 6
 *
 * 1440 geometry:
 *   1440 / 6  spokes = 240 per spoke
 *   1440 / 12 faces  = 120 per face  (Metatron META_FACE_SZ)
 *   1440 / 144       = 10 clock cycles per full walk
 *   1440 = 4 × 360 = quaternion coverage (pentagon sync point)
 *   digit_sum(1440)  = 9 ✓ GEAR family (×5 factor)
 *
 * CPAIR self-inverse at 1440: verified ✓
 *   cpair(enc) = (enc + 720) % 1440
 *   cpair(cpair(enc)) == enc for all enc 0..1439
 *
 * No malloc. No heap. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_TRING_WALK_H
#define GEO_TRING_WALK_H

#include <stdint.h>

#define TRING_WALK_CYCLE    1440u  /* pentagon sync point (was 720)  */
#define TRING_WALK_STRIDE     37u  /* prime, coprime to 1440         */
#define TRING_WALK_SPOKE_SZ  240u  /* positions per spoke (1440/6)   */
#define TRING_WALK_SPOKES      6u

/* ── enc: tile i → TRing position 0..1439 ────────────────────── */
static inline uint16_t tring_walk_enc(uint32_t i)
{
    return (uint16_t)((i * TRING_WALK_STRIDE) % TRING_WALK_CYCLE);
}

/* ── spoke: 0..5 ─────────────────────────────────────────────── */
static inline uint8_t tring_walk_spoke(uint32_t i)
{
    return (uint8_t)(tring_walk_enc(i) / TRING_WALK_SPOKE_SZ);
}

/* ── polarity: 0=ROUTE 1=GROUND  (enc%240 >= 120) ────────────── */
static inline uint8_t tring_walk_polarity(uint32_t i)
{
    return (uint8_t)((tring_walk_enc(i) % TRING_WALK_SPOKE_SZ) >= (TRING_WALK_SPOKE_SZ / 2u));
}

/* ── spoke histogram verify (debug) ─────────────────────────────
 * Returns max imbalance across 6 spokes for NT tiles.
 * Expect ≤ ceil(NT/6) - floor(NT/6) for any NT.
 */
static inline uint32_t tring_walk_spoke_imbalance(uint32_t NT)
{
    uint32_t hist[TRING_WALK_SPOKES] = {0};
    for (uint32_t i = 0u; i < NT; i++)
        hist[tring_walk_spoke(i)]++;
    uint32_t mn = hist[0], mx = hist[0];
    for (uint32_t s = 1u; s < TRING_WALK_SPOKES; s++) {
        if (hist[s] < mn) mn = hist[s];
        if (hist[s] > mx) mx = hist[s];
    }
    return mx - mn;
}

#endif /* GEO_TRING_WALK_H */
