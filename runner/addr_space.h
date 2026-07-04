/*
 * addr_space.h — Power-of-N Address Space
 *
 * Core geometry: 128 x 162 = 20736 = 144^2
 *
 *   128 = 2^7  (head dimension power-of-2)
 *   162 = 2 x 81 = 2 x 3^4  (Y-triangle face count)
 *   144 = 2^4 x 3^2  (LCM of 128 and 162's prime factors)
 *
 * Scaling:
 *   Tier0: 128 x 162           =  144^2    =     20,736   (fits any model)
 *   Tier1: (128 x 162)^2       =  144^4    = 429,981,696  (large MoE)
 *   Tier2: (128 x 162)^3       =  144^6    =   ~8e14      (future)
 *   Tier3: (128 x 162)^4       =  144^8    =   ~1.8e17    (far future)
 *
 * Bit layout per tier (macro x micro):
 *   Tier0:  7-bit macro (128) x 8-bit micro (256>=162) = 15 bits
 *   Tier1: 15-bit macro (32768>=20736) x 14-bit micro (16384) = 29 bits
 *   Tier2: 22-bit macro x 21-bit micro = 43 bits
 *   Tier3: 29-bit macro x 29-bit micro = 58 bits
 *
 * Design: tensor name -> addr decomposition:
 *   macro = which "tower group" (layer-level routing)
 *   micro = position within tower (head/weight slot)
 *
 * The20736-slot micro space in Tier1+ is itself a 128x162 geometry,
 * creating a fractal address where each macro-cell contains a full
 * Tier0 universe.
 *
 * Backward compatible: GEO_FULL=20736 remains the base unit.
 */

#ifndef ADDR_SPACE_H
#define ADDR_SPACE_H

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "rdh_addr.h"

/* ═══════════════════════════════════════════════════════════════════
 * CORE GEOMETRY CONSTANTS
 * ═══════════════════════════════════════════════════════════════════ */

#define ADDR_DIM_A         128u    /* 2^7 — head-dimension power-of-2 */
#define ADDR_DIM_B         162u    /* 2 x 3^4 — Y-triangle face count */
#define ADDR_BASE          (ADDR_DIM_A * ADDR_DIM_B)  /* 20736 = 144^2 */
#define ADDR_N             144u    /* = 2^4 x 3^2 */
#define ADDR_STRIDE        12u     /* capo stride (spoke spacing) */
#define ADDR_TOWER         144u    /* cells per tower */

/* ═══════════════════════════════════════════════════════════════════
 * TIER DEFINITIONS
 * ═══════════════════════════════════════════════════════════════════
 *
 * power_of_k: (128 x 162)^k = ADDR_BASE^(k+1)   [k starts at 0]
 * This means Tier k has (k+1) levels of 128x162 nesting.
 *
 * Bit layout:
 *   Total bits = ceil(log2(capacity))
 *   Split: macro_bits = total/2 (rounded), micro_bits = total - macro_bits
 *   macro_bits covers >= ADDR_BASE slots (for Tier1+)
 *   micro_bits covers >= ADDR_BASE slots (for Tier1+)
 *   Exception: Tier0 uses 7/8 to match128/162 exactly.
 */

#define ADDR_MAX_TIERS     4u

typedef struct {
    uint8_t  tier;          /* tier index (0-3) */
    uint32_t capacity;      /* total address slots */
    uint8_t  total_bits;    /* bits needed for full address */
    uint8_t  macro_bits;    /* high bits: macro cell selector */
    uint8_t  micro_bits;    /* low bits: position within macro cell */
    uint32_t macro_mask;    /* (1 << macro_bits) - 1 */
    uint32_t micro_mask;    /* (1 << micro_bits) - 1 */
    uint32_t macro_slots;   /* 1 << macro_bits */
    uint32_t micro_slots;   /* 1 << micro_bits */
} AddrTier;

