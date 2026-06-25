/*
 * frustum_coset.h — FrustumBlock 4896B Coset Layout
 * ═══════════════════════════════════════════════════════════════
 *
 * Defines byte-exact offsets for every coset inside a FrustumBlock.
 * All seeks are O(1) — no loop, no index, no malloc.
 *
 * ── FrustumBlock memory map (4896B) ─────────────────────────
 *
 *   offset     0 .. 3455  [3456B] DATA ZONE — 54 DiamondBlocks × 64B
 *   offset  3456 .. 3599  [ 144B] meta.letter_map
 *   offset  3600 .. 3887  [ 288B] meta.slope_map
 *   offset  3888 .. 3899  [  12B] meta.drain_state
 *   offset  3900 .. 3903  [   4B] meta.drain_ctrl
 *   offset  3904 .. 3931  [  28B] meta.shadow_state
 *   offset  3932 .. 3935  [   4B] meta.shadow_ctrl
 *   offset  3936 .. 4319  [ 384B] meta.merkle_roots (12 × 32B)
 *   offset  4320 .. 4895  [ 576B] meta.reserved  ← FrustumHeader @[0..16]
 *
 * ── Data Zone Coset Structure ────────────────────────────────
 *
 *   54 DiamondBlocks are partitioned by coset = diamond_idx % 3
 *   Each coset holds 18 diamonds (= 18 × 64B = 1152B)
 *
 *   Coset 0 (mod3=0): diamond idx  0, 3, 6, ..., 51
 *   Coset 1 (mod3=1): diamond idx  1, 4, 7, ..., 52
 *   Coset 2 (mod3=2): diamond idx  2, 5, 8, ..., 53
 *
 *   Direct seek formula (O(1)):
 *     coset_nth(coset, n) = coset * 64 + n * 192
 *     stride = 3 × 64 = 192B (skip to next diamond in same coset)
 *
 *   Coset alignment to sacred constants:
 *     3 cosets  = 3¹  = Fibonacci seed
 *     18 per coset = 2 × 3² = 2 × 9
 *     54 total  = 2 × 3³ = Rubik sticker count
 *
 * ── DiamondBlock 64B Internal Layout ────────────────────────
 *
 *   offset  0..15  [16B] payload_a   — World A data (STORE side)
 *   offset 16..31  [16B] payload_b   — World B data (DRAIN side)
 *   offset 32..47  [16B] tring_slot  — temporal ring linkage (720/1440)
 *   offset 48..63  [16B] meta_inline — clock_phase, flags, lane_group
 *
 * Sacred: FRUSTUM_BLOCK_BYTES=4896, FCOSET_DIAMOND_BYTES=64 — FROZEN
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef FRUSTUM_COSET_H
#define FRUSTUM_COSET_H

#include <stdint.h>
#include <stdbool.h>

/* ══════════════════════════════════════════════════════════════
 * FROZEN CONSTANTS
 * ══════════════════════════════════════════════════════════════ */

#define FCOSET_BLOCK_BYTES     4896u   /* total FrustumBlock size          */
#define FCOSET_DIAMOND_BYTES     64u   /* bytes per DiamondBlock           */
#define FCOSET_DIAMOND_COUNT     54u   /* 2×3³ diamonds per block          */
#define FCOSET_COSET_COUNT        3u   /* trinity partition (mod 3)        */
#define FCOSET_PER_COSET         18u   /* 54 / 3 = 18 diamonds per coset   */
#define FCOSET_STRIDE           192u   /* 3 × 64B — skip to next in coset  */

/* ── meta zone absolute offsets ──────────────────────────── */
#define FCOSET_META_OFF        3456u   /* start of FrustumMeta             */
#define FCOSET_LETTER_MAP_OFF  3456u   /* letter_map[144]                  */
#define FCOSET_SLOPE_MAP_OFF   3600u   /* slope_map[144] × 2B              */
#define FCOSET_DRAIN_STATE_OFF 3888u   /* drain_state[12]                  */
#define FCOSET_DRAIN_CTRL_OFF  3900u   /* drain_ctrl (4B)                  */
#define FCOSET_SHADOW_STATE_OFF 3904u  /* shadow_state[28]                 */
#define FCOSET_SHADOW_CTRL_OFF 3932u   /* shadow_ctrl (4B)                 */
#define FCOSET_MERKLE_OFF      3936u   /* merkle_roots[12][32]             */
#define FCOSET_RESERVED_OFF    4320u   /* reserved[576] — header @[0..16]  */
#define FCOSET_HEADER_OFF      4320u   /* FrustumHeader inside reserved    */

/* ── DiamondBlock internal field offsets ─────────────────── */
#define FCOSET_DIA_PAYLOAD_A    0u     /* World A data  [16B]              */
#define FCOSET_DIA_PAYLOAD_B   16u     /* World B data  [16B]              */
#define FCOSET_DIA_TRING_SLOT  32u     /* tring linkage [16B]              */
#define FCOSET_DIA_META_INLINE 48u     /* clock, flags  [16B]              */
#define FCOSET_DIA_FIELD_BYTES 16u     /* each field = 16B                 */

