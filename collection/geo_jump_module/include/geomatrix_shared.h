#pragma once
#include <stdint.h>
#include "geo_config.h"
#define GEOMATRIX_PATHS    18
#define GEO_WINDOW_LO      128
#define GEO_WINDOW_HI      144
#define PHASE_PROBE    0
#define PHASE_MAIN     1
#define PHASE_MIRROR   2
#define PHASE_CANCEL   3
static const uint64_t PHASE_MASK64[4] = {
    0xAAAAAAAA00000000ULL,
    0x5555555500000000ULL,
    0xF0F0F0F000000000ULL,
    0x0F0F0F0F00000000ULL,
};
typedef struct {
    uint64_t sig;
    uint16_t hpos;
    uint16_t idx;
    uint8_t  bit;
    uint8_t  phase;
    uint8_t  _pad[2];
} GeoPacket;
typedef struct {
    uint32_t sig32;
    uint16_t idx;
    uint8_t  bit;
    uint8_t  phase;
} GeoPacketSmall;
static inline uint64_t geo_bundle_fold64(const uint64_t *bundle) {
    uint64_t f = 0;
    for (int i = 0; i < GEO_BUNDLE_WORDS; i++) f ^= bundle[i];
    return f;
}
static inline uint64_t geo_compute_sig64(const uint64_t *bundle, uint8_t phase) {
    return geo_bundle_fold64(bundle) ^ PHASE_MASK64[phase & 3];
}
static inline uint32_t geo_sig64_to_sig32(uint64_t sig64) {
    return (uint32_t)(sig64 >> 32) ^ (uint32_t)(sig64 & 0xFFFFFFFFU);
}
static inline uint32_t geo_compute_sig32(const uint64_t *bundle, uint8_t phase) {
    return geo_sig64_to_sig32(geo_compute_sig64(bundle, phase));
}
