/*
 * geo_compound_cfg.h — Modular Compound Geometry Configuration
 * ═══════════════════════════════════════════════════════════════
 *
 * Two compound layers sharing one parameterized engine:
 *
 *   5-TETRA (FROZEN core):
 *     12 × compound-of-5-tetrahedra
 *     face = triangle  →  temporal walk, sacred numbers
 *     720 slots, 3456 address space, 6912 residual zone
 *
 *   5-OCTA (frustum layer):
 *     12 × compound-of-5-octahedra
 *     face dual = cube (square)  →  frustum-friendly, no tri→quad convert
 *     maps INTO 6912 residual zone — does NOT touch sacred numbers
 *
 * Junction point: 6912 = 2⁸×3³
 *   5-tetra path:  3456 × 2       = 6912  (residual zone ceiling)
 *   5-octa path:   1536 × 4.5     = 6912  (frustum zone ceiling)
 *                  768  × 9       = 6912  (base unit × 3²)
 *
 * Usage (select layer at compile time or runtime):
 *   #define GEO_COMPOUND_USE_TETRA   → tetra config active
 *   #define GEO_COMPOUND_USE_OCTA    → octa  config active
 *   Or: pass GeoCompoundCfg* at runtime for both simultaneously
 *
 * No malloc. No float. No pointers between layers.
 * Sacred numbers: FROZEN. 6912: JUNCTION. Do not touch core.
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_COMPOUND_CFG_H
#define GEO_COMPOUND_CFG_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════
   COMPOUND TYPE TAG
   ══════════════════════════════════════════════════════════════ */
typedef enum {
    GEO_COMPOUND_TETRA = 0,   /* 5-tetra × 12, triangle face, temporal */
    GEO_COMPOUND_OCTA  = 1,   /* 5-octa  × 12, square dual, spatial/frustum */
} GeoCompoundType;

/* ══════════════════════════════════════════════════════════════
   FACE BASE TYPE
   (determines frustum compatibility)
   ══════════════════════════════════════════════════════════════ */
typedef enum {
    GEO_FACE_TRIANGLE = 0,   /* tetra: needs tri→quad conversion for frustum */
    GEO_FACE_SQUARE   = 1,   /* octa dual (cube): frustum-native, no convert */
} GeoFaceBase;

/* ══════════════════════════════════════════════════════════════
   MODULAR CONFIG STRUCT
   All parameters derived from compound type — no magic numbers
   inline in code.
   ══════════════════════════════════════════════════════════════ */
typedef struct {
    GeoCompoundType type;

    /* geometry */
    uint32_t compounds;        /* 12 for both */
    uint32_t faces_per_comp;   /* tetra=4, octa=8 */
    uint32_t face_slots;       /* slots per face (temporal granularity) */
    uint32_t total_slots;      /* compounds × faces_per_comp × ... */

    /* address space */
    uint32_t addr_base;        /* start of this layer's zone */
    uint32_t addr_range;       /* size of address space */
    uint32_t junction;         /* 6912 for both — meeting point */

    /* spoke / lane geometry */
    uint32_t spokes;           /* lanes per cycle */
    uint32_t slots_per_spoke;  /* addr_range / spokes */

    /* frustum */
    GeoFaceBase face_base;
    uint32_t    frustum_divisor; /* tetra=2 (needs split), octa=1 (native) */

    /* sacred number family: 2^a × 3^b */
    uint8_t  pow2;
    uint8_t  pow3;
} GeoCompoundCfg;

/* ══════════════════════════════════════════════════════════════
   FROZEN CONFIGS (compile-time constants)
   ══════════════════════════════════════════════════════════════ */

/*
 * 5-TETRA CONFIG (sacred — do not change)
 *
 *   compounds=12, faces=4×5=20 per comp, 60 slots/face
 *   720 total slots (12×60), addr=0..3455, residual=3456..6911
 *   spokes=6, 120 slots/spoke
 *   face_base=TRIANGLE → frustum needs ×2 conversion
 *   6912 = 3456 × 2 = residual ceiling
 *   2⁷ × 3³ = 3456
 */
static const GeoCompoundCfg GEO_CFG_TETRA = {
    .type            = GEO_COMPOUND_TETRA,
    .compounds       = 12u,
    .faces_per_comp  = 20u,      /* 5 tetra × 4 faces each */
    .face_slots      = 60u,      /* 720 / 12 */
    .total_slots     = 720u,     /* FROZEN: GAN_TRING_CYCLE */
    .addr_base       = 0u,
    .addr_range      = 3456u,    /* FROZEN: GEO_FULL_N = 2⁷×3³ */
    .junction        = 6912u,    /* 3456 × 2 */
    .spokes          = 6u,
    .slots_per_spoke = 120u,     /* 720 / 6 */
    .face_base       = GEO_FACE_TRIANGLE,
    .frustum_divisor = 2u,       /* tri→quad: split each face in 2 */
    .pow2            = 7u,
    .pow3            = 3u,
};

/*
 * 5-OCTA CONFIG (new frustum layer)
 *
 *   compounds=12, faces=8×5=40 per comp, octa dual=cube (square face)
 *   1536 = 2⁹×3 total slots, addr=3456..6911 (inside residual zone)
 *   spokes=6, 256 slots/spoke
 *   face_base=SQUARE → frustum-native (no conversion)
 *   6912 = 1536 × 4.5 = 768 × 9 = junction
 *   2⁸ × 3³ = 6912 (shared invariant)
 *
 *   NOTE: addr_base=3456 — lives INSIDE tetra residual zone
 *         does not touch tetra sacred range 0..3455
 */
