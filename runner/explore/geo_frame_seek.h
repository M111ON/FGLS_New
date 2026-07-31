/*
 * geo_frame_seek.h — Deterministic Frame Seek on Fibo 1440 Timeline
 * ══════════════════════════════════════════════════════════════════
 *
 * 1 frame = 12 edges (9 Hilbert active + 3 Peano on invert/line-12)
 * All subsequent frames = same structure × phase iteration
 * Everything is deterministic — store only enc (2 bytes)
 *
 * Timeline: 1440 positions (fibo cycle)
 *   enc(t) = (t × 37) % 1440     — stride-37 walk, full bijection
 *   seek(enc) → frame O(1)       — no replay needed
 *   next(enc) → (enc + 37) % 1440
 *
 * Frame decomposition from enc:
 *   Hilbert: group(0..2), edge(0..2), is_skip
 *   Peano:   step(0..3) on line-12, sub(0..2) ternary
 *   ico_idx: 0..161 icosphere address
 *
 * Sacred constants (FROZEN):
 *   TRING_WALK_CYCLE  = 1440  (12 × 120)
 *   TRING_WALK_STRIDE = 37    (prime, gcd(37,1440)=1)
 *   META_FACE_SZ      = 120   (slots per face)
 *   FRAME_EDGES       = 12    (9 Hilbert + 3 Peano)
 *   ICO_NODES         = 162   (81 × 2 poles)
 *
 * No malloc. No float. Stateless O(1).
 * ══════════════════════════════════════════════════════════════════
 */

#ifndef GEO_FRAME_SEEK_H
#define GEO_FRAME_SEEK_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define FRAME_CYCLE       1440u   /* fibo timeline length            */
#define FRAME_STRIDE        37u   /* prime walk, gcd(37,1440)=1      */
#define FRAME_FACE_SZ      120u   /* slots per face (1440/12)        */
#define FRAME_EDGES         12u   /* edges per frame (9H + 3P)       */
#define FRAME_H_ACTIVE       9u   /* Hilbert active edges            */
#define FRAME_P_STEPS        4u   /* Peano steps on line-12          */
#define FRAME_ICO_NODES    162u   /* icosphere L2 (81×2)             */
#define FRAME_PEANO_GRID    81u   /* 3⁴ ternary space                */

/* ══════════════════════════════════════════════════════════════
   FRAME STRUCTS
   ══════════════════════════════════════════════════════════════ */

/* Hilbert edge — World A (north pole, binary) */
typedef struct {
    uint8_t group;    /* 0..2  which of 3 Hilbert groups    */
    uint8_t edge;     /* 0..2  edge within group            */
    uint8_t is_skip;  /* 1 = invert point (skip → line-12) */
} FrameHilbert;

/* Peano step — World B (south pole, ternary, runs on line-12) */
typedef struct {
    uint8_t step;         /* 0..3  position on line-12         */
    uint8_t sub;          /* 0..2  ternary sub-position        */
    uint8_t hilbert_group;/* which Hilbert skip maps here      */
} FramePeano;

/* Dual frame — one complete 12-edge unit */
typedef struct {
    FrameHilbert h;       /* World A                           */
    FramePeano   p;       /* World B (invert of H)             */
    uint16_t     enc;     /* source enc 0..1439                */
    uint8_t      ico_idx; /* 0..161 icosphere address          */
    uint8_t      face;    /* 0..11 dodecahedron face           */
    uint8_t      slot;    /* 0..119 slot within face           */
    uint8_t      phase;   /* iteration phase (enc/12 % 12)    */
} DualFrame;

/* ══════════════════════════════════════════════════════════════
   CORE: frame_at(enc) — O(1) seek
   ══════════════════════════════════════════════════════════════
 *
 * Decompose enc (0..1439) into full DualFrame.
 * All fields derived deterministically — no state needed.
 *
 * Hilbert decomposition:
 *   face  = enc / FACE_SZ          (0..11)
 *   group = face % 3               (0..2, 3 groups per 3-face cluster)
 *   edge  = enc % 3                (0..2, trit position)
 *   is_skip = (enc % 4 == 3)       (every 4th = skip/invert)
 *
 * Peano decomposition (runs on invert positions):
 *   step  = (enc / 3) % FRAME_P_STEPS   (0..3)
 *   sub   = enc % 3                      (0..2 ternary)
 *   hilbert_group = (enc / 12) % 3       (which skip cluster)
 *
 * icosphere: ico_idx = enc % ICO_NODES   (0..161)
 * phase:     enc / FRAME_EDGES % 12      (iteration counter)
 */
