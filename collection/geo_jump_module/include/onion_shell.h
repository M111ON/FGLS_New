/*
 * onion_shell.h — 12-Layer Onion Shell Container
 * ════════════════════════════════════════════════
 *
 * Full onion: 2 dodeca (A fixed, B +36°) × 12 layers each
 *
 * Structure:
 *   OnionShell
 *   ├── ShellContainer layer[12]   active working set
 *   ├── uint32_t anchor[24]        global fixed (A=0..11, B=12..23)
 *   ├── LiftShaft shaft[24]        elevator: center → anchor, O(1)
 *   ├── uint8_t  side              SHELL_A / SHELL_B active
 *   └── uint8_t  composite_mask    bitmask: which layers are composite
 *
 * LiftShaft: direct path inner↔outer without walking each layer
 *   shaft[i].addr[layer] = precomputed address at each layer for anchor i
 *
 * Anchor placement:
 *   A[i] = geo_shell_to_node(geo_shell_encode(i, 0, SHELL_A))
 *   B[i] = geo_shell_to_node(geo_shell_encode(i, 0, SHELL_B))
 *   → exact pentagon center, no drift on subdivide
 *
 * Depends: shell_container.h, shell_hop.h, geo_shell.h
 * No malloc. No float.
 * ════════════════════════════════════════════════
 */

#ifndef ONION_SHELL_H
#define ONION_SHELL_H

#include <stdint.h>
#include <string.h>
#include "geo_shell.h"
#include "shell_container.h"
#include "shell_hop.h"

/* ── Constants ───────────────────────────────────────────────── */
#define ONION_LAYERS        12u    /* active working set           */
#define ONION_ANCHORS       24u    /* 12A + 12B global fixed       */
#define ONION_MAX_LAYERS    65536u /* address space ceiling        */

/* ── LiftShaft ───────────────────────────────────────────────── */
/*
 * 1 shaft per anchor (24 total)
 * addr[layer] = precomputed node address at each layer
 * → jump inner↔outer in O(1), no per-layer traversal
 */
typedef struct {
    uint32_t anchor_id;            /* 0-23 global anchor           */
    uint32_t addr[ONION_LAYERS];   /* address at each layer        */
    uint8_t  side;                 /* SHELL_A or SHELL_B           */
} LiftShaft;

/* ── OnionShell ──────────────────────────────────────────────── */
typedef struct {
    ShellContainer layer[ONION_LAYERS];   /* 12 active shells       */
    uint32_t       anchor[ONION_ANCHORS]; /* 24 global fixed anchors*/
    LiftShaft      shaft[ONION_ANCHORS];  /* 24 elevator shafts     */
    uint8_t        side;                  /* active: SHELL_A/B      */
    uint8_t        composite_mask;        /* bit i = layer i is composite */
    uint32_t       seed;                  /* birth seed             */
} OnionShell;

/* ── Anchor placement (exact, no drift) ─────────────────────── */
/*
 * A[i] = pentagon center of face i, side A (inner)
 * B[i] = pentagon center of face i, side B (outer = +36° compound)
 * Uses geo_shell_to_node() which is exact O(1)
 */
static inline void _onion_place_anchors(OnionShell *o)
{
    for (uint8_t i = 0u; i < SHELL_FACES; i++) {
        /* A: face i, ring 0, side SHELL_A */
        o->anchor[i]              = geo_shell_to_node(
            geo_shell_encode(i, 0u, SHELL_A));
        /* B: face i, ring 0, side SHELL_B */
        o->anchor[SHELL_FACES + i] = geo_shell_to_node(
            geo_shell_encode(i, 0u, SHELL_B));
    }
}

/* ── Lift shaft precompute ───────────────────────────────────── */
/*
 * For each anchor i, precompute address at every layer
 * layer 0 = innermost, layer 11 = outermost
 * addr[l] = anchor_base + l × SHELL_FACE_BLOCK / ONION_LAYERS
 */
static inline void _onion_build_shafts(OnionShell *o)
{
    const uint32_t step = SHELL_FACE_BLOCK / ONION_LAYERS; /* 144 */

    for (uint8_t i = 0u; i < ONION_ANCHORS; i++) {
        o->shaft[i].anchor_id = i;
        o->shaft[i].side      = (i < SHELL_FACES) ? SHELL_A : SHELL_B;
        uint32_t base         = o->anchor[i];
        for (uint8_t l = 0u; l < ONION_LAYERS; l++) {
            o->shaft[i].addr[l] = (base + l * step) % SHELL_FULL;
        }
    }
}

/* ── Init ────────────────────────────────────────────────────── */
/*
 * onion_init — build full onion from seed
 *   - place 24 anchors exact
 *   - build 24 lift shafts
 *   - init 12 layers with increasing subdivision
 *   - start with SHELL_A active
 */