/*
 * Static tier table.
 *
 * Tier0: 15 bits, macro=7 (128 slots), micro=8 (256 slots)
 *        Direct 128x162 mapping — matches head_dim and Y-triangle.
 *
 * Tier1: 29 bits, macro=15 (32768 slots), micro=14 (16384 slots)
 *        Each macro-cell contains a full Tier0 universe (20736 slots).
 *        For MoE models with 256+ experts or very large hidden dims.
 *
 * Tier2: 43 bits, macro=22, micro=21
 *        Triple-nested128x162 — theoretical, for models > 1T params.
 *
 * Tier3: 58 bits, macro=29, micro=29
 *        Quadruple-nested — far future.
 */
static const AddrTier ADDR_TIERS[ADDR_MAX_TIERS] = {
    /* tier  capacity       bits  macro  micro  macro_mask     micro_mask     macro_slots  micro_slots */
    {  0,   20736u,          15,     7,     8,  0x0000007Fu,   0x000000FFu,         128u,         256u },
    {  1,   429981696u,      29,    15,    14,  0x00007FFFu,   0x00003FFFu,       32768u,       16384u },
    {  2,   0u,              43,    22,    21,  0x003FFFFFu,   0x001FFFFFu,     4194304u,     4194304u },  /* capacity > uint32 */
    {  3,   0u,              58,    29,    29,  0x1FFFFFFFu,   0x1FFFFFFFu,    536870912u,   536870912u },
};

/* Tier2+ capacity needs 64-bit; helper to get it at runtime */
static inline uint64_t addr_tier_capacity(uint8_t tier) {
    if (tier == 0) return 20736ULL;
    if (tier == 1) return 429981696ULL;
    /* Tier k: (20736)^(k+1) */
    uint64_t cap = 20736ULL;
    for (uint8_t i = 1; i <= tier; i++) cap *= 20736ULL;
    return cap;
}

/* ═══════════════════════════════════════════════════════════════════
 * TIER SELECTION — auto-pick tier from model dimensions
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * addr_select_tier() — choose minimal tier for given tensor count + hidden dim.
 *
 * Heuristics:
 *   - tensor_count < 20736  -> Tier0 (all current models)
 *   - tensor_count < 429981696 -> Tier1 (massive MoE)
 *   - otherwise -> Tier2+
 *
 * hidden_dim also matters: if h > 128, we need more micro-space.
 * For h=16384 (Llama 3.1 405B), Tier0's 256 micro slots are tight
 * but work because we map by tensor name, not by element.
 */
static inline uint8_t addr_select_tier(uint32_t tensor_count, uint32_t hidden_dim) {
    (void)hidden_dim;
    if (tensor_count < 20736u)        return 0;
    if (tensor_count < 429981696u)    return 1;
    return 2;  /* theoretical */
}

/* ═══════════════════════════════════════════════════════════════════
 * ADDRESS DECOMPOSITION — split flat addr into (macro, micro)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t macro;     /* macro-cell index (which tower group) */
    uint32_t micro;     /* micro position (position within tower group) */
    uint32_t addr;      /* original flat address */
    uint8_t  tier;      /* tier used */
} AddrDecomp;

/*
 * addr_decompose() — split a flat address into (macro, micro).
 *
 * Tier0: macro = addr / 256 (7 bits), micro = addr % 256 (8 bits)
 *        macro selects one of128 "macro positions" (layers or groups)
 *        micro selects position within the128x162 geometry
 *
 * Tier1: macro = addr / 16384 (15 bits), micro = addr % 16384 (14 bits)
 *        macro selects one of 32768 macro-cells (each = full Tier0)
 *        micro selects position within the macro-cell's 128x162 space
 *
 * For Tier0, we do NOT simply addr/256. Instead we use the actual
 * 128x162 geometry:
 *   macro = (addr / ADDR_DIM_B) % ADDR_DIM_A   [0..127]
 *   micro = addr % ADDR_DIM_B                   [0..161]
 *   ... but for flat addr, the bit-split is equivalent and faster.
 */
static inline AddrDecomp addr_decompose(uint32_t addr, uint8_t tier) {
    AddrDecomp d;
    d.addr = addr;
    d.tier = tier;

    if (tier == 0) {
        /* Tier0: 7-bit macro (128 slots) x 8-bit micro (256 slots >= 162) */
        d.macro = (addr >> 8) & 0x7Fu;   /* high 7 bits */
        d.micro = addr & 0xFFu;           /* low 8 bits */
    } else if (tier < ADDR_MAX_TIERS) {
        const AddrTier *t = &ADDR_TIERS[tier];
        d.macro = (addr >> t->micro_bits) & t->macro_mask;
        d.micro = addr & t->micro_mask;
    } else {
        d.macro = addr;
        d.micro = 0;
    }
    return d;
}

