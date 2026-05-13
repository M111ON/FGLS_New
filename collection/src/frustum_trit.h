/*
 * frustum_trit.h — Trit Decomposition Engine
 * ════════════════════════════════════════════════════════════════
 *
 * Pure computational layer — no geometry deps, no GEO_WALK, no tetra.
 * Virtual apex is computed and subtracted; nothing maps into 5-tetra.
 *
 * One trit (0..26 = 3³) encodes three structural roles simultaneously:
 *
 *   trit  = (addr ^ value) % 27      [3³ address, 0..26]
 *   coset = trit / 3                 [GiantCube zone, 0..8  = 3²]
 *   face  = trit % 6                 [cube direction, 0..5]
 *   level = trit % 4                 [core depth,     0..3  = 2²]
 *   letter= addr  % 26               [LetterPair A..Z, 0..25]
 *   slope = fibo_seed ^ addr         [apex fingerprint, XOR-reversible]
 *
 * Cycle properties:
 *   lcm(3,6,4) = 12  → pattern repeats every 12 trits
 *   27 = 2×12 + 3    → two full cycles + one extra coset
 *   storing trit alone recovers all three roles (zero redundancy)
 *
 * Slope / apex fingerprint:
 *   slope_A XOR slope_B = addr_A XOR addr_B
 *   collision: slope equality ↔ same fibo-space position
 *   recovery:  addr = slope XOR fibo_seed  (1 op)
 *   security:  without fibo_seed → slope leaks nothing about addr
 *
 * No malloc. No float. No heap. Standalone.
 * ════════════════════════════════════════════════════════════════
 */

#ifndef FRUSTUM_TRIT_H
#define FRUSTUM_TRIT_H

#include <stdint.h>

/* ── constants ─────────────────────────────────────────────── */
#define TRIT_MOD       27u   /* 3³ */
#define COSET_COUNT     9u   /* 3² */
#define FACE_COUNT      6u   /* cube faces */
#define LEVEL_COUNT     4u   /* 2²  */
#define LETTER_COUNT   26u   /* A..Z */
#define FIBO_SEED_DEFAULT  1696631ULL  /* PHI_UP constant */

/* ── result struct ─────────────────────────────────────────── */
typedef struct {
    uint8_t  trit;      /* 0..26  — primary trit key            */
    uint8_t  coset;     /* 0..8   — GiantCube zone  (trit/3)    */
    uint8_t  face;      /* 0..5   — cube direction  (trit%6)    */
    uint8_t  level;     /* 0..3   — core depth      (trit%4)    */
    uint8_t  letter;    /* 0..25  — LetterPair A..Z (addr%26)   */
    uint8_t  _pad[3];
    uint64_t slope;     /* fibo_seed ^ addr — apex fingerprint  */
} TritAddr;

/* ── trit_decompose: main entry ────────────────────────────── */
static inline TritAddr trit_decompose(uint64_t addr,
                                       uint32_t value,
                                       uint64_t fibo_seed)
{
    TritAddr t;
    t.trit   = (uint8_t)((addr ^ (uint64_t)value) % TRIT_MOD);
    t.coset  = t.trit / 3u;
    t.face   = t.trit % FACE_COUNT;
    t.level  = t.trit % LEVEL_COUNT;
    t.letter = (uint8_t)(addr % LETTER_COUNT);
    t.slope  = fibo_seed ^ addr;
    t._pad[0] = t._pad[1] = t._pad[2] = 0u;
    return t;
}

/* ── trit_recover_addr: slope → addr (1 XOR op) ────────────── */
static inline uint64_t trit_recover_addr(uint64_t slope, uint64_t fibo_seed)
{
    return slope ^ fibo_seed;
}

/* ── trit_collision: same fibo-space position? ─────────────── */
static inline int trit_collision(const TritAddr *a, const TritAddr *b)
{
    return a->slope == b->slope;   /* slope_A==slope_B ↔ addr_A==addr_B */
}

/* ── trit_coset_silent: test reserved_mask bit for coset ───── */
static inline int trit_coset_silent(uint16_t reserved_mask, uint8_t coset)
{
    return (reserved_mask >> coset) & 1u;
}

/* ── trit_letter_char: letter index → ASCII char ───────────── */
static inline char trit_letter_char(uint8_t letter)
{
    return (char)('A' + (letter % LETTER_COUNT));
}

#endif /* FRUSTUM_TRIT_H */