static const GeoCompoundCfg GEO_CFG_OCTA = {
    .type            = GEO_COMPOUND_OCTA,
    .compounds       = 12u,
    .faces_per_comp  = 40u,      /* 5 octa × 8 faces each */
    .face_slots      = 128u,     /* 1536 / 12 */
    .total_slots     = 1536u,    /* 2⁹ × 3 */
    .addr_base       = 3456u,    /* starts at tetra ceiling — residual zone */
    .addr_range      = 3456u,    /* same range, different zone */
    .junction        = 6912u,    /* 3456 + 3456 = ceiling = tetra junction */
    .spokes          = 6u,
    .slots_per_spoke = 256u,     /* 1536 / 6 */
    .face_base       = GEO_FACE_SQUARE,
    .frustum_divisor = 1u,       /* square base: native, no split */
    .pow2            = 8u,
    .pow3            = 3u,       /* 6912 = 2⁸ × 3³ shared with junction */
};

/* ══════════════════════════════════════════════════════════════
   MODULAR ADDRESS ENGINE
   All derived from cfg — same code, different numbers
   ══════════════════════════════════════════════════════════════ */

/* spoke index from absolute address */
static inline uint32_t geo_cfg_spoke(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local / c->slots_per_spoke;
}

/* face index (0..compounds-1) from local position */
static inline uint32_t geo_cfg_face(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local / c->face_slots;
}

/* local slot within face */
static inline uint32_t geo_cfg_slot_in_face(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local % c->face_slots;
}

/* polarity: 50/50 split — same rule for both compounds */
static inline uint8_t geo_cfg_polarity(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    uint32_t pentagon = local % c->face_slots;
    return (uint8_t)((pentagon % (c->face_slots / 2)) >= (c->face_slots / 4));
}

/* hilbert bucket — same formula, different divisor */
static inline uint32_t geo_cfg_hilbert(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local / c->spokes;   /* total_slots/spokes = hilbert buckets */
}

/* frustum scale factor: how many addr units per frustum quad */
static inline uint32_t geo_cfg_frustum_unit(const GeoCompoundCfg *c)
{
    return c->face_slots * c->frustum_divisor;
}

/* ══════════════════════════════════════════════════════════════
   GEOFACE ROUTER — O(1) face translation between layers
   Translates face_id from one compound geometry to another
   using the shared junction invariant (6912 = 2⁸×3³)
   ══════════════════════════════════════════════════════════════ */

/*
 * face_route: map src face_id → dst face_id
 *
 * Both share compounds=12 → face_id % 12 is universal key
 * Scale by face_slots ratio to get destination slot
 *
 * TETRA → OCTA: face_id × (octa.face_slots / tetra.face_slots)
 *               = face_id × (128 / 60) ≈ face_id × 2 (integer approx)
 *               exact: use cross-product through junction
 * OCTA  → TETRA: inverse
 */
static inline uint32_t geo_face_route(
    const GeoCompoundCfg *src,
    const GeoCompoundCfg *dst,
    uint32_t src_face_id)
{
    /* both have 12 compounds → modulo is universal */
    uint32_t face_mod = src_face_id % src->compounds;

    /* scale through junction: src_face × (dst_slots / src_slots)
     * integer exact via junction: multiply by dst, divide by src */
    uint32_t dst_face = (face_mod * dst->total_slots) / src->total_slots;

    /* clamp to dst face range */
    return dst_face % dst->compounds;
}

/* absolute address translation: tetra addr → octa addr (or reverse) */
static inline uint64_t geo_addr_translate(
    const GeoCompoundCfg *src,
    const GeoCompoundCfg *dst,
    uint64_t src_addr)
{
    /* normalize to [0, src.addr_range) */
    uint32_t local = (uint32_t)((src_addr - src->addr_base) % src->addr_range);

    /* scale proportionally through shared junction */
    uint64_t scaled = ((uint64_t)local * dst->addr_range) / src->addr_range;

    return dst->addr_base + (scaled % dst->addr_range);
}

/* ══════════════════════════════════════════════════════════════
   JUNCTION VERIFY — both configs must resolve to 6912
   Call once at init to validate config integrity
   ══════════════════════════════════════════════════════════════ */
static inline int geo_compound_cfg_verify(const GeoCompoundCfg *c)
{
    /* addr_base + addr_range must not exceed junction */
    if (c->addr_base + c->addr_range > c->junction) return -1;

    /* total_slots × spokes sanity */
    if (c->total_slots != c->spokes * c->slots_per_spoke) return -1;

    /* junction must be 2^pow2 × 3^pow3 */
    uint32_t chk = 1u;
    for (uint8_t i = 0; i < c->pow2; i++) chk *= 2u;
    for (uint8_t i = 0; i < c->pow3; i++) chk *= 3u;
    if (chk != c->junction) return -1;

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   CONVENIENCE: default cfg pointer by type
   ══════════════════════════════════════════════════════════════ */
static inline const GeoCompoundCfg *geo_compound_cfg(GeoCompoundType t)
{
    return (t == GEO_COMPOUND_OCTA) ? &GEO_CFG_OCTA : &GEO_CFG_TETRA;
}

#endif /* GEO_COMPOUND_CFG_H */