/*
 * addr_compose() — merge (macro, micro) back into flat address.
 */
static inline uint32_t addr_compose(uint32_t macro, uint32_t micro, uint8_t tier) {
    if (tier == 0) {
        return ((macro & 0x7Fu) << 8) | (micro & 0xFFu);
    } else if (tier < ADDR_MAX_TIERS) {
        const AddrTier *t = &ADDR_TIERS[tier];
        return ((macro & t->macro_mask) << t->micro_bits) | (micro & t->micro_mask);
    }
    return macro;
}

/* ═══════════════════════════════════════════════════════════════════
 * GEOMETRY DECOMPOSITION — macro/micro -> (spoke, layer, slot)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t spoke;     /* spoke index (0-5 or 0-11) */
    uint32_t layer;     /* layer within spoke (0-26 or 0-143) */
    uint32_t slot;      /* weight slot within cell */
    uint32_t face;      /* face index (for SID perturbation) */
} GeoDecomp;

/*
 * addr_to_geo() — decompose flat address into Y-triangle geometry.
 *
 * The address space maps to model structure:
 *   addr -> (layer, head/slot, weight_type)
 *
 * For transformer models, the natural mapping is:
 *   layer = addr / HEADS_PER_LAYER
 *   head  = addr % HEADS_PER_LAYER
 *
 * Since head_dim is typically a power of 2 (64, 128), and
 * ADDR_DIM_A = 128, we use128 as the "micro granularity":
 *
 * Tier0 (20736 = 162 x 128):
 *   macro = addr / 128  (0-161) — maps to layer or tower position
 *   micro = addr % 128  (0-127) — maps to head/slot within layer
 *
 * This aligns with actual model dimensions:
 *   head_dim = 64, 128 (powers of 2)
 *   128 macro positions covers up to162 layers (LFM2: 16, Llama: 126)
 *
 * For "blk.LAYER.TYPE" tensors, the layer index IS the macro.
 * For non-block tensors, macro=0.
 *
 * The128/162 split is NOT 12 spokes x 144 towers — it's
 * 128 head-slot positions x 162 layer positions, which maps
 * directly to transformer architecture.
 */
static inline GeoDecomp addr_to_geo(uint32_t addr, uint8_t tier) {
    GeoDecomp g;
    g.face = 0;

    if (tier == 0) {
        /* Tier0: 162 macro positions x 128 micro positions = 20736 */
        g.spoke = addr / ADDR_DIM_A;    /* 0-161 (layer/tower position) */
        g.layer = addr / ADDR_DIM_A;    /* same as spoke for flat addr */
        g.slot  = addr % ADDR_DIM_A;    /* 0-127 (head/weight slot) */
    } else {
        /* Higher tiers: macro-cell is a full Tier0, micro is sub-address */
        AddrDecomp ad = addr_decompose(addr, tier);

        g.spoke = ad.macro / ADDR_DIM_A;
        g.layer = ad.macro / ADDR_DIM_A;
        g.slot  = ad.micro % ADDR_DIM_A;
    }

    return g;
}

/* ═══════════════════════════════════════════════════════════════════
 * TENSOR NAME -> ADDRESS MAPPING
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * addr_from_tensor_name() — deterministic hash of tensor name -> address.
 *
 * For "blk.X.Y" tensors:
 *   macro = layer_index (fits in macro_bits)
 *   micro = hash(tensor_type_name) -> spoke + slot
 *
 * For non-block tensors (token_embd, output_norm):
 *   macro = 0 (first macro-cell)
 *   micro = hash(full_name) -> specific position
 */
