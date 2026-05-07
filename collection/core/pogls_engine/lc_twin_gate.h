/*
 * lc_twin_gate.h — LC Polarity Gate + LetterPair Coupling
 * ════════════════════════════════════════════════════════
 * Gate layer ระหว่าง pipeline_wire_process และ geo_fast_intersect
 *
 * Decision flow:
 *   encode(addr) → LCHdr hA
 *   encode(value) → LCHdr hB
 *   lch_gate(hA, hB) → WARP | ROUTE | COLLISION | GROUND
 *
 * LetterPair coupling (P3):
 *   letter_idx = addr % 26        → A..Z slot
 *   slope_hash = fibo_seed ^ addr → apex slope fingerprint
 *   lc_pair_match → couple faces if angle+letter agree
 *
 * No malloc. No float. No heap.
 * ════════════════════════════════════════════════
 */

#ifndef LC_TWIN_GATE_H
#define LC_TWIN_GATE_H

#include <stdint.h>
#include <string.h>

/* ── LC constants ── */
#define LC_PAIRS      26u   /* A..Z */
#define LC_FACES       6u   /* frustum directions */
#define LC_PALETTE    16u   /* palette slots */

/* ── LCGate: routing decision ── */
typedef enum {
    LC_GATE_WARP      = 0,   /* fast intersect path        */
    LC_GATE_ROUTE     = 1,   /* normal intersect path      */
    LC_GATE_COLLISION = 2,   /* drift_acc += 8, then geo   */
    LC_GATE_GROUND    = 3,   /* bypass geo → dodeca direct */
} LCGate;

/* ── LCHdr: packed 8B header encoding geometry bits ── */
typedef struct {
    uint8_t  sign;      /* bit polarity 0/1          */
    uint8_t  mag;       /* magnitude class 0..7      */
    uint8_t  rgb;       /* color channel 0..2        */
    uint8_t  level;     /* depth level 0..3          */
    uint8_t  angle;     /* angle class 0..5          */
    uint8_t  letter;    /* A..Z slot (0..25)         */
    uint16_t slope;     /* slope fingerprint         */
} LCHdr;   /* 8B */

/* ── LetterPair ── */
typedef struct {
    uint8_t upper;   /* 0=A..25=Z */
    uint8_t lower;   /* 0=a..25=z */
} LetterPair;

static inline int lc_pair_valid(LetterPair p) {
    return (p.upper < LC_PAIRS) && (p.lower < LC_PAIRS)
           && (p.upper == p.lower);
}

static inline LetterPair lc_make_pair(uint8_t idx) {
    LetterPair p = { (uint8_t)(idx % LC_PAIRS),
                     (uint8_t)(idx % LC_PAIRS) };
    return p;
}

/* ── CubeNode: one frustum face slot ── */
typedef struct {
    uint64_t   core;
    LetterPair key;
    uint8_t    face_id;
    uint8_t    coupled;
} CubeNode;   /* 12B */

/* ── LCTwinGateCtx: gate context ── */
typedef struct {
    uint8_t  palette_a[LC_PALETTE];   /* addr palette */
    uint8_t  palette_b[LC_PALETTE];   /* value palette */
    CubeNode faces[LC_FACES];         /* LetterPair face slots */
    uint64_t slope_hash;              /* apex slope fingerprint */
    uint8_t  coupled_count;
    uint32_t gate_counts[4];          /* WARP/ROUTE/COLLISION/GROUND */
} LCTwinGateCtx;

/* ════════════════════════════════════════
   INIT
   ════════════════════════════════════════ */
static inline void lc_twin_gate_init(LCTwinGateCtx *g) {
    memset(g, 0, sizeof(*g));
    /* default palette: identity spread */
    for (uint8_t i = 0; i < LC_PALETTE; i++) {
        g->palette_a[i] = (uint8_t)(i * 17u % 26u);
        g->palette_b[i] = (uint8_t)(i * 13u % 26u);
    }
    for (uint8_t f = 0; f < LC_FACES; f++)
        g->faces[f].face_id = f;
}

/* ════════════════════════════════════════
   ENCODE: addr/value → LCHdr
   ════════════════════════════════════════ */
