/*
 * geofield_header.h — GeoField Virtual Format Header (24B fixed)
 * ══════════════════════════════════════════════════════════════
 *
 * Header = everything. Data = virtual, reconstructible from seed + geometry.
 *
 * Geometric constants (compile-time, NOT stored in header):
 *
 *   GEOF_CONVERGENCE = 12
 *     → 12 = GCD(hex_angle=60°, pent_angle=72°)
 *     → 12 = pentagon count (Euler invariant)
 *     → 12¹=12  12²=144  12³=1728  12⁴=20736=144²=128×162=2⁸×3⁴
 *     → universal alignment point, generator of full address space
 *
 *   GEOF_HP_RATIO = 3
 *     → Hilbert:Peano = 9:3 = 3:1
 *     → 9 real lines + 3 Peano overlay = 12 total (convergence)
 *     → every 12 steps = 1 positive invert event
 *
 *   GEOF_TICKS_ROUND = 1440
 *     → fibo clock period = 2 × 720 = 2⁵ × 3² × 5
 *     → 1 tick = 1 floor level
 *
 * Shape family (angular):
 *   SHAPE_HEX_FAMILY  = 0  → 60°  (tetra, octa, rubik)      12×5
 *   SHAPE_PENT_FAMILY = 1  → 72°  (cylinder×5, icosa)        12×6
 *   SHAPE_HYBRID      = 2  → both (goldberg sphere)
 *
 * Codec id maps to hex_tile variant:
 *   CODEC_FLAT=0, CODEC_TRIPLET=1, CODEC_GRADIENT=2, CODEC_EDGE=3
 *
 * Header size: 24B = 3×8 = 3 DiamondWords
 *   → fits in single 64B cache line with 40B context remaining
 *   → digit-sum(24) = 6 → ×9 = 54 = NEXUS
 *
 * No float. No heap. No global state.
 * ══════════════════════════════════════════════════════════════
 */
#pragma once
#include <stdint.h>
#include <string.h>

/* ── Compile-time geometric constants (never stored) ────────── */

#define GEOF_CONVERGENCE    12u     /* GCD(60°,72°) = pentagon count    */
#define GEOF_HP_RATIO        3u     /* Hilbert:Peano line ratio          */
#define GEOF_TICKS_ROUND  1440u     /* fibo clock period (2⁵×3²×5)      */
#define GEOF_ALIGN_POINT 20736u     /* 12⁴ = 144² = 2⁸×3⁴               */

/* Angular family — shape_id upper nibble */
#define GEOF_SHAPE_HEX_FAMILY   0u  /* 60° — tetra / octa / rubik        */
#define GEOF_SHAPE_PENT_FAMILY  1u  /* 72° — cylinder×5 / icosa          */
#define GEOF_SHAPE_HYBRID       2u  /* both — goldberg sphere             */

/* Shape id: upper nibble = family, lower nibble = topology
 * family: 0=HEX(60°)  1=PENT(72°)  2=HYBRID
 * topo:   0=tetra  1=octa/cylinder  2=rubik  3=goldberg  */
#define GEOF_SHAPE_TETRA        0x00u  /* HEX  family, tetra tring       */
#define GEOF_SHAPE_OCTA         0x01u  /* HEX  family, octa O-ring       */
#define GEOF_SHAPE_RUBIK        0x02u  /* HEX  family, 6×9 Rubik (54)    */
#define GEOF_SHAPE_CYLINDER     0x11u  /* PENT family, 12-face cylinder  */
#define GEOF_SHAPE_GOLDBERG     0x21u  /* HYBRID, goldberg sphere        */

/* Codec id */
#define GEOF_CODEC_FLAT         0u
#define GEOF_CODEC_TRIPLET      1u
#define GEOF_CODEC_GRADIENT     2u
#define GEOF_CODEC_EDGE         3u

/* Magic bytes "GEOF" */
#define GEOF_MAGIC  0x47454F46u     /* 'G','E','O','F'                   */
#define GEOF_VERSION        1u

/* ── GeoFieldHeader — 24B fixed ─────────────────────────────── */
/*
 * Byte layout (24B total):
 *   [0..3]   magic        "GEOF"
 *   [4]      version      format version
 *   [5]      shape_id     upper nibble=family, lower nibble=topology
 *   [6]      codec_id     FLAT/TRIPLET/GRADIENT/EDGE
 *   [7]      flags        bit0=lossless, bit1=chord_active, bit2-7=rsvd
 *   [8..9]   floor_count  number of floors (ticks used)
 *   [10..11] pad_align    reserved — align seed to 4B boundary
 *   [12..15] seed         reconstruct seed (primary key)
 *   [16]     chord_root   reserved (future: base chord address)
 *   [17]     chord_flags  reserved (future: #/b/maj/min offset)
 *   [18..19] pad_chord    reserved — chord expansion
 *   [20..23] digest       CRC32 of bytes [0..19]
 */