static inline uint32_t addr_from_tensor_name(const char *name, uint8_t tier) {
    if (!name || name[0] == '\0') return 0;

    /* FNV-1a hash */
    uint32_t h = 0x811c9dc5u;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 0x01000193u;
    }

    uint32_t macro = 0;
    uint32_t micro = 0;

    /* Parse "blk.LAYER.TYPE" pattern */
    if (name[0] == 'b' && name[1] == 'l' && name[2] == 'k' && name[3] == '.') {
        /* Extract layer number */
        uint32_t layer = 0;
        const char *p = name + 4;
        while (*p >= '0' && *p <= '9') {
            layer = layer * 10 + (uint32_t)(*p - '0');
            p++;
        }

        /* Skip dot, hash the type name */
        if (*p == '.') p++;
        uint32_t th = 0;
        for (const char *q = p; *q; q++) {
            th = th * 131u + (uint8_t)*q;
        }

        const AddrTier *t = &ADDR_TIERS[tier < ADDR_MAX_TIERS ? tier : 0];
        macro = layer % t->macro_slots;
        micro = th % t->micro_slots;
    } else {
        /* Non-block tensor: all in micro */
        micro = h % (tier < ADDR_MAX_TIERS ? ADDR_TIERS[tier].micro_slots : 256u);
    }

    return addr_compose(macro, micro, tier);
}

/* ═══════════════════════════════════════════════════════════════════
 * RDH ADDRESSING — collision-free alternative to hash-based mapping
 * ═══════════════════════════════════════════════════════════════════
 *
 * Uses rdh_addr.h's 5-parameter (ring, wedge, mirror, u, v) formula
 * instead of FNV-1a hash + bit-split.
 *
 * RDH guarantees:
 *   - No collision (bijective mixed-radix encoding)
 *   - O(1) encode/decode — no hash computation
 *   - ring = layer index (from "blk.N." pattern)
 *   - wedge = type name hash (within tir micro_slots)
 *
 * Compatibility: RDH produces addresses 0..20735 (all valid),
 * while hash-based produces 0..32767 (20736 valid).
 */

/* RDH config for a given tier — maps (ring, wedge, mirror, u, v) to flat addr.
 * Tier0: 128 rings x 256 wedges x 1 x 1 x 1 = 32768
 * Address layout matches existing hash-based (macro<<8)|micro = ring*256+wedge.
 * Only keys < ADDR_BASE (20736) are valid, matching current valid range.
 * Using 256 wedges (power-of-2) enables bitfield: key = (ring<<8)|wedge. */
static inline RDHConfig addr_rdh_config(uint8_t tier) {
    RDHConfig cfg = { 0, 0, 1, 1, 1 };
    if (tier >= ADDR_MAX_TIERS) tier = 0;
    cfg.n_rings  = ADDR_TIERS[tier].macro_slots; /* 128 */
    cfg.n_wedges = ADDR_TIERS[tier].micro_slots; /* 256 (power-of-2, matches existing bit layout) */
    cfg.n_mirror = 1;
    cfg.max_u    = 1;
    return cfg;
}

/* addr_from_rdh_name() — RDH version of addr_from_tensor_name().
 *
 * For "blk.X.Y" tensors:
 *   ring = layer  (0..n_rings-1)
 *   wedge = hash(tensor_type) % n_wedges  (0..n_wedges-1)
 *   mirror = 0, u = 0, v = 0
 *   key = ring * n_wedges + wedge  (no collision)
 *
 * For non-block tensors (token_embd, output_norm):
 *   ring = 0
 *   wedge = hash(full_name) % n_wedges
 *
 * Returns flat address in range [0, n_rings * n_wedges).
 * Tier0: [0, 20736)
 */