static inline void onion_init(OnionShell *o, uint32_t seed)
{
    memset(o, 0, sizeof(*o));
    o->seed = seed;
    o->side = SHELL_A;
    o->composite_mask = 0u;

    /* 1. place anchors exact */
    _onion_place_anchors(o);

    /* 2. build lift shafts */
    _onion_build_shafts(o);

    /* 3. init each layer
     *    subdivision increases outward: layer 0 = coarse, 11 = fine
     *    seed per layer = parent seed XOR layer_id × fibo prime */
    for (uint8_t l = 0u; l < ONION_LAYERS; l++) {
        uint32_t layer_seed = seed ^ ((uint32_t)l * 89u); /* fibo[11]=89 */
        uint8_t  subdiv     = (uint8_t)(l + 1u);          /* 1..12 */

        shell_init(&o->layer[l], l, subdiv, layer_seed);

        /* wire anchor table from global anchors */
        for (uint8_t a = 0u; a < SHELL_N_ANCHORS; a++) {
            uint8_t gidx = (o->side == SHELL_A) ? a : (a + SHELL_FACES);
            o->layer[l].anchor[a] = o->anchor[gidx % ONION_ANCHORS];
        }
    }
}

/* ── Side toggle (A↔B clutch) ───────────────────────────────── */
/*
 * onion_toggle_side — switch active compound A↔B
 * O(1): flip side flag + rewire layer anchor tables
 * No data moved, no recalculation of geometry
 */
static inline void onion_toggle_side(OnionShell *o)
{
    o->side = (o->side == SHELL_A) ? SHELL_B : SHELL_A;

    /* rewire anchor tables to new active side */
    for (uint8_t l = 0u; l < ONION_LAYERS; l++) {
        for (uint8_t a = 0u; a < SHELL_N_ANCHORS; a++) {
            uint8_t gidx = (o->side == SHELL_A)
                           ? a
                           : (uint8_t)(a + SHELL_FACES);
            o->layer[l].anchor[a] = o->anchor[gidx % ONION_ANCHORS];
        }
    }
}

/* ── Lift: jump directly to any layer on same anchor ────────── */
/*
 * onion_lift — O(1) cross-layer jump via shaft
 * anchor_id : 0-23 global anchor
 * to_layer  : 0-11 destination layer
 */
static inline uint32_t onion_lift(const OnionShell *o,
                                   uint8_t anchor_id,
                                   uint8_t to_layer)
{
    if (anchor_id >= ONION_ANCHORS) anchor_id = 0u;
    if (to_layer  >= ONION_LAYERS)  to_layer  = ONION_LAYERS - 1u;
    return o->shaft[anchor_id].addr[to_layer];
}

/* ── Composite attach/detach ─────────────────────────────────── */
static inline void onion_attach(OnionShell *o, uint8_t layer_id) {
    if (layer_id < ONION_LAYERS)
        o->composite_mask |= (uint8_t)(1u << (layer_id % 8u));
}
static inline void onion_detach(OnionShell *o, uint8_t layer_id) {
    if (layer_id < ONION_LAYERS)
        o->composite_mask &= (uint8_t)~(1u << (layer_id % 8u));
}
static inline uint8_t onion_is_composite(const OnionShell *o, uint8_t layer_id) {
    if (layer_id >= ONION_LAYERS) return 0u;
    return (o->composite_mask >> (layer_id % 8u)) & 1u;
}

/* ── Address via chord on specific layer ─────────────────────── */
static inline uint64_t onion_addr(const OnionShell *o,
                                   uint8_t layer_id,
                                   const Chord *c)
{
    if (layer_id >= ONION_LAYERS) layer_id = 0u;
    return shell_addr(&o->layer[layer_id], c);
}

/* ── Verify ──────────────────────────────────────────────────── */
static inline int onion_verify(const OnionShell *o)
{
    /* all 24 anchors in valid range */
    for (uint8_t i = 0u; i < ONION_ANCHORS; i++)
        if (o->anchor[i] >= SHELL_FULL) return -1;

    /* no two anchors on same side identical */
    for (uint8_t i = 0u; i < SHELL_FACES; i++)
        for (uint8_t j = (uint8_t)(i+1u); j < SHELL_FACES; j++)
            if (o->anchor[i] == o->anchor[j]) return -2;

    /* lift shafts in range */
    for (uint8_t i = 0u; i < ONION_ANCHORS; i++)
        for (uint8_t l = 0u; l < ONION_LAYERS; l++)
            if (o->shaft[i].addr[l] >= SHELL_FULL) return -3;

    /* each layer verify */
    for (uint8_t l = 0u; l < ONION_LAYERS; l++)
        if (shell_verify(&o->layer[l]) != 0) return -4;

    return 0;
}

#endif /* ONION_SHELL_H */