typedef struct {
    uint32_t magic;         /* 0x47454F46 = "GEOF"                       */
    uint8_t  version;       /* GEOF_VERSION                              */
    uint8_t  shape_id;      /* GEOF_SHAPE_* — angular family + topology  */
    uint8_t  codec_id;      /* GEOF_CODEC_*                              */
    uint8_t  flags;         /* bit0=lossless, bit1=chord_active          */
    uint16_t floor_count;   /* floors used (≤ GEOF_TICKS_ROUND=1440)     */
    uint16_t pad_align;     /* reserved — keep seed at offset 12 (4B)    */
    uint32_t seed;          /* reconstruct seed — primary key            */
    uint8_t  chord_root;    /* reserved — future chord base address      */
    uint8_t  chord_flags;   /* reserved — future #/b/maj/min             */
    uint16_t pad_chord;     /* reserved — chord expansion                */
    uint8_t  digest[4];     /* CRC32 of header bytes [0..19]             */
} GeoFieldHeader;           /* sizeof = 24B ✓                            */

/* ── Flags bits ─────────────────────────────────────────────── */
#define GEOF_FLAG_LOSSLESS      (1u << 0)
#define GEOF_FLAG_CHORD_ACTIVE  (1u << 1)

/* ── Shape family extract ───────────────────────────────────── */
static inline uint8_t geof_shape_family(uint8_t shape_id) {
    return (shape_id >> 4) & 0x0Fu;
}
static inline uint8_t geof_shape_topo(uint8_t shape_id) {
    return shape_id & 0x0Fu;
}

/* Angular step for shape family (degrees × 10 for integer math) */
static inline uint16_t geof_angular_step(uint8_t shape_id) {
    uint8_t fam = geof_shape_family(shape_id);
    if (fam == GEOF_SHAPE_HEX_FAMILY)  return 600u;  /* 60.0° × 10 */
    if (fam == GEOF_SHAPE_PENT_FAMILY) return 720u;  /* 72.0° × 10 */
    return 0u; /* hybrid — caller handles both */
}

/* ── CRC32 (simple — no table, header is only 20B) ──────────── */
static inline uint32_t geof_crc32_small(const uint8_t *buf, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ── Init / validate ────────────────────────────────────────── */

/*
 * geof_header_init — populate header, compute digest
 * floor_count must be ≤ GEOF_TICKS_ROUND (1440)
 */
static inline void geof_header_init(GeoFieldHeader *h,
                                     uint8_t  shape_id,
                                     uint8_t  codec_id,
                                     uint8_t  flags,
                                     uint16_t floor_count,
                                     uint32_t seed)
{
    memset(h, 0, sizeof(*h));
    h->magic       = GEOF_MAGIC;
    h->version     = GEOF_VERSION;
    h->shape_id    = shape_id;
    h->codec_id    = codec_id;
    h->flags       = flags;
    h->floor_count = (floor_count > GEOF_TICKS_ROUND)
                     ? (uint16_t)GEOF_TICKS_ROUND : floor_count;
    h->seed        = seed;
    /* digest covers bytes 0..19 */
    uint32_t crc = geof_crc32_small((const uint8_t*)h, 20u);
    h->digest[0]   = (uint8_t)(crc);
    h->digest[1]   = (uint8_t)(crc >> 8);
    h->digest[2]   = (uint8_t)(crc >> 16);
    h->digest[3]   = (uint8_t)(crc >> 24);
}

/*
 * geof_header_valid — check magic, version, digest
 * Returns 1 if valid, 0 if corrupt/unknown.
 */
static inline int geof_header_valid(const GeoFieldHeader *h) {
    if (h->magic   != GEOF_MAGIC)   return 0;
    if (h->version != GEOF_VERSION) return 0;
    /* recompute digest over bytes 0..19 */
    GeoFieldHeader tmp;
    memcpy(&tmp, h, sizeof(tmp));
    memset(tmp.digest, 0, 4);
    uint32_t crc = geof_crc32_small((const uint8_t*)&tmp, 20u);
    uint32_t stored = (uint32_t)h->digest[0]
                    | ((uint32_t)h->digest[1] << 8)
                    | ((uint32_t)h->digest[2] << 16)
                    | ((uint32_t)h->digest[3] << 24);
    return (crc == stored) ? 1 : 0;
}

/*
 * geof_header_seed_space — total virtual address space for this header
 * = floor_count × GEOF_TICKS_ROUND ticks mapped into 12⁴ = 20736 space
 * Returns position count (≤ GEOF_ALIGN_POINT).
 */
static inline uint32_t geof_header_seed_space(const GeoFieldHeader *h) {
    uint32_t pos = (uint32_t)h->floor_count * GEOF_CONVERGENCE;
    return (pos > GEOF_ALIGN_POINT) ? GEOF_ALIGN_POINT : pos;
}