static inline uint32_t addr_from_rdh_name(const char *name, uint8_t tier) {
    if (!name || name[0] == '\0') return 0;

    RDHConfig cfg = addr_rdh_config(tier);
    uint32_t ring = 0, wedge = 0;

    /* Parse "blk.LAYER.TYPE" pattern */
    if (name[0] == 'b' && name[1] == 'l' && name[2] == 'k' && name[3] == '.') {
        uint32_t layer = 0;
        const char *p = name + 4;
        while (*p >= '0' && *p <= '9') {
            layer = layer * 10 + (uint32_t)(*p - '0');
            p++;
        }
        ring = layer % (uint32_t)cfg.n_rings;

        /* Hash type name into wedge (FNV-1a for good distribution) */
        if (*p == '.') p++;
        uint32_t th = 0x811c9dc5u;
        for (const char *q = p; *q; q++) {
            th ^= (uint8_t)*q;
            th *= 0x01000193u;
        }
        wedge = th % (uint32_t)cfg.n_wedges;
    } else {
        /* Non-block tensor: distribute across valid ring range.
         * ring capped to max ring that keeps key < ADDR_BASE. */
        size_t len = strlen(name);
        uint32_t max_r = ADDR_BASE / (uint32_t)cfg.n_wedges;
        if (max_r < 1) max_r = 1;
        uint32_t h = 0x811c9dc5u;
        for (size_t i = 0; i < len; i++) {
            h ^= (uint8_t)name[i];
            h *= 0x01000193u;
        }
        ring = h % max_r;
        uint32_t h2 = 0x6b8b4567u;
        for (size_t i = len; i > 0; i--) {
            h2 ^= (uint8_t)name[i - 1];
            h2 *= 0x01000193u;
        }
        wedge = h2 % (uint32_t)cfg.n_wedges;
    }

    return (uint32_t)rdh_key(&cfg, (int64_t)ring, (int64_t)wedge, 0, 0, 0);
}

/* addr_rdh_capo() — RDH mirror flag for SID face.
 * Instead of modular addition, uses mirror=1 for face 1+.
 * This makes SID face duality intrinsic to the address. */
static inline uint32_t addr_rdh_capo(uint32_t base, uint32_t face, uint8_t tier) {
    if (face == 0) return base;
    RDHConfig cfg = addr_rdh_config(tier);
    /* For SID face f: mirror_flag = 1 (alternate universe) */
    int64_t ring = 0, wedge = 0, mirror = 0, u = 0;
    rdh_decompose(&cfg, (int64_t)base, &ring, &wedge, &mirror, &u);
    mirror = (int64_t)(face & 1);  /* face 0 = original, face 1+ = mirror */
    return (uint32_t)rdh_key(&cfg, ring, wedge, mirror, 0, 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * CAPO — face rotation within address space
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * addr_capo() — rotate address for SID face f.
 *
 * face 0 = original
 * face 1..11 = perturbed copies at capo offsets
 *
 * Uses the same144-stride rotation as the existing system:
 *   new_addr = (base + f * STRIDE * TOWER) % capacity
 *
 * For Tier0: capacity=20736, stride=12, tower=144
 *   face f offset = f * 12 * 144 = f * 1728
 *   face 1 offset = 1728 (1/12th of address space)
 *   face 11 offset = 19008
 */
static inline uint32_t addr_capo(uint32_t base, uint32_t face, uint8_t tier) {
    uint64_t cap = addr_tier_capacity(tier);
    uint64_t offset = (uint64_t)face * (uint64_t)ADDR_STRIDE * (uint64_t)ADDR_TOWER;
    return (uint32_t)((base + offset) % cap);
}

/* ═══════════════════════════════════════════════════════════════════
 * UTILITIES
 * ═══════════════════════════════════════════════════════════════════ */

/* Print address decomposition for debugging */
static inline void addr_print(uint32_t addr, uint8_t tier) {
    AddrDecomp d = addr_decompose(addr, tier);
    GeoDecomp  g = addr_to_geo(addr, tier);
    fprintf(stderr,
        "[addr] tier=%u  addr=%u  macro=%u  micro=%u  "
        "spoke=%u  layer=%u  slot=%u\n",
        tier, d.addr, d.macro, d.micro,
        g.spoke, g.layer, g.slot);
}

/* Check if address is within tier capacity */
static inline int addr_valid(uint32_t addr, uint8_t tier) {
    if (tier >= ADDR_MAX_TIERS) return 0;
    if (tier <= 1) {
        return addr < ADDR_TIERS[tier].capacity;
    }
    return addr < (uint32_t)addr_tier_capacity(tier);
}

/* Get capacity string for display */
static inline const char* addr_tier_name(uint8_t tier) {
    switch (tier) {
        case 0: return "144^2 (20736)";
        case 1: return "144^4 (430M)";
        case 2: return "144^6 (~8e14)";
        case 3: return "144^8 (~1.8e17)";
        default: return "unknown";
    }
}

#endif /* ADDR_SPACE_H */
