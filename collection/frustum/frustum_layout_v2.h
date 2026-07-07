/*
 * frustum_layout_v2.h — FrustumBlock container (4896B)
 *
 * Architecture:
 *   [  17B ] header   — security DNA (17 = prime)
 *   [3456B ] data     — 54 × 64B DiamondBlock
 *   [1440B ] meta     — structured, geometry-aligned
 *   ──────────────────
 *   [4896B ] total    = 17 × 2 × 144
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Frozen constants ──────────────────────────────────── */
#define DGLS_DIAMOND_COUNT    54        /* 2×3³ = Rubik stickers       */
#define DGLS_DIAMOND_BYTES    64        /* 64B per DiamondBlock         */
#define DGLS_DATA_BYTES       3456      /* 54×64                        */
#define DGLS_CLOCK_TICKS      144       /* FiboClock cycle              */
#define DGLS_DRAIN_COUNT      12        /* pentagon drains (Goldberg)   */
#define DGLS_SHADOW_COUNT     27        /* trit-cube 3³                 */
#define DGLS_POLAR_COUNT       3        /* icosa poles N/S/C            */
#define DGLS_MERKLE_BYTES     32        /* SHA-256 per drain root       */
#define DGLS_META_BYTES       1440      /* 10×144 = gear family         */
#define DGLS_TOTAL_BYTES      4896      /* 17×2×144 = file boundary     */

/* ── Header (17B) ──────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];      /* "DGLS" = 0x44474C53               */
    uint8_t  version;       /* format version                    */
    uint8_t  rotation_state;/* 0..5 → lane_group = rot%6         */
    uint8_t  world_flags;   /* bit0=WorldA/B, bit6=drain_gap     */
    uint8_t  clock_phase;   /* current FiboClock phase 0..143    */
    uint64_t block_id;      /* unique block fingerprint (8B)     */
    uint8_t  _pad;          /* keeps header at exact 17B         */
} FrustumHeader;            /* sizeof = 17B ✓                    */

/* ── Meta zone (1440B) ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  letter_map[DGLS_CLOCK_TICKS];          /* 144B */
    uint16_t slope_map[DGLS_CLOCK_TICKS];           /* 288B */
    uint8_t  drain_state[DGLS_DRAIN_COUNT];         /*  12B */
    uint32_t drain_ctrl;                            /*   4B */
    uint8_t  shadow_state[DGLS_SHADOW_COUNT];       /*  27B */
    uint8_t  polar_state[DGLS_POLAR_COUNT];         /*   3B */
    uint32_t shadow_ctrl;                           /*   4B */
    uint8_t  merkle_roots[DGLS_DRAIN_COUNT][DGLS_MERKLE_BYTES]; /* 384B */
    uint8_t  reserved[574];                         /* 574B */
    /* total: 144+288+16+34+384+574 = 1440B ✓ */
} FrustumMeta;

/* ── Canonical container (4896B) ────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t     data[DGLS_DATA_BYTES];  /* 3456B, offset=0    */
    FrustumMeta meta;                   /* 1440B, offset=3456 */
} FrustumBlock;                         /* sizeof = 4896B ✓   */

/* Header access — zero-cost pointer cast */
static inline FrustumHeader *frustum_header(FrustumBlock *b) {
    return (FrustumHeader *)&b->meta.reserved[0];
}

static inline const FrustumHeader *frustum_header_c(const FrustumBlock *b) {
    return (const FrustumHeader *)&b->meta.reserved[0];
}

/* ── Compile-time assertions ────────────────────────────── */
_Static_assert(sizeof(FrustumHeader) == 17,   "header must be 17B");
_Static_assert(sizeof(FrustumMeta)   == 1440, "meta must be 1440B");
_Static_assert(sizeof(FrustumBlock)  == 4896, "block must be 4896B");
_Static_assert(offsetof(FrustumBlock, meta) == 3456, "meta offset must be 3456");
