#ifndef LC_TWIN_GATE_H
#define LC_TWIN_GATE_H

#include <stdint.h>
#include <string.h>
#include "lc_hdr.h"

enum {
    LC_GATE_WARP = 0,
    LC_GATE_ROUTE = 1,
    LC_GATE_COLLISION = 2,
    LC_GATE_GROUND = 3,
};

typedef LCGate LCGateCompat;

typedef struct {
    LCPalette palette_a;
    LCPalette palette_b;
    uint32_t gate_counts[4];
} LCTwinGateCtx;

static inline void lc_twin_gate_init(LCTwinGateCtx *c)
{
    memset(c, 0, sizeof(*c));
    for (uint32_t i = 0; i < 4u; i++) {
        c->palette_a.mask[i] = ~0ull;
        c->palette_b.mask[i] = ~0ull;
    }
}

/* Legacy stream path: always positive, so sign-based GROUND never fires. */
static inline LCHdr lc_hdr_encode_addr(uint64_t addr)
{
    return lch_pack(
        0u,
        1u,
        (uint8_t)(addr & 0x7u),
        (uint8_t)((addr >> 3) & 0x7u),
        (uint8_t)((addr >> 6) & 0x7u),
        LC_LEVEL_0,
        (uint16_t)(addr % LCH_ANG_STEPS),
        (uint8_t)(addr & 0x3u));
}

static inline LCHdr lc_hdr_encode_value(uint64_t value)
{
    return lch_pack(
        0u,
        1u,
        (uint8_t)(value & 0x7u),
        (uint8_t)((value >> 3) & 0x7u),
        (uint8_t)((value >> 6) & 0x7u),
        LC_LEVEL_0,
        (uint16_t)(value % LCH_ANG_STEPS),
        (uint8_t)(value & 0x3u));
}

#endif
