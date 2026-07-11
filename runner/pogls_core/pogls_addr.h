#ifndef POGLS_ADDR_H
#define POGLS_ADDR_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Core Geometry Constants ── */

#define POGLS_DIM_A         128u
#define POGLS_DIM_B         162u
#define POGLS_BASE          (POGLS_DIM_A * POGLS_DIM_B)
#define POGLS_N             144u
#define POGLS_STRIDE        12u
#define POGLS_TOWER         144u
#define POGLS_MAX_TIERS     4u

/* ── Tier Table ── */

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

extern const PoglsAddrTier POGLS_TIERS[POGLS_MAX_TIERS];

/* ── Address Decomposition ── */

typedef struct {
    uint32_t macro;
    uint32_t micro;
    uint32_t addr;
    uint8_t  tier;
} PoglsAddrDecomp;

typedef struct {
    uint32_t spoke;
    uint32_t layer;
    uint32_t slot;
    uint32_t face;
} PoglsGeoDecomp;

/* ── API ── */

uint64_t    pogls_tier_capacity(uint8_t tier);
uint8_t     pogls_select_tier(uint32_t tensor_count, uint32_t hidden_dim);

PoglsAddrDecomp pogls_decompose(uint32_t addr, uint8_t tier);
uint32_t    pogls_compose(uint32_t macro, uint32_t micro, uint8_t tier);

PoglsGeoDecomp pogls_to_geo(uint32_t addr, uint8_t tier);

uint32_t    pogls_from_name(const char *name, uint8_t tier);
uint32_t    pogls_capo(uint32_t base, uint32_t face, uint8_t tier);

int         pogls_addr_valid(uint32_t addr, uint8_t tier);
const char* pogls_tier_name(uint8_t tier);
void        pogls_addr_print(uint32_t addr, uint8_t tier);

#ifdef __cplusplus
}
#endif

#endif /* POGLS_ADDR_H */