/* ══════════════════════════════════════════════════════════════
 * COSET SEEK — O(1) direct byte offset into FrustumBlock
 * ══════════════════════════════════════════════════════════════ */

/*
 * fcoset_diamond_off: offset of diamond[idx] inside block data zone
 *   idx = 0..53
 *   returns byte offset from start of FrustumBlock
 */
static inline uint32_t fcoset_diamond_off(uint8_t idx)
{
    return (uint32_t)idx * FCOSET_DIAMOND_BYTES;
}

/*
 * fcoset_coset_base: offset of first diamond in coset c (c = 0,1,2)
 *   coset 0 → offset   0 (diamond 0)
 *   coset 1 → offset  64 (diamond 1)
 *   coset 2 → offset 128 (diamond 2)
 */
static inline uint32_t fcoset_coset_base(uint8_t coset)
{
    return (uint32_t)coset * FCOSET_DIAMOND_BYTES;
}

/*
 * fcoset_nth: offset of the n-th diamond in coset c
 *   c = 0,1,2   (coset id, mod3 of diamond_idx)
 *   n = 0..17   (position within coset, 18 diamonds total)
 *   returns byte offset from start of FrustumBlock
 *
 *   formula:  c*64 + n*192
 *   ≡ (c + n*3) * 64  — same as diamond_idx * 64
 */
static inline uint32_t fcoset_nth(uint8_t coset, uint8_t n)
{
    return (uint32_t)coset * FCOSET_DIAMOND_BYTES
         + (uint32_t)n     * FCOSET_STRIDE;
}

/*
 * fcoset_of: coset id of diamond[idx]  (0, 1, or 2)
 */
static inline uint8_t fcoset_of(uint8_t diamond_idx)
{
    return (uint8_t)(diamond_idx % FCOSET_COSET_COUNT);
}

/*
 * fcoset_n_of: position within coset (0..17) of diamond[idx]
 */
static inline uint8_t fcoset_n_of(uint8_t diamond_idx)
{
    return (uint8_t)(diamond_idx / FCOSET_COSET_COUNT);
}

/* ══════════════════════════════════════════════════════════════
 * DiamondBlock field seek — offset from diamond base
 * ══════════════════════════════════════════════════════════════ */

/*
 * fcoset_field_off: absolute offset of a field inside diamond[idx]
 *   field_off = FCOSET_DIA_PAYLOAD_A / _B / _TRING_SLOT / _META_INLINE
 */
static inline uint32_t fcoset_field_off(uint8_t diamond_idx, uint32_t field_off)
{
    return fcoset_diamond_off(diamond_idx) + field_off;
}

/* convenience wrappers */
static inline uint32_t fcoset_payload_a_off  (uint8_t idx) { return fcoset_field_off(idx, FCOSET_DIA_PAYLOAD_A);    }
static inline uint32_t fcoset_payload_b_off  (uint8_t idx) { return fcoset_field_off(idx, FCOSET_DIA_PAYLOAD_B);    }
static inline uint32_t fcoset_tring_slot_off (uint8_t idx) { return fcoset_field_off(idx, FCOSET_DIA_TRING_SLOT);   }
static inline uint32_t fcoset_meta_inline_off(uint8_t idx) { return fcoset_field_off(idx, FCOSET_DIA_META_INLINE);  }

/* ══════════════════════════════════════════════════════════════
 * META ZONE SEEK — absolute offsets for meta fields
 * ══════════════════════════════════════════════════════════════ */

/*
 * fcoset_letter_map_off: offset of letter_map[tick]  tick=0..143
 */
static inline uint32_t fcoset_letter_map_off(uint8_t tick)
{
    return FCOSET_LETTER_MAP_OFF + (uint32_t)tick;
}

/*
 * fcoset_slope_map_off: offset of slope_map[tick]  (2B entry)  tick=0..143
 */
static inline uint32_t fcoset_slope_map_off(uint8_t tick)
{
    return FCOSET_SLOPE_MAP_OFF + (uint32_t)tick * 2u;
}

/*
 * fcoset_drain_state_off: offset of drain_state[d]  d=0..11
 */
static inline uint32_t fcoset_drain_state_off(uint8_t d)
{
    return FCOSET_DRAIN_STATE_OFF + (uint32_t)d;
}

/*
 * fcoset_merkle_root_off: offset of merkle_roots[d]  d=0..11  (32B each)
 */
static inline uint32_t fcoset_merkle_root_off(uint8_t d)
{
    return FCOSET_MERKLE_OFF + (uint32_t)d * 32u;
}

/*
 * fcoset_shadow_state_off: offset of shadow_state[s]  s=0..27
 */
static inline uint32_t fcoset_shadow_state_off(uint8_t s)
{
    return FCOSET_SHADOW_STATE_OFF + (uint32_t)s;
}

