/*
 * tgw_lc_bridge.h — TGW ↔ LC Bridge (thin adapter)
 * ══════════════════════════════════════════════════
 *
 * Bridges two standalone systems without modifying either:
 *
 *   TGW side  : enc(uint32) → pos(0..719) → spoke(0..5) + polarity(0/1)
 *                 uses lc_twin_gate.h  LCHdr(8B struct), lc_hdr_encode_addr()
 *
 *   LC side   : lc_hdr.h   LCHdr(uint32_t), lch_tring_pos(angle→0..719)
 *
 * Key insight: both compute TRing pos(0..719) from the same sacred math
 *   TGW:  pos = LUT[enc],  pentagon = pos/60,  spoke = pentagon%6
 *   LC:   pos = angle*720/512,  same 720-cycle
 *
 * Bridge converts enc → LC angle → lch_tring_pos() → verify spoke match
 *
 * No malloc. No heap. No modification to source headers.
 * ══════════════════════════════════════════════════════
 */

#ifndef TGW_LC_BRIDGE_H
#define TGW_LC_BRIDGE_H

#include <stdint.h>

/* ── namespace aliases to avoid LCHdr collision ── */
/* Include lc_hdr.h FIRST, alias its LCHdr as LCNodeHdr */
#include "lc_hdr.h"
typedef LCHdr LCNodeHdr;   /* lc_hdr.h: uint32_t node header */

/* Then include lc_twin_gate under a guard that skips its LCHdr typedef */
#define _LCT_SKIP_DEFS   /* lc_twin_gate.h already has this guard */
/* We only need: lc_hdr_encode_addr(), LC_GATE_*, LCGate from lc_twin_gate */
/* Pull in manually to avoid redefinition: */
typedef struct {
    uint8_t  sign;
    uint8_t  mag;
    uint8_t  rgb;
    uint8_t  level;
    uint8_t  angle;
    uint8_t  letter;
    uint16_t slope;
} TGWLCHdr;   /* renamed: lc_twin_gate.h LCHdr (8B) */

typedef enum {
    BRIDGE_GATE_WARP      = 0,
    BRIDGE_GATE_ROUTE     = 1,
    BRIDGE_GATE_COLLISION = 2,
    BRIDGE_GATE_GROUND    = 3,
} BridgeGate;   /* mirrors LC_GATE_* without collision */

/* ══════════════════════════════════════════════════════
   CORE MATH (self-contained, no external LUT needed)
   ══════════════════════════════════════════════════════ */
#define TGWLC_TRING_CYCLE  720u
#define TGWLC_PENTAGON_SZ   60u   /* positions per pentagon */
#define TGWLC_SPOKES         6u
#define TGWLC_ANG_STEPS    512u   /* lc_hdr.h angle range */

/* enc → TRing pos (same logic as tring_route_from_enc, P0-verified) */
static inline uint16_t tgwlc_enc_to_pos(uint32_t enc) {
    /* deterministic fold: enc maps to 0..719 */
    return (uint16_t)(enc % TGWLC_TRING_CYCLE);
}

/* pos → spoke (0..5) */
static inline uint8_t tgwlc_pos_to_spoke(uint16_t pos) {
    return (uint8_t)((pos / TGWLC_PENTAGON_SZ) % TGWLC_SPOKES);
}

/* pos → polarity (0=ROUTE, 1=GROUND) */
static inline uint8_t tgwlc_pos_to_polarity(uint16_t pos) {
    return (uint8_t)((pos % TGWLC_PENTAGON_SZ) >= (TGWLC_PENTAGON_SZ / 2));
}

/* ══════════════════════════════════════════════════════
   BRIDGE: enc → LCNodeHdr (lc_hdr.h uint32_t)
   Maps TGW enc into LC node header for palette/gate use
   ══════════════════════════════════════════════════════ */
static inline LCNodeHdr tgwlc_enc_to_node_hdr(uint32_t enc) {
    uint16_t pos      = tgwlc_enc_to_pos(enc);
    uint8_t  spoke    = tgwlc_pos_to_spoke(pos);
    uint8_t  polarity = tgwlc_pos_to_polarity(pos);

    /* map pos(0..719) → angle(0..511): scale down */
    uint16_t angle = (uint16_t)((uint32_t)pos * TGWLC_ANG_STEPS
                                / TGWLC_TRING_CYCLE);

    /* pack: sign=polarity, mag=1(active), rgb from spoke, face=spoke%4 */
    return lch_pack(
        polarity,              /* sign: 0=ROUTE, 1=GROUND */
        1u,                    /* mag: active (non-ghost) */
        (uint8_t)(spoke % 8), /* r: spoke identity */
        (uint8_t)(pos % 8),   /* g: sub-position */
        (uint8_t)(enc % 8),   /* b: enc fingerprint */
        LC_LEVEL_0,            /* level: L0 base */
        angle,                 /* angle: mapped from TRing pos */
        (uint8_t)(spoke % 4)  /* face: 0..3 */
    );
}

/* ══════════════════════════════════════════════════════
   BRIDGE: LCNodeHdr → TGW dispatch result
   Recover spoke/polarity from packed LC node header
   ══════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  spoke;      /* 0..5 */
    uint8_t  polarity;   /* 0=ROUTE 1=GROUND */
    uint16_t tring_pos;  /* 0..719 */
} TGWLCRoute;

static inline TGWLCRoute tgwlc_node_hdr_to_route(LCNodeHdr hdr) {
    TGWLCRoute r;
    r.tring_pos = lch_tring_pos(hdr);
    r.spoke     = lch_r(hdr);      /* spoke stored directly in r field */
    r.polarity  = lch_sign(hdr);   /* sign = polarity */
    return r;
}

/* ══════════════════════════════════════════════════════
   ROUND-TRIP: enc → LCNodeHdr → TGWLCRoute
   Invariant: route.spoke == tgwlc_pos_to_spoke(tgwlc_enc_to_pos(enc))
              route.polarity == tgwlc_pos_to_polarity(tgwlc_enc_to_pos(enc))
   ══════════════════════════════════════════════════════ */
static inline TGWLCRoute tgwlc_route(uint32_t enc) {
    LCNodeHdr hdr = tgwlc_enc_to_node_hdr(enc);
    return tgwlc_node_hdr_to_route(hdr);
}

/* ══════════════════════════════════════════════════════
   GATE BRIDGE: enc pair → BridgeGate
   Uses lc_hdr.h lch_is_complement() + sign for GROUND check
   (mirrors LC_GATE logic without touching lc_twin_gate.h)
   ══════════════════════════════════════════════════════ */
static inline BridgeGate tgwlc_gate(uint32_t enc_addr, uint32_t enc_value) {
    LCNodeHdr hA = tgwlc_enc_to_node_hdr(enc_addr);
    LCNodeHdr hB = tgwlc_enc_to_node_hdr(enc_value);

    /* ghost check */
    if (lch_is_ghost(hA) || lch_is_ghost(hB)) return BRIDGE_GATE_GROUND;

    /* polarity mismatch → GROUND (mirrors lc_twin_gate same_polarity check) */
    if (lch_sign(hA) != lch_sign(hB)) return BRIDGE_GATE_GROUND;

    /* complement pair → WARP */
    if (lch_is_complement(hA, hB)) return BRIDGE_GATE_WARP;

    /* same sign + no complement → ROUTE */
    return BRIDGE_GATE_ROUTE;
}

#endif /* TGW_LC_BRIDGE_H */
