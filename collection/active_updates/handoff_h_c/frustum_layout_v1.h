/**
 * fgls_block_layout.h
 * FrustumBlock = 4896B container = 1 complete frustum unit
 *
 * Layout (frozen):
 *   [  17B ] header   — security DNA (17 = prime, not in 2^n×3^m)
 *   [3456B ] data     — 54 × 64B DiamondBlock (FIXED, no touch)
 *   [1440B ] meta     — structured, geometry-aligned
 *   ──────────────────
 *   [4896B ] total    = 17 × 2 × 144
 */

#ifndef FGLS_BLOCK_LAYOUT_H
#define FGLS_BLOCK_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

/* ── frozen constants ─────────────────────────────────────── */
#define FGLS_DIAMOND_COUNT    54        /* 2×3³ = Rubik stickers       */
#define FGLS_DIAMOND_BYTES    64        /* 64B per DiamondBlock         */
#define FGLS_DATA_BYTES       3456      /* 54×64                        */
#define FGLS_CLOCK_TICKS      144       /* FiboClock cycle              */
#define FGLS_DRAIN_COUNT      12        /* pentagon drains (Goldberg)   */
#define FGLS_SHADOW_COUNT     28        /* outer boundary (4×7)         */
#define FGLS_MERKLE_BYTES     32        /* SHA-256 per drain root       */
#define FGLS_META_BYTES       1440      /* 10×144 = gear family         */
#define FGLS_TOTAL_BYTES      4896      /* 17×2×144 = file boundary     */

/* ── header (17B) ────────────────────────────────────────── */
/* 17 = security prime, ไม่อยู่ใน 2^n×3^m → attacker stride miss */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];      /* "FGLS" = 0x46474C53               */
    uint8_t  version;       /* format version                    */
    uint8_t  rotation_state;/* 0..5 → lane_group = rot%6         */
    uint8_t  world_flags;   /* bit0=WorldA/B, bit6=drain_gap     */
    uint8_t  clock_phase;   /* current FiboClock phase 0..143    */
    uint64_t block_id;      /* unique block fingerprint (8B)     */
    /* total = 4+1+1+1+1+8 = 16B ... +1 reserved = 17B */
    uint8_t  _pad;          /* keeps header at exact 17B         */
} FrustumHeader;            /* sizeof = 17B ✓                    */

/* ── data zone (3456B) ───────────────────────────────────── */
/* DiamondBlock ไม่นิยามซ้ำที่นี่ — ใช้ของเดิมจาก geo_diamond_core.h */
/* offset: 17 .. 3472 */

/* ── meta zone (1440B) ───────────────────────────────────── */
typedef struct __attribute__((packed)) {

    /* letter_map: 144B — 1 byte per clock tick
     * maps tick → letter/symbol index in LC space
     * offset in meta: 0 */
    uint8_t  letter_map[FGLS_CLOCK_TICKS];          /* 144B */

    /* slope_map: 288B — 2 bytes per clock tick
     * maps tick → frustum slope (uint16, fixed-point Q8.8)
     * offset in meta: 144 */
    uint16_t slope_map[FGLS_CLOCK_TICKS];           /* 288B */

    /* drain_bitmap: 16B — 12 drains + control flags
     * bit layout per drain (1 byte each):
     *   bit0   = active drain (1=open, 0=closed)
     *   bit1   = flush_pending
     *   bit2   = merkle_dirty (root needs recompute)
     *   bit3   = confirmed_delete (tombstone committed)
     *   bit4-7 = reserved
     * remaining 4B = global drain control word
     * offset in meta: 432 */
    uint8_t  drain_state[FGLS_DRAIN_COUNT];         /* 12B */
    uint32_t drain_ctrl;                            /*  4B  → total 16B */

    /* shadow_bitmap: 32B — 28 shadow entries + 4B flags
     * 1 byte per shadow zone (index 0..27)
     *   bit0 = occupied (data written, not flushed)
     *   bit1 = heptagon_fence (one-way valve: once set, read→flush only)
     *   bit2-7 = reserved
     * offset in meta: 448 */
    uint8_t  shadow_state[FGLS_SHADOW_COUNT];       /* 28B */
    uint32_t shadow_ctrl;                           /*  4B  → total 32B */

    /* merkle_roots: 384B — 12 × 32B
     * 1 SHA-256 root per pentagon drain
     * root = commit hash of all blocks flushed through this drain
     * recompute triggered by merkle_dirty flag above
     * offset in meta: 480 */
    uint8_t  merkle_roots[FGLS_DRAIN_COUNT][FGLS_MERKLE_BYTES]; /* 384B */

    /* reserved: 576B = 4×144 — gear family, เผื่อ:
     *   rotation state expansion (1440 migration)
     *   lane_group index cache (54B max)
     *   future coset balance counters
     * offset in meta: 864 */
    uint8_t  reserved[576];                         /* 576B */

    /* meta total: 144+288+16+32+384+576 = 1440B ✓ */
} FrustumMeta;