/* ══════════════════════════════════════════════════════════════
 * COSET ITERATOR — walk all diamonds in a coset, no storage
 * ══════════════════════════════════════════════════════════════
 *
 * Usage:
 *   FcosetIter it;
 *   fcoset_iter_init(&it, coset_id);
 *   while (fcoset_iter_next(&it)) {
 *       uint32_t off = it.off;   // byte offset into FrustumBlock
 *       uint8_t  idx = it.idx;   // diamond_idx 0..53
 *   }
 */

typedef struct {
    uint8_t  coset;   /* 0, 1, 2 */
    uint8_t  n;       /* current position within coset 0..17 */
    uint8_t  idx;     /* diamond_idx = coset + n*3          */
    uint32_t off;     /* byte offset from FrustumBlock start */
} FcosetIter;

static inline void fcoset_iter_init(FcosetIter *it, uint8_t coset)
{
    it->coset = coset;
    it->n     = 0u;
    it->idx   = coset;
    it->off   = fcoset_coset_base(coset);
}

/* returns true while more diamonds remain */
static inline bool fcoset_iter_next(FcosetIter *it)
{
    if (it->n >= FCOSET_PER_COSET) return false;
    it->off = fcoset_nth(it->coset, it->n);
    it->idx = (uint8_t)(it->coset + it->n * FCOSET_COSET_COUNT);
    it->n++;
    return true;
}

/* ══════════════════════════════════════════════════════════════
 * VERIFY — all offsets, roundtrip, boundary
 * ══════════════════════════════════════════════════════════════ */

static inline int fcoset_verify(void)
{
    /* diamond[0] at offset 0 */
    if (fcoset_diamond_off(0)  != 0u)    return -1;
    /* diamond[53] at offset 53*64 = 3392 */
    if (fcoset_diamond_off(53) != 3392u) return -2;
    /* last diamond end: 3392+64 = 3456 = meta start */
    if (3392u + FCOSET_DIAMOND_BYTES != FCOSET_META_OFF) return -3;

    /* coset bases */
    if (fcoset_coset_base(0) != 0u)   return -4;
    if (fcoset_coset_base(1) != 64u)  return -5;
    if (fcoset_coset_base(2) != 128u) return -6;

    /* fcoset_nth: coset1 n=2 → 1*64 + 2*192 = 448 = diamond[7]*64 ✓ */
    if (fcoset_nth(1, 2) != 448u) return -7;
    if (fcoset_nth(1, 2) != fcoset_diamond_off(7)) return -8;

    /* fcoset_nth last entry: coset2 n=17 → 2*64 + 17*192 = 3392 = diamond[53] */
    if (fcoset_nth(2, 17) != 3392u) return -9;

    /* coset_of / n_of roundtrip */
    for (uint8_t i = 0; i < FCOSET_DIAMOND_COUNT; i++) {
        uint8_t c = fcoset_of(i);
        uint8_t n = fcoset_n_of(i);
        if (fcoset_nth(c, n) != fcoset_diamond_off(i)) return -10;
    }

    /* DiamondBlock field offsets */
    if (fcoset_payload_a_off(0)   != 0u)   return -11;
    if (fcoset_payload_b_off(0)   != 16u)  return -12;
    if (fcoset_tring_slot_off(0)  != 32u)  return -13;
    if (fcoset_meta_inline_off(0) != 48u)  return -14;
    /* diamond[1] payload_b = 64 + 16 = 80 */
    if (fcoset_payload_b_off(1)   != 80u)  return -15;

    /* meta zone boundaries */
    if (FCOSET_LETTER_MAP_OFF != 3456u)  return -16;
    if (FCOSET_SLOPE_MAP_OFF  != 3600u)  return -17;
    if (FCOSET_MERKLE_OFF     != 3936u)  return -18;
    if (FCOSET_HEADER_OFF     != 4320u)  return -19;
    /* last reserved byte = 4320 + 576 - 1 = 4895 */
    if (FCOSET_RESERVED_OFF + 576u != FCOSET_BLOCK_BYTES) return -20;

    /* slope_map[143] = 3600 + 143*2 = 3886, end = 3888 = drain_state */
    if (fcoset_slope_map_off(143) != 3886u)   return -21;
    if (3886u + 2u != FCOSET_DRAIN_STATE_OFF) return -22;

    /* merkle drain 11 = 3936 + 11*32 = 4288, end = 4320 = reserved */
    if (fcoset_merkle_root_off(11) != 4288u)   return -23;
    if (4288u + 32u != FCOSET_RESERVED_OFF)    return -24;

    /* iterator: walk coset 0, verify 18 stops */
    FcosetIter it;
    fcoset_iter_init(&it, 0);
    uint8_t count = 0;
    while (fcoset_iter_next(&it)) {
        if (it.off != fcoset_diamond_off(it.idx)) return -25;
        count++;
    }
    if (count != FCOSET_PER_COSET) return -26;

    return 0;   /* all clear */
}

#endif /* FRUSTUM_COSET_H */
