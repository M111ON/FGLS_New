/*
 * geo_dodeca_ring.h — Pentagon Ring: cross-dodeca routing + hex conversion
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Ring = 5 dodecahedra × 2 flowers × 12 pentagon faces = 120 positions
 *   (×2 globes A/B = 288 total, matching SHELL_TOTAL)
 *
 * Within our shell encoding (face×ring×side):
 *   ring 0-4 = flower 0 (north), ring 5-9 = flower 1 (south)
 *   dodeca = ring % 5, flower = ring / 5
 *
 * ring_step(): cross-face/-dodeca navigation via edge adjacency
 * ring_to_hex() / hex_jump(): 120→20 vertex-centered hex routing
 *
 * Depends on: geo_dodeca_adj.h (DODECA_ADJ_A), geo_shell.h (shell types)
 * No malloc. No float. Frozen.
 */
#pragma once
#include <stdint.h>
#include "geo_dodeca_adj.h"
#include "geo_shell.h"

#define DODECA_VERTS    20u     /* dodecahedron vertices           */
#define RING_DODECA      5u     /* dodecahedra in the ring         */
#define RING_FLOWERS     2u     /* flowers per dodeca              */
#define RING_TOTAL     120u     /* 5×2×12 = one globe              */
#define RING_DUAL      288u     /* ×2 globes = SHELL_TOTAL         */

/* ── Vertex→Faces (20 vertices, each shared by 3 pentagons) ──── */
static const uint8_t DODECA_VERT_FACES[20][3] = {
    [ 0] = {0,1,2},  [ 1] = {0,1,3},  [ 2] = {0,2,4},  [ 3] = {0,3,6},
    [ 4] = {1,2,5},  [ 5] = {1,3,7},  [ 6] = {0,4,6},  [ 7] = {1,5,7},
    [ 8] = {2,4,8},  [ 9] = {3,6,9},  [10] = {2,5,8},  [11] = {3,7,9},
    [12] = {4,6,10}, [13] = {5,7,11}, [14] = {4,8,10}, [15] = {6,9,10},
    [16] = {5,8,11}, [17] = {7,9,11}, [18] = {8,10,11},[19] = {9,10,11},
};

/* ── Face→Vertices (5 CCW vertices per face) ─────────────────── */
static const uint8_t DODECA_FACE_VERTS[12][5] = {
    [ 0]={1,0,2,6,3},  [ 1]={1,0,4,7,5},  [ 2]={10,8,2,0,4},
    [ 3]={11,9,3,1,5}, [ 4]={12,6,2,8,14},[ 5]={13,7,4,10,16},
    [ 6]={12,6,3,9,15},[ 7]={13,7,5,11,17},[ 8]={10,8,14,18,16},
    [ 9]={11,9,15,19,17},[10]={19,18,14,12,15},[11]={19,18,16,13,17},
};

/* ── Bridge: our shell ring ↔ zip dodeca+flower ──────────────── */
/* ring 0-4 = dodeca 0-4, flower 0 (north)                      */
/* ring 5-9 = dodeca 0-4, flower 1 (south)                      */
static inline uint8_t ring_to_dodeca(uint8_t ring) {
    return ring % RING_DODECA;
}
static inline uint8_t ring_to_flower(uint8_t ring) {
    return (ring / RING_DODECA) % RING_FLOWERS;
}
static inline uint8_t dodeca_flower_to_ring(uint8_t dodeca, uint8_t flower) {
    return dodeca + flower * RING_DODECA;
}

/* ── Flower classification ───────────────────────────────────── */
/* faces 0-5 = north (flower 0), faces 6-11 = south (flower 1)   */
static inline uint8_t face_flower(uint8_t face) {
    return (face < 6u) ? 0u : 1u;
}

/* ── Boundary edge test ───────────────────────────────────────── */
/* Edge is a boundary if its two faces belong to different flowers */
static inline uint8_t edge_is_boundary(uint8_t face, uint8_t edge) {
    uint8_t nb = DODECA_ADJ_A[face % SHELL_FACES][edge % DODECA_EDGES].face;
    return (face_flower(face) != face_flower(nb)) ? 1u : 0u;
}

/* ── ring_step: advance one edge crossing ─────────────────────── */
/*
 * Given shell coords (face, ring, side) and an edge to cross,
 * returns new (face, ring) — side unchanged within same globe.
 *
 * Internal edge → stay same ring, cross to adjacent face
 * Boundary edge → cross to next dodeca's flower
 */
typedef struct { uint8_t face; uint8_t ring; } RingStepResult;

static inline RingStepResult ring_step(uint8_t face,
                                        uint8_t ring,
                                        uint8_t edge) {
    DodecaAdj nb = dodeca_adj(0, face, edge);
    uint8_t nb_flower = face_flower(nb.face);

    if (nb_flower == face_flower(face)) {
        /* internal: same dodeca, same flower */
        return (RingStepResult){nb.face, ring};
    }
    /* boundary: cross flower/dodeca boundary */
    uint8_t dodeca = ring_to_dodeca(ring);
    uint8_t flower = ring_to_flower(ring);
    if (flower == 1u) {
        /* flower 1 (south) → flower 0 (north) of next dodeca */
        uint8_t next_d = (dodeca + 1u) % RING_DODECA;
        return (RingStepResult){
            nb.face,
            dodeca_flower_to_ring(next_d, 0u)
        };
    } else {
        /* flower 0 (north) → flower 1 (south) same dodeca */
        return (RingStepResult){
            nb.face,
            dodeca_flower_to_ring(dodeca, 1u)
        };
    }
}

/* ── 120→20 hex conversion ───────────────────────────────────── */
/* face-centered (120 positions) → vertex-centered (20 regions)  */
static inline uint8_t ring_to_hex(uint8_t face, uint8_t vert_slot) {
    return DODECA_FACE_VERTS[face % SHELL_FACES][vert_slot % DODECA_EDGES];
}
/* inverse: vertex → one of its 3 faces */
static inline uint8_t hex_to_face(uint8_t vert, uint8_t face_slot) {
    return DODECA_VERT_FACES[vert % DODECA_VERTS][face_slot % 3u];
}

/* Fibonacci threshold: use hex routing at layer >= 6 */
static inline uint8_t ring_use_hex(uint8_t layer) {
    return (layer >= 6u) ? 1u : 0u;
}