/* ── top-level container (4896B) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    FrustumHeader header;                   /*   17B, offset=0    */
    uint8_t       data[FGLS_DATA_BYTES];    /* 3456B, offset=17   */
    FrustumMeta   meta;                     /* 1440B, offset=3473 */
    /* 17+3456+1440 = 4913 ... ไม่ใช่ 4896 → ดู note ด้านล่าง */
} FrustumBlock;

/* ── compile-time size assertions ───────────────────────────
 * ถ้า build พัง ที่นี่ = layout drift, แก้ก่อนทุกอย่าง
 */
_Static_assert(sizeof(FrustumHeader) == 17,   "header must be 17B");
_Static_assert(sizeof(FrustumMeta)   == 1440, "meta must be 1440B");
_Static_assert(sizeof(FrustumBlock)  == 4913, "⚠ see layout note");

/*
 * ⚠ LAYOUT NOTE — 4913 vs 4896
 * ─────────────────────────────
 * 17 + 3456 + 1440 = 4913 = 17³ (cube of 17 — ไม่ได้ตั้งใจ แต่ geometry จริง)
 * 4896 = 17 × 288 = 17 × 2 × 144 (file boundary จาก genesis)
 *
 * Gap = 17B — ตรงกับ header size พอดี
 * Resolution options:
 *   (A) ยอมรับ 4913 = 17³ เป็น true container size
 *       → file boundary = 4913B, เอกสาร genesis ต้อง update
 *   (B) ลด data zone: 3439B + 17B header = 3456, meta 1440
 *       → 3439 ไม่ align กับ 54×64 → พัง option นี้ทิ้ง
 *   (C) ลด meta 17B: reserved = 576-17 = 559B (ไม่ใช่ 4×144 → พัง geometry)
 *   (D) header absorb เข้า meta offset: header เป็น logical only,
 *       file = data(3456) + meta(1440) = 4896, header อยู่ใน reserved[0..16]
 *       → layout เดิมไม่พัง, header access ผ่าน pointer cast เท่านั้น
 *
 * แนะนำ (D) — header เป็น overlay บน reserved ไม่เพิ่ม byte จริง
 * FrustumBlock = data(3456) + meta(1440) = 4896B ✓
 * FrustumHeader = (FrustumMeta*)->reserved[0..16] ผ่าน cast
 */

/* ── option D: clean 4896B struct ──────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t     data[FGLS_DATA_BYTES];  /* 3456B, offset=0    */
    FrustumMeta meta;                   /* 1440B, offset=3456 */
} FrustumBlock4896;                     /* sizeof = 4896B ✓   */

/* header access via cast — zero cost */
static inline FrustumHeader* frustum_header(FrustumBlock4896 *b) {
    return (FrustumHeader*) &b->meta.reserved[0];
}

_Static_assert(sizeof(FrustumBlock4896) == 4896, "must be 4896B");

#endif /* FGLS_BLOCK_LAYOUT_H */
