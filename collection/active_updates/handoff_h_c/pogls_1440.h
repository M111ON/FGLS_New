/*
 * pogls_1440.h — Task #3: 1440 cycle migration (non-breaking overlay)
 * ════════════════════════════════════════════════════════════════════
 *
 * Design:
 *   1440 = 2 × 720 = two-phase execution layer
 *   phase 0 (idx 0..719)   : base cycle, bit6 = (trit^slope)&1
 *   phase 1 (idx 720..1439): XOR overlay, bit6 = (trit^slope^1)&1
 *
 * Non-breaking rule:
 *   pogls_dispatch_1440(idx) where idx < 720 → identical to base
 *   Core 720 logic UNTOUCHED
 *
 * Sacred: POGLS_BASE_CYCLE=720, POGLS_EXT_CYCLE=1440 — FROZEN
 * ════════════════════════════════════════════════════════════════════
 */

#ifndef POGLS_1440_H
#define POGLS_1440_H

#include <stdint.h>

/* ── constants ─────────────────────────────────────────────── */
#define POGLS_BASE_CYCLE  720u
#define POGLS_EXT_CYCLE  1440u   /* 2 × BASE */

/* ── phase result ──────────────────────────────────────────── */
typedef struct {
    uint16_t idx720;   /* 0..719 — base cycle index             */
    uint8_t  phase;    /* 0 or 1 — which layer                  */
    uint8_t  bit6;     /* Switch Gate = (trit^slope^phase)&1    */
} Pogls1440Slot;

/* ── trit/slope extractors (minimal, no GEO_WALK dep) ──────── */
static inline uint8_t _p1440_trit(uint16_t idx720) {
    return (uint8_t)(idx720 % 27u);   /* 3³ */
}
static inline uint8_t _p1440_slope(uint16_t idx720) {
    return (uint8_t)(idx720 % 256u);
}

/* ── core dispatch (patched: phase XOR) ────────────────────── */
/*
 * tgw_dispatch_v2_phase — drop-in with phase support
 * phase=0 → identical to original (bit6 = (trit^slope)&1)
 * phase=1 → flipped polarity layer
 */
static inline uint8_t tgw_dispatch_v2_phase(uint16_t idx720, uint8_t phase) {
    uint8_t trit  = _p1440_trit(idx720);
    uint8_t slope = _p1440_slope(idx720);
    return (uint8_t)((trit ^ slope ^ phase) & 1u);
}

/* ── 1440 wrapper ──────────────────────────────────────────── */
static inline Pogls1440Slot pogls_dispatch_1440(uint16_t idx) {
    Pogls1440Slot s;
    s.idx720 = (uint16_t)(idx % POGLS_BASE_CYCLE);
    s.phase  = (uint8_t)(idx / POGLS_BASE_CYCLE);   /* 0 or 1 */
    s.bit6   = tgw_dispatch_v2_phase(s.idx720, s.phase);
    return s;
}

#endif /* POGLS_1440_H */
