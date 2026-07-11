/*
 * pogls_addr.h — 144² Address Resolution (Pure C)
 *
 * Core geometry: 128 × 162 = 20736 = 144²
 *   128 = 2⁷  (head dimension)
 *   162 = 2 × 3⁴  (Y-triangle faces)
 *   144 = 2⁴ × 3²  (LCM of prime factors)
 *
 * API:
 *   addr_from_tensor_name() — hash tensor name → flat address
 *   addr_decompose()        — flat addr → (macro, micro)
 *   addr_compose()          — (macro, micro) → flat addr
 *   addr_capo()             — face rotation
 *   addr_valid()            — bounds check
 *
 * No platform dependency — pure integer math.
 */

#ifndef POGLS_ADDR_H
#define POGLS_ADDR_H

#include <stdint.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
 * CORE GEOMETRY CONSTANTS
 * ═══════════════════════════════════════════════════════════════════ */

#define POGLS_ADDR_DIM_A    128u    /* 2⁷ — head dimension */
#define POGLS_ADDR_DIM_B    162u    /* 2 × 3⁴ — Y-triangle faces */
#define POGLS_ADDR_BASE     20736u  /* 128 × 162 = 144² */
#define POGLS_ADDR_N        144u
#define POGLS_ADDR_STRIDE   12u
#define POGLS_ADDR_TOWER    144u
#define POGLS_ADDR_MAX_TIERS 4u

/* ═══════════════════════════════════════════════════════════════════
 * TIER DEFINITIONS
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  tier;
    uint32_t capacity;
    uint8_t  total_bits;
    uint8_t  macro_bits;
    uint8_t  micro_bits;
    uint32_t macro_mask;
    uint32_t micro_mask;
    uint32_t macro_slots;
    uint32_t micro_slots;
} PoglsAddrTier;

static const PoglsAddrTier POGLS_ADDR_TIERS[POGLS_ADDR_MAX_TIERS] = {
    /* tier  capacity     bits  macro  micro  macro_mask   micro_mask   macro_slots  micro_slots */
    {  0,   20736u,        15,     7,     8,  0x0000007Fu, 0x000000FFu,       128u,       256u },
    {  1,   429981696u,    29,    15,    14,  0x00007FFFu, 0x00003FFFu,     32768u,     16384u },
    {  2,   0u,            43,    22,    21,  0x003FFFFFu, 0x001FFFFFu,   4194304u,   4194304u },
    {  3,   0u,            58,    29,    29,  0x1FFFFFFFu, 0x1FFFFFFFu,  536870912u, 536870912u },
};

static inline uint64_t pogls_addr_tier_capacity(uint8_t tier) {
    if (tier == 0) return 20736ULL;
    if (tier == 1) return 429981696ULL;
    uint64_t cap = 20736ULL;
    for (uint8_t i = 1; i <= tier; i++) cap *= 20736ULL;
    return cap;
}

/* ═══════════════════════════════════════════════════════════════════
 * TIER SELECTION
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint8_t pogls_addr_select_tier(uint32_t tensor_count, uint32_t hidden_dim) {
    (void)hidden_dim;
    if (tensor_count < 20736u)     return 0;
    if (tensor_count < 429981696u) return 1;
    return 2;
}

/* ═══════════════════════════════════════════════════════════════════
 * ADDRESS DECOMPOSITION
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t macro;
    uint32_t micro;
    uint32_t addr;
    uint8_t  tier;
} PoglsAddrDecomp;

static inline PoglsAddrDecomp pogls_addr_decompose(uint32_t addr, uint8_t tier) {
    PoglsAddrDecomp d;
    d.addr = addr;
    d.tier = tier;

    if (tier == 0) {
        d.macro = (addr >> 8) & 0x7Fu;
        d.micro = addr & 0xFFu;
    } else if (tier < POGLS_ADDR_MAX_TIERS) {
        const PoglsAddrTier *t = &POGLS_ADDR_TIERS[tier];
        d.macro = (addr >> t->micro_bits) & t->macro_mask;
        d.micro = addr & t->micro_mask;
    } else {
        d.macro = addr;
        d.micro = 0;
    }
    return d;
}

static inline uint32_t pogls_addr_compose(uint32_t macro, uint32_t micro, uint8_t tier) {
    if (tier == 0) {
        return ((macro & 0x7Fu) << 8) | (micro & 0xFFu);
    } else if (tier < POGLS_ADDR_MAX_TIERS) {
        const PoglsAddrTier *t = &POGLS_ADDR_TIERS[tier];
        return ((macro & t->macro_mask) << t->micro_bits) | (micro & t->micro_mask);
    }
    return macro;
}

/* ═══════════════════════════════════════════════════════════════════
 * TENSOR NAME → ADDRESS MAPPING
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint32_t pogls_addr_from_name(const char *name, uint8_t tier) {
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
        uint32_t layer = 0;
        const char *p = name + 4;
        while (*p >= '0' && *p <= '9') {
            layer = layer * 10 + (uint32_t)(*p - '0');
            p++;
        }
        if (*p == '.') p++;
        uint32_t th = 0;
        for (const char *q = p; *q; q++) {
            th = th * 131u + (uint8_t)*q;
        }
        const PoglsAddrTier *t = &POGLS_ADDR_TIERS[tier < POGLS_ADDR_MAX_TIERS ? tier : 0];
        macro = layer % t->macro_slots;
        micro = th % t->micro_slots;
    } else {
        micro = h % (tier < POGLS_ADDR_MAX_TIERS ? POGLS_ADDR_TIERS[tier].micro_slots : 256u);
    }

    return pogls_addr_compose(macro, micro, tier);
}

/* ═══════════════════════════════════════════════════════════════════
 * CAPO — face rotation
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint32_t pogls_addr_capo(uint32_t base, uint32_t face, uint8_t tier) {
    uint64_t cap = pogls_addr_tier_capacity(tier);
    uint64_t offset = (uint64_t)face * (uint64_t)POGLS_ADDR_STRIDE * (uint64_t)POGLS_ADDR_TOWER;
    return (uint32_t)((base + offset) % cap);
}

/* ═══════════════════════════════════════════════════════════════════
 * UTILITIES
 * ═══════════════════════════════════════════════════════════════════ */

static inline int pogls_addr_valid(uint32_t addr, uint8_t tier) {
    if (tier >= POGLS_ADDR_MAX_TIERS) return 0;
    if (tier <= 1) return addr < POGLS_ADDR_TIERS[tier].capacity;
    return addr < (uint32_t)pogls_addr_tier_capacity(tier);
}

static inline const char* pogls_addr_tier_name(uint8_t tier) {
    switch (tier) {
        case 0: return "144^2 (20736)";
        case 1: return "144^4 (430M)";
        case 2: return "144^6 (~8e14)";
        case 3: return "144^8 (~1.8e17)";
        default: return "unknown";
    }
}

#endif /* POGLS_ADDR_H */
