/*
 * geo_field_climate.h — Geometric Tensor Field + Capo
 * ══════════════════════════════════════════════════════════════
 *
 * Field = seed + key + attractors
 *   seed:   geo_jump origin (region anchor)
 *   key:    capo shift (domain transfer operator)
 *   field:  attractor map (implicit structure)
 *
 * Capo = frame-of-reference shift
 *   same tensor topology, different address region
 *
 * Modality:
 *   TEXT/AUDIO: sparse 1D field (sequential attractors)
 *   IMAGE:      dense 2D field  (spatial attractor map)
 *   VIDEO:      delta chain     (Δtensor per fibo tick)
 *   GEO:        raw geo_jump address space
 *
 * Depends on: geo_jump.h, geo_shell_fold.h, geo_dodeca_adj.h
 * No malloc. No float. Frozen.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "geo_jump.h"
#include "geo_shell_fold.h"
#include "geo_dodeca_adj.h"

#define CLIMATE_MAX_ATTRACTORS   8u
#define CLIMATE_KEY_BITS         8u

#define CLIMATE_MODALITY_TEXT    0u
#define CLIMATE_MODALITY_AUDIO   1u
#define CLIMATE_MODALITY_IMAGE   2u
#define CLIMATE_MODALITY_VIDEO   3u
#define CLIMATE_MODALITY_GEO     4u

/* ── Attractor ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t node;
    uint8_t  weight;
    uint8_t  layer;
    uint8_t  pent_id;
    uint8_t  flags;
} ClimateAttractor;

#define CLIMATE_FLAG_HOT      (1u<<0)
#define CLIMATE_FLAG_BOUNDARY (1u<<1)
#define CLIMATE_FLAG_PEAK     (1u<<2)

/* ── Field Descriptor ──────────────────────────────────────────── */
typedef struct {
    uint32_t seed;
    uint8_t  key;
    uint8_t  modality;
    uint8_t  n_attract;
    uint8_t  globe;
    uint32_t tick;
    uint32_t centroid;
    ClimateAttractor attract[CLIMATE_MAX_ATTRACTORS];
} ClimateField;

/* ── Capo shift ─────────────────────────────────────────────────── */
static inline void climate_capo(ClimateField *f, uint8_t new_key) {
    if (new_key == f->key) return;
    uint32_t delta = GEO_WRAP((uint64_t)(new_key - f->key) * GEO_TOWER);
    for (uint8_t i = 0; i < f->n_attract && i < CLIMATE_MAX_ATTRACTORS; i++)
        f->attract[i].node = GEO_WRAP(f->attract[i].node + delta);
    f->centroid = GEO_WRAP(f->centroid + delta);
    f->seed     = GEO_WRAP(f->seed     + delta);
    f->key      = new_key;
}

/* ── Centroid ──────────────────────────────────────────────────── */
static inline void climate_update_centroid(ClimateField *f) {
    if (f->n_attract == 0u) return;
    uint64_t sum = 0; uint32_t tw = 0;
    for (uint8_t i = 0; i < f->n_attract && i < CLIMATE_MAX_ATTRACTORS; i++) {
        sum += (uint64_t)f->attract[i].node * f->attract[i].weight;
        tw  += f->attract[i].weight;
    }
    f->centroid = (tw > 0u) ? GEO_WRAP((uint32_t)(sum / tw)) : f->seed;
}

/* ── Init ──────────────────────────────────────────────────────── */
static inline void climate_init(ClimateField *f,
                                 uint32_t seed, uint8_t key,
                                 uint8_t modality, uint8_t globe,
                                 uint32_t tick) {
    memset(f, 0, sizeof(*f));
    f->seed     = seed % GEO_FULL;
    f->key      = key;
    f->modality = modality;
    f->globe    = globe & 1u;
    f->tick     = tick;
    f->centroid = f->seed;
}

/* ── Add attractor ─────────────────────────────────────────────── */
static inline int climate_add(ClimateField *f,
                               uint32_t node, uint8_t weight) {
    if (f->n_attract >= CLIMATE_MAX_ATTRACTORS) return -1;
    uint8_t i = f->n_attract++;
    f->attract[i].node    = node % GEO_FULL;
    f->attract[i].weight  = weight;
    f->attract[i].layer   = (uint8_t)geo_shell_level(node);
    f->attract[i].pent_id = (uint8_t)(geo_pentagon_id(node) - 1u);
    /* hot path: node is on pentagon axis (no cell offset) */
    uint32_t face_stride = GEO_FULL / GEO_PENTAGONS;
    f->attract[i].flags  = ((node % face_stride) % GEO_TOWER == 0)
                           ? CLIMATE_FLAG_HOT : 0u;
    climate_update_centroid(f);
    return 0;
}

/* ── Similarity ────────────────────────────────────────────────── */
static inline uint8_t climate_similarity(const ClimateField *a,
                                           const ClimateField *b) {
    uint8_t cnt_score = (a->n_attract == b->n_attract) ? 128u : 64u;
    uint32_t da = a->centroid, db = b->centroid;
    uint32_t dist = (da > db) ? da - db : db - da;
    if (dist > GEO_FULL / 2u) dist = GEO_FULL - dist;
    uint32_t ds = 128u - (dist * 128u) / (GEO_FULL / 2u);
    return (uint8_t)((cnt_score + ds) / 2u);
}

/* ── Validate ──────────────────────────────────────────────────── */
static inline int climate_valid(const ClimateField *f) {
    if (f->n_attract > CLIMATE_MAX_ATTRACTORS) return 0;
    if (f->centroid >= GEO_FULL) return 0;
    for (uint8_t i = 0; i < f->n_attract; i++)
        if (f->attract[i].node >= GEO_FULL) return 0;
    return 1;
}
