/*
 * geo_field_ring.h — Ring 120 Classifier + Full Pipeline
 * ══════════════════════════════════════════════════════════════
 * Pipeline: [Ring 120] → [Junction 30] → [Core 24] → [geo_jump]
 *
 * Depends on: geo_jump.h, geo_dodeca_adj.h, geo_dodeca_ring.h,
 *             geo_shell_fold.h, geo_field_climate.h, geo_shell.h
 * No malloc. No float. Frozen.
 * ══════════════════════════════════════════════════════════════
 */
#pragma once
#include <stdint.h>
#include "geo_jump.h"
#include "geo_dodeca_adj.h"
#include "geo_dodeca_ring.h"
#include "geo_shell_fold.h"
#include "geo_field_climate.h"

/* ── Routing constants ───────────────────────────────────────── */
#define RING_ROUTE_HOTPATH  0u
#define RING_ROUTE_JUNCTION 1u
#define RING_ROUTE_CAPO     2u
#define RING_ROUTE_GLOBEB   3u

#define RING_DENSITY_TEXT    48u
#define RING_DENSITY_AUDIO   96u
#define RING_DENSITY_VIDEO  160u
#define RING_DENSITY_IMAGE  200u

/* Junction 30 = 5 dodeca × 3 face-pairs × 2 flowers */
#define JUNCTION_COUNT      30u

/* ── Classifier result ───────────────────────────────────────── */
typedef struct {
    uint8_t  face;        /* dodecahedron face 0-11               */
    uint8_t  ring;        /* ring 0-9 (dodeca×flower)             */
    uint8_t  globe;       /* 0=A 1=B                              */
    uint8_t  modality;    /* CLIMATE_MODALITY_*                   */
    uint8_t  route;       /* RING_ROUTE_*                         */
    uint8_t  layer;       /* resolved shell layer 0-11            */
    uint8_t  confidence;  /* 0-255                                */
    uint8_t  junction;    /* junction node 0-29                   */
    uint8_t  origin;      /* core origin 0-23 (globe*12+face%12)  */
    uint8_t  _pad[3];
    uint32_t node;        /* final geo_jump node_id               */
} RingClassified;         /* 16B                                  */

/* ── Modality classify ───────────────────────────────────────── */
static inline uint8_t ring_classify_modality(uint8_t density,
                                              uint8_t is_temporal) {
    if (!is_temporal)                      return CLIMATE_MODALITY_IMAGE;
    if (density < RING_DENSITY_TEXT)       return CLIMATE_MODALITY_TEXT;
    if (density < RING_DENSITY_AUDIO)      return CLIMATE_MODALITY_AUDIO;
    return CLIMATE_MODALITY_VIDEO;
}

/* ── Ring state → face + ring (uses existing encoding) ────────── */
/* ring_state 0-119: ring_state / 12 = ring(0-9), % 12 = face    */
static inline uint8_t ring_state_to_face(uint8_t state) {
    return state % DODECA_FACES;
}
static inline uint8_t ring_state_to_ring(uint8_t state) {
    return (state / DODECA_FACES) % (RING_DODECA*RING_FLOWERS);
}

/* ── Junction encode 0-29 ────────────────────────────────────── */
static inline uint8_t junction_encode(uint8_t face, uint8_t ring) {
    return (uint8_t)((ring_to_dodeca(ring) * 6u
                    + ring_to_flower(ring) * 3u
                    + (face % 6u) / 2u) % JUNCTION_COUNT);
}

