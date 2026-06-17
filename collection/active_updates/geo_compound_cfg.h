/* geo_compound_cfg.h — Unified Geometry (Y-Triangle), no TETRA/OCTA duality */
#ifndef GEO_COMPOUND_CFG_H
#define GEO_COMPOUND_CFG_H
#include <stdint.h>
typedef struct {
    uint32_t compounds, faces_per_comp, face_slots, total_slots;
    uint32_t addr_base, addr_range, junction, spokes, slots_per_spoke;
    uint8_t pow2, pow3;
} GeoCompoundCfg;
static const GeoCompoundCfg GEO_CFG = {
    .compounds=12u, .faces_per_comp=12u, .face_slots=1728u, .total_slots=20736u,
    .addr_base=0u,  .addr_range=20736u,  .junction=20736u,
    .spokes=6u,     .slots_per_spoke=3456u,
    .pow2=8u,       .pow3=4u,
};
static inline uint32_t geo_cfg_spoke(const GeoCompoundCfg *c, uint64_t a){return(uint32_t)((a-c->addr_base)%c->total_slots)/c->slots_per_spoke;}
static inline uint32_t geo_cfg_face(const GeoCompoundCfg *c,uint64_t a){return(uint32_t)((a-c->addr_base)%c->total_slots)/c->face_slots;}
static inline uint32_t geo_cfg_slot_in_face(const GeoCompoundCfg *c,uint64_t a){return(uint32_t)((a-c->addr_base)%c->total_slots)%c->face_slots;}
static inline uint8_t geo_cfg_polarity(const GeoCompoundCfg *c,uint64_t a){uint32_t p=(uint32_t)((a-c->addr_base)%c->total_slots)%c->face_slots;return(uint8_t)((p%(c->face_slots/2u))>=(c->face_slots/4u));}
static inline uint32_t geo_cfg_hilbert(const GeoCompoundCfg *c,uint64_t a){return(uint32_t)((a-c->addr_base)%c->total_slots)/c->spokes;}
#endif