static inline DualFrame frame_at(uint16_t enc)
{
    DualFrame f;
    f.enc  = enc;
    f.face = (uint8_t)(enc / FRAME_FACE_SZ);          /* 0..11  */
    f.slot = (uint8_t)(enc % FRAME_FACE_SZ);          /* 0..119 */

    /* Hilbert */
    f.h.group   = (uint8_t)(f.face % 3);
    f.h.edge    = (uint8_t)(enc % 3);
    f.h.is_skip = (uint8_t)((enc % FRAME_EDGES) >= FRAME_H_ACTIVE);

    /* Peano — runs on invert (skip) domain */
    f.p.step          = (uint8_t)((enc / 3) % FRAME_P_STEPS);
    f.p.sub           = (uint8_t)(enc % 3);
    f.p.hilbert_group = (uint8_t)((enc / FRAME_EDGES) % 3);

    /* icosphere + phase */
    f.ico_idx = (uint8_t)(enc % FRAME_ICO_NODES);
    f.phase   = (uint8_t)((enc / FRAME_EDGES) % 12);

    return f;
}

/* ══════════════════════════════════════════════════════════════
   TIMELINE NAVIGATION — all O(1)
   ══════════════════════════════════════════════════════════════ */

/* enc at time t */
static inline uint16_t frame_enc(uint32_t t)
{
    return (uint16_t)((t * FRAME_STRIDE) % FRAME_CYCLE);
}

/* next enc in walk */
static inline uint16_t frame_next(uint16_t enc)
{
    return (uint16_t)((enc + FRAME_STRIDE) % FRAME_CYCLE);
}

/* prev enc in walk (inverse stride = 37, since 37×37%1440=1369≠1)
 * modinv(37,1440): 37×? ≡ 1 (mod 1440)
 * 37 × 1189 = 43993 = 30×1440 + 1193... compute properly:
 * use: prev = (enc + FRAME_CYCLE - FRAME_STRIDE) % FRAME_CYCLE */
static inline uint16_t frame_prev(uint16_t enc)
{
    return (uint16_t)((enc + FRAME_CYCLE - FRAME_STRIDE) % FRAME_CYCLE);
}

/* seek: jump directly to any time t */
static inline DualFrame frame_seek(uint32_t t)
{
    return frame_at(frame_enc(t));
}

/* cpair: diameter flip (north↔south pole) */
static inline uint16_t frame_cpair(uint16_t enc)
{
    return (uint16_t)((enc + FRAME_CYCLE / 2u) % FRAME_CYCLE);
}

/* ══════════════════════════════════════════════════════════════
   VERIFY — call once at init, returns 0 on pass
   ══════════════════════════════════════════════════════════════ */

static inline int geo_frame_seek_verify(void)
{
    /* [T1] stride-37 full cycle on 1440 */
    uint16_t visited[1440] = {0};
    uint16_t e = 0;
    for (uint32_t i = 0; i < FRAME_CYCLE; i++) {
        if (visited[e]) return -1;   /* duplicate */
        visited[e] = 1;
        e = frame_next(e);
    }
    if (e != 0) return -2;           /* must return to start */

    /* [T2] frame_enc / frame_at roundtrip */
    for (uint32_t t = 0; t < 1440u; t++) {
        DualFrame f = frame_seek(t);
        if (f.enc != frame_enc(t))   return -3;
        if (f.face > 11)             return -4;
        if (f.h.group > 2)           return -5;
        if (f.h.edge  > 2)           return -6;
        if (f.p.step  >= FRAME_P_STEPS) return -7;
        if (f.p.sub   > 2)           return -8;
        if (f.ico_idx >= FRAME_ICO_NODES) return -9;
    }

    /* [T3] cpair self-inverse */
    for (uint16_t enc = 0; enc < FRAME_CYCLE; enc++) {
        if (frame_cpair(frame_cpair(enc)) != enc) return -10;
    }

    /* [T4] prev(next(enc)) == enc */
    for (uint16_t enc = 0; enc < FRAME_CYCLE; enc++) {
        if (frame_prev(frame_next(enc)) != enc) return -11;
    }

    /* [T5] 9+3=12 edges: is_skip count over 12 consecutive */
    uint32_t skip_count = 0, active_count = 0;
    for (uint16_t enc = 0; enc < FRAME_EDGES; enc++) {
        DualFrame f = frame_at(enc);
        if (f.h.is_skip) skip_count++;
        else             active_count++;
    }
    if (active_count != FRAME_H_ACTIVE) return -12;  /* must be 9 */
    if (skip_count   != 3u)             return -13;  /* must be 3 */

    /* [T6] phase cycles 0..11 over 144 frames */
    for (uint16_t enc = 0; enc < 144u; enc++) {
        DualFrame f = frame_at(enc);
        if (f.phase >= 12u) return -14;
    }

    return 0;
}

#endif /* GEO_FRAME_SEEK_H */