/* ── Core classifier ─────────────────────────────────────────── */
static inline RingClassified ring_classify(uint8_t  state,
                                            uint8_t  density,
                                            uint8_t  is_temporal,
                                            uint8_t  layer,
                                            uint32_t tick) {
    RingClassified rc;
    state     %= RING_TOTAL;
    uint8_t face  = ring_state_to_face(state);
    uint8_t ring  = ring_state_to_ring(state);
    uint8_t globe = (state >= 60u) ? 1u : 0u;
    uint8_t live  = (tick > 0u)
                  ? shell_fold_nearest(layer, tick, 0u)
                  : layer;

    rc.face       = face;
    rc.ring       = ring;
    rc.globe      = globe;
    rc.modality   = ring_classify_modality(density, is_temporal);
    rc.layer      = live;
    rc.junction   = junction_encode(face, ring);
    rc.origin     = (uint8_t)(globe * DODECA_FACES + face);

    /* pole faces (0=north, 11=south) → hot path */
    if (face == 0u || face == 11u) {
        rc.route      = RING_ROUTE_HOTPATH;
        rc.confidence = 255u;
        rc.node       = ring_hot_path(face, live, globe);
    } else if (globe) {
        rc.route      = RING_ROUTE_GLOBEB;
        rc.confidence = 200u;
        rc.node       = GEO_WRAP(ring_hot_path(face, live, 0u)
                               + dodeca_globe_offset(1u));
    } else {
        rc.route      = RING_ROUTE_JUNCTION;
        rc.confidence = (uint8_t)(160u + ring * 8u);
        /* route via face stride in geo_jump space */
        uint32_t face_stride = GEO_FULL / GEO_PENTAGONS;
        rc.node = GEO_WRAP((uint32_t)face * face_stride
                         + (uint32_t)live * GEO_TOWER
                         + dodeca_globe_offset(globe));
    }

    return rc;
}

/* ── Pipeline result ─────────────────────────────────────────── */
typedef struct {
    uint32_t       node;
    uint8_t        junction;
    uint8_t        origin;
    uint8_t        route;
    uint8_t        confidence;
    RingClassified ring;
} PipelineResult;   /* 8 + 16 = 24B */

/* ── Full pipeline ───────────────────────────────────────────── */
static inline PipelineResult pipeline_route(uint8_t  ring_state,
                                             uint8_t  density,
                                             uint8_t  is_temporal,
                                             uint8_t  layer,
                                             uint32_t tick,
                                             uint32_t seed_node) {
    PipelineResult pr;
    pr.ring       = ring_classify(ring_state, density, is_temporal,
                                  layer, tick);
    pr.junction   = pr.ring.junction;
    pr.origin     = pr.ring.origin;
    pr.route      = pr.ring.route;
    pr.confidence = pr.ring.confidence;

    if (pr.ring.route == RING_ROUTE_HOTPATH) {
        pr.node = pr.ring.node;
    } else {
        GeoJumpType jt = (pr.ring.modality == CLIMATE_MODALITY_IMAGE)
                       ? JUMP_HILBERT : JUMP_PEANO;
        pr.node = geo_jump(pr.ring.node, jt, seed_node % GEO_BLOCK);
    }
    return pr;
}

/* ── shell_addr_ring: drop-in for shell_addr() ───────────────── */
/* eliminates JUNCTION=6912 + stride 37/41 hack                  */
static inline uint32_t shell_addr_ring(uint8_t  shell_id,
                                        uint8_t  chord_id,
                                        uint32_t seed,
                                        uint8_t  globe,
                                        uint32_t tick) {
    uint8_t state   = (uint8_t)((shell_id * 10u + chord_id * 3u
                               + seed % 7u) % RING_TOTAL);
    uint8_t density = (chord_id == 3u) ? RING_DENSITY_IMAGE
                    : (chord_id == 2u) ? RING_DENSITY_AUDIO
                    : RING_DENSITY_TEXT;
    uint8_t temporal = (chord_id <= 1u) ? 1u : 0u;
    (void)globe;
    PipelineResult pr = pipeline_route(state, density, temporal,
                                        shell_id, tick, seed);
    return pr.node;
}

/* ── ClimateField → pipeline ─────────────────────────────────── */
static inline PipelineResult climate_route(const ClimateField *f,
                                            uint8_t layer) {
    uint8_t face  = (uint8_t)((geo_pentagon_id(f->centroid) - 1u)
                              % DODECA_FACES);
    uint32_t face_stride = GEO_FULL / GEO_PENTAGONS;
    uint8_t ring  = (uint8_t)((f->centroid / face_stride) % (RING_DODECA*RING_FLOWERS));
    uint8_t state = (uint8_t)(ring * DODECA_FACES + face);

    uint8_t density  = (f->modality == CLIMATE_MODALITY_IMAGE) ? 220u
                     : (f->modality == CLIMATE_MODALITY_VIDEO) ? 160u : 40u;
    uint8_t temporal = (f->modality != CLIMATE_MODALITY_IMAGE) ? 1u : 0u;

    return pipeline_route(state, density, temporal, layer, f->tick, f->seed);
}