static inline LCHdr lc_hdr_encode_addr(uint64_t addr) {
    LCHdr h;
    h.sign   = (uint8_t)(addr >> 63);
    h.mag    = (uint8_t)((addr >> 48) & 0x7u);
    h.rgb    = (uint8_t)((addr >> 40) & 0x3u) % 3u;
    h.level  = (uint8_t)((addr >> 32) & 0x3u);
    h.angle  = (uint8_t)((addr >> 16) % 6u);
    h.letter = (uint8_t)(addr % LC_PAIRS);
    h.slope  = (uint16_t)((addr ^ (addr >> 16)) & 0xFFFFu);
    return h;
}

static inline LCHdr lc_hdr_encode_value(uint64_t value) {
    LCHdr h;
    h.sign   = (uint8_t)(value >> 63);
    h.mag    = (uint8_t)((value >> 48) & 0x7u);
    h.rgb    = (uint8_t)((value >> 40) & 0x3u) % 3u;
    h.level  = (uint8_t)((value >> 32) & 0x3u);
    h.angle  = (uint8_t)((value >> 16) % 6u);
    h.letter = (uint8_t)(value % LC_PAIRS);
    h.slope  = (uint16_t)((value ^ (value >> 16)) & 0xFFFFu);
    return h;
}

/* ════════════════════════════════════════
   GATE: LCHdr pair → routing decision
   ════════════════════════════════════════ */
static inline LCGate lch_gate(LCHdr hA, LCHdr hB,
                               const uint8_t pa[LC_PALETTE],
                               const uint8_t pb[LC_PALETTE])
{
    int same_polarity = (hA.sign == hB.sign);
    if (!same_polarity) return LC_GATE_GROUND;

    int aligned = (hA.angle == hB.angle);
    if (!aligned) return LC_GATE_COLLISION;

    /* check LetterPair coupling via palette */
    uint8_t la = pa[hA.letter % LC_PALETTE];
    uint8_t lb = pb[hB.letter % LC_PALETTE];
    int coupled = (la == lb) && (la < LC_PAIRS);

    return coupled ? LC_GATE_WARP : LC_GATE_ROUTE;
}

/* ════════════════════════════════════════
   P3 — LetterPair Coupling
   feed fibo_seed as slope_hash into CubeNode faces
   ════════════════════════════════════════ */
static inline void lc_gate_assign(LCTwinGateCtx *g,
                                   uint64_t addr,
                                   uint64_t fibo_seed,
                                   uint8_t  face_id)
{
    if (face_id >= LC_FACES) return;
    uint8_t  letter = (uint8_t)(addr % LC_PAIRS);
    uint64_t core   = addr ^ fibo_seed ^ ((uint64_t)face_id * 0x9E3779B97F4A7C15ULL);

    g->faces[face_id].core    = core;
    g->faces[face_id].key     = lc_make_pair(letter);
    g->faces[face_id].coupled = 0;
    g->slope_hash = fibo_seed ^ (addr >> 8);
}

static inline int lc_gate_match(const LCTwinGateCtx *g,
                                  uint8_t fa, uint8_t fb)
{
    if (fa >= LC_FACES || fb >= LC_FACES || fa == fb) return 0;
    const CubeNode *a = &g->faces[fa];
    const CubeNode *b = &g->faces[fb];

    if (!lc_pair_valid(a->key) || !lc_pair_valid(b->key)) return 0;
    int letter_ok = (a->key.upper == b->key.upper);
    if (!letter_ok) return 0;

    uint64_t angle_xor = a->core ^ b->core;
    int angle_ok = ((angle_xor ^ g->slope_hash) & 0xFFFFu) == 0;
    return angle_ok;
}

static inline int lc_gate_force_couple(LCTwinGateCtx *g)
{
    int coupled = 0;
    for (uint8_t a = 0; a < LC_FACES; a++) {
        for (uint8_t b = (uint8_t)(a + 1u); b < LC_FACES; b++) {
            if (lc_gate_match(g, a, b)) {
                g->faces[a].coupled = 1u;
                g->faces[b].coupled = 1u;
                coupled++;
            }
        }
    }
    g->coupled_count = (uint8_t)((g->coupled_count + coupled) & 0xFFu);
    return coupled;
}

/* helper for external stats callers */
static inline void lc_gate_count(LCTwinGateCtx *g, LCGate gate) {
    g->gate_counts[gate]++;
}

#endif /* LC_TWIN_GATE_H */
