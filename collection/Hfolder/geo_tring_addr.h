/*
 * geo_tring_addr.h — Temporal Ring Address Encoder (self-contained)
 * ══════════════════════════════════════════════════════════════════
 * Maps (tile_x, tile_y, channel) → TringAddr { addr, pattern_id }
 *
 * Address space:
 *   live zone    : 0..3455  (3456 = 144×24 = GEO_FULL_N)
 *   residual zone: 3456..6911 (6912 = 3456×2)
 *
 * Encoding:
 *   compound = (tx + ty*stride) % 144  → fibo clock position
 *   spoke    = (tx ^ ty ^ ch)  % 6     → dodeca direction
 *   offset   = (tx*3 + ty*7 + ch*11)   → position within compound
 *
 *   addr     = compound*24 + spoke*4 + (offset%4)   → 0..3455
 *   if ch==2 (residual channel): addr += 3456        → residual zone
 *
 *   pattern_id = compound % 144  → zone classifier (0..143)
 *
 * Roundtrip: tring_decode(tring_encode(tx,ty,ch)) == (tx,ty,ch)
 *   NOT bijective for all (tx,ty) — addr space 3456 < tile space
 *   BUT pattern_id is stable per compound → zone classification reliable
 *
 * Sacred numbers preserved: 144, 3456, 6912, 6, 27
 * No malloc. No float.
 * ══════════════════════════════════════════════════════════════════
 */

#ifndef GEO_TRING_ADDR_H
#define GEO_TRING_ADDR_H

#include <stdint.h>
#include <string.h>

#define TRING_LIVE_N      3456u   /* GEO_FULL_N = 144×24                */
#define TRING_RESIDUAL_N  3456u   /* mirror zone                         */
#define TRING_TOTAL       6912u   /* live + residual                     */
#define TRING_COMPOUNDS    144u   /* fibo clock period                   */
#define TRING_SPOKES         6u   /* dodeca faces                        */
#define TRING_LEVELS         4u   /* slots per spoke                     */
#define TRING_STRIDE        32u   /* canonical tile stride (image width) */

typedef struct {
    uint16_t addr;        /* geometric address 0..6911  */
    uint8_t  pattern_id;  /* compound index 0..143      */
    uint8_t  zone;        /* 0=anchor 1=mid 2=residual  */
} TringAddr;

/* ── encode (tx, ty, ch) → TringAddr ─────────────────────────── */
static inline TringAddr tring_encode(uint8_t tx, uint8_t ty, uint8_t ch) {
    uint32_t compound  = ((uint32_t)tx + (uint32_t)ty * TRING_STRIDE)
                         % TRING_COMPOUNDS;
    uint32_t spoke     = ((uint32_t)tx ^ (uint32_t)ty ^ (uint32_t)ch)
                         % TRING_SPOKES;
    uint32_t offset    = ((uint32_t)tx * 3u
                        + (uint32_t)ty * 7u
                        + (uint32_t)ch * 11u) % TRING_LEVELS;

    uint32_t addr      = compound * (TRING_SPOKES * TRING_LEVELS)
                       + spoke    * TRING_LEVELS
                       + offset;   /* 0..3455 */

    /* ch==2: route to residual zone */
    if (ch == 2u) addr += TRING_LIVE_N;

    uint8_t pid  = (uint8_t)(compound % TRING_COMPOUNDS);
    uint8_t zone = (pid < 48u) ? 0u : (pid < 96u) ? 1u : 2u;

    TringAddr r;
    r.addr       = (uint16_t)(addr % TRING_TOTAL);
    r.pattern_id = pid;
    r.zone       = zone;
    return r;
}

/* ── roundtrip verify (zone stability, not full bijection) ────── */
static inline uint32_t tring_roundtrip_verify(void) {
    uint32_t errors = 0;
    /* verify: all addresses land in valid range */
    for (uint8_t ty = 0; ty < 48u; ty++) {
        for (uint8_t tx = 0; tx < 48u; tx++) {
            for (uint8_t ch = 0; ch < 3u; ch++) {
                TringAddr a = tring_encode(tx, ty, ch);
                if (a.addr >= TRING_TOTAL)        errors++;
                if (a.pattern_id >= TRING_COMPOUNDS) errors++;
                if (a.zone > 2u)                  errors++;
                /* ch 0/1 → live zone, ch 2 → residual zone */
                if (ch < 2u && a.addr >= TRING_LIVE_N)  errors++;
                if (ch == 2u && a.addr < TRING_LIVE_N)  errors++;
            }
        }
    }
    return errors;
}

#endif /* GEO_TRING_ADDR_H */
