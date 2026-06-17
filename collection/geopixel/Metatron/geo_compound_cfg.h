/*
 * geo_compound_cfg.h — Unified Geometry Configuration (Y-Triangle)
 * MIGRATED from compound-of-5-tetra/octa duality to geo_jump Y-triangle.
 * Single address space: GEO_FULL = 20736 = 576 Y-triangles × 36 tick.
 * No TETRA/OCTA enum. No face_base/frustum_divisor. No geo_face_route().
 */

#ifndef GEO_COMPOUND_CFG_H
#define GEO_COMPOUND_CFG_H
#include <stdint.h>

typedef struct {
    uint32_t compounds;        uint32_t faces_per_comp;
    uint32_t face_slots;       uint32_t total_slots;
    uint32_t addr_base;        uint32_t addr_range;
    uint32_t junction;         uint32_t spokes;
    uint32_t slots_per_spoke;  uint8_t  pow2; uint8_t  pow3;
} GeoCompoundCfg;

static const GeoCompoundCfg GEO_CFG = {
    .compounds=12u, .faces_per_comp=12u, .face_slots=1728u, .total_slots=20736u,
    .addr_base=0u,  .addr_range=20736u,  .junction=20736u,
    .spokes=6u,     .slots_per_spoke=3456u,
    .pow2=8u,       .pow3=4u,
};

static inline uint32_t geo_cfg_spoke(const GeoCompoundCfg *c, uint64_t a) {
    return (uint32_t)((a-c->addr_base)%c->total_slots)/c->slots_per_spoke;
}
static inline uint32_t geo_cfg_face(const GeoCompoundCfg *c, uint64_t a) {
    return (uint32_t)((a-c->addr_base)%c->total_slots)/c->face_slots;
}
static inline uint32_t geo_cfg_slot_in_face(const GeoCompoundCfg *c, uint64_t a) {
    return (uint32_t)((a-c->addr_base)%c->total_slots)%c->face_slots;
}
static inline uint8_t geo_cfg_polarity(const GeoCompoundCfg *c, uint64_t a) {
    uint32_t p=(uint32_t)((a-c->addr_base)%c->total_slots)%c->face_slots;
    return (uint8_t)((p%(c->face_slots/2u))>=(c->face_slots/4u));
}
static inline uint32_t geo_cfg_hilbert(const GeoCompoundCfg *c, uint64_t a) {
    return (uint32_t)((a-c->addr_base)%c->total_slots)/c->spokes;
}
#endif
