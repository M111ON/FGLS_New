#include "pogls_addr.h"

const PoglsAddrTier POGLS_TIERS[POGLS_MAX_TIERS] = {
    { 0, 20736u,         15,  7,  8, 0x0000007Fu, 0x000000FFu,    128u,    256u },
    { 1, 429981696u,     29, 15, 14, 0x00007FFFu, 0x00003FFFu,  32768u,  16384u },
    { 2, 0u,             43, 22, 21, 0x003FFFFFu, 0x001FFFFFu,4194304u,4194304u },
    { 3, 0u,             58, 29, 29, 0x1FFFFFFFu, 0x1FFFFFFFu,536870912u,536870912u },
};

uint64_t pogls_tier_capacity(uint8_t tier) {
    if (tier == 0) return 20736ULL;
    if (tier == 1) return 429981696ULL;
    uint64_t cap = 20736ULL;
    for (uint8_t i = 1; i <= tier; i++) cap *= 20736ULL;
    return cap;
}

uint8_t pogls_select_tier(uint32_t tensor_count, uint32_t hidden_dim) {
    (void)hidden_dim;
    if (tensor_count < 20736u)        return 0;
    if (tensor_count < 429981696u)    return 1;
    return 2;
}

PoglsAddrDecomp pogls_decompose(uint32_t addr, uint8_t tier) {
    PoglsAddrDecomp d;
    d.addr = addr;
    d.tier = tier;
    if (tier == 0) {
        d.macro = (addr >> 8) & 0x7Fu;
        d.micro = addr & 0xFFu;
    } else if (tier < POGLS_MAX_TIERS) {
        const PoglsAddrTier *t = &POGLS_TIERS[tier];
        d.macro = (addr >> t->micro_bits) & t->macro_mask;
        d.micro = addr & t->micro_mask;
    } else {
        d.macro = addr;
        d.micro = 0;
    }
    return d;
}

uint32_t pogls_compose(uint32_t macro, uint32_t micro, uint8_t tier) {
    if (tier == 0) {
        return ((macro & 0x7Fu) << 8) | (micro & 0xFFu);
    } else if (tier < POGLS_MAX_TIERS) {
        const PoglsAddrTier *t = &POGLS_TIERS[tier];
        return ((macro & t->macro_mask) << t->micro_bits) | (micro & t->micro_mask);
    }
    return macro;
}

PoglsGeoDecomp pogls_to_geo(uint32_t addr, uint8_t tier) {
    PoglsGeoDecomp g;
    g.face = 0;
    if (tier == 0) {
        g.spoke = addr / POGLS_DIM_A;
        g.layer = addr / POGLS_DIM_A;
        g.slot  = addr % POGLS_DIM_A;
    } else {
        PoglsAddrDecomp ad = pogls_decompose(addr, tier);
        g.spoke = ad.macro / POGLS_DIM_A;
        g.layer = ad.macro / POGLS_DIM_A;
        g.slot  = ad.micro % POGLS_DIM_A;
    }
    return g;
}

uint32_t pogls_from_name(const char *name, uint8_t tier) {
    if (!name || name[0] == '\0') return 0;

    uint32_t h = 0x811c9dc5u;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 0x01000193u;
    }

    uint32_t macro = 0;
    uint32_t micro = 0;

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

        const PoglsAddrTier *t = &POGLS_TIERS[tier < POGLS_MAX_TIERS ? tier : 0];
        macro = layer % t->macro_slots;
        micro = th % t->micro_slots;
    } else {
        micro = h % (tier < POGLS_MAX_TIERS ? POGLS_TIERS[tier].micro_slots : 256u);
    }

    return pogls_compose(macro, micro, tier);
}

uint32_t pogls_capo(uint32_t base, uint32_t face, uint8_t tier) {
    uint64_t cap = pogls_tier_capacity(tier);
    uint64_t offset = (uint64_t)face * (uint64_t)POGLS_STRIDE * (uint64_t)POGLS_TOWER;
    return (uint32_t)((base + offset) % cap);
}

int pogls_addr_valid(uint32_t addr, uint8_t tier) {
    if (tier >= POGLS_MAX_TIERS) return 0;
    if (tier <= 1) return addr < POGLS_TIERS[tier].capacity;
    return addr < (uint32_t)pogls_tier_capacity(tier);
}

const char* pogls_tier_name(uint8_t tier) {
    switch (tier) {
        case 0: return "144^2 (20736)";
        case 1: return "144^4 (430M)";
        case 2: return "144^6 (~8e14)";
        case 3: return "144^8 (~1.8e17)";
        default: return "unknown";
    }
}

void pogls_addr_print(uint32_t addr, uint8_t tier) {
    PoglsAddrDecomp d = pogls_decompose(addr, tier);
    PoglsGeoDecomp  g = pogls_to_geo(addr, tier);
    fprintf(stderr,
        "[pogls_addr] tier=%u addr=%u macro=%u micro=%u "
        "spoke=%u layer=%u slot=%u\n",
        tier, d.addr, d.macro, d.micro,
        g.spoke, g.layer, g.slot);
}
