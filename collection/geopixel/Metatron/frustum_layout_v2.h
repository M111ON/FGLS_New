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

/* ── canonical container (4896B) — option D locked ──────────
 *
 * header เป็น logical overlay บน reserved[0..16]
 * ไม่มี byte cost จริง — 17³ effect เป็น emergent property
 *
 * memory map:
 *   offset    0 : data[3456]         54 DiamondBlocks
 *   offset 3456 : meta.letter_map    144B
 *   offset 3600 : meta.slope_map     288B
 *   offset 3888 : meta.drain_state   12B
 *   offset 3900 : meta.drain_ctrl    4B
 *   offset 3904 : meta.shadow_state  28B
 *   offset 3932 : meta.shadow_ctrl   4B
 *   offset 3936 : meta.merkle_roots  384B
 *   offset 4320 : meta.reserved      576B  ← header lives here [0..16]
 *   offset 4896 : END
 */
typedef struct __attribute__((packed)) {
    uint8_t     data[FGLS_DATA_BYTES];  /* 3456B, offset=0    */
    FrustumMeta meta;                   /* 1440B, offset=3456 */
} FrustumBlock;                         /* sizeof = 4896B ✓   */

/* header access — zero cost pointer cast, no memcpy */
static inline FrustumHeader* frustum_header(FrustumBlock *b) {
    return (FrustumHeader*) &b->meta.reserved[0];
}

/* read-only const variant */
static inline const FrustumHeader* frustum_header_c(const FrustumBlock *b) {
    return (const FrustumHeader*) &b->meta.reserved[0];
}

/* ── compile-time assertions — build fails = layout drift ─── */
_Static_assert(sizeof(FrustumHeader) == 17,   "header must be 17B");
_Static_assert(sizeof(FrustumMeta)   == 1440, "meta must be 1440B");
_Static_assert(sizeof(FrustumBlock)  == 4896, "block must be 4896B");
_Static_assert(offsetof(FrustumBlock, meta) == 3456, "meta offset must be 3456");

#endif /* FGLS_BLOCK_LAYOUT_H */
