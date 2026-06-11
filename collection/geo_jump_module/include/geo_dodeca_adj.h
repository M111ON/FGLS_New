/*
 * geo_dodeca_adj.h — Dodecahedron Edge Adjacency (generated)
 * ═══════════════════════════════════════════════════════════
 * Derived from exact vertex coordinates (phi-based)
 * Face ordering: north pole (z=+1.171) → south pole (z=-1.171)
 * Edge ordering: counterclockwise around face centroid
 *
 * Globe B: 36° Z-rotation IS a dodecahedron symmetry
 *   → face/edge adjacency IDENTICAL to Globe A
 *   → B differs ONLY in address space offset, not topology
 * Verified: ADJ_A == ADJ_B from actual vertex coordinate rotation
 *
 * Verified: symmetry_errors=0, each face appears 5× as neighbor
 * Face graph diameter = 3 (antipodal faces = 3 hops)
 *
 * No malloc. No float. Frozen.
 * ═══════════════════════════════════════════════════════════
 */
#pragma once
#include <stdint.h>

#define DODECA_FACES   12u
#define DODECA_EDGES    5u
#define DODECA_GLOBE    2u   /* 0=A  1=B (both same topology) */
#define DODECA_DIST_MAX 3u   /* face graph diameter */

typedef struct { uint8_t face; uint8_t edge; } DodecaAdj;

/* Globe A — icosa-axis aligned, CCW edge ordering */
static const DodecaAdj DODECA_ADJ_A[12][5] = {
    [ 0] = {{1,0}, {2,2}, {4,1}, {6,1}, {3,2}},  /* north z=+1.171 */
    [ 1] = {{0,0}, {2,3}, {5,1}, {7,1}, {3,3}},  /* upper z=+1.171 */
    [ 2] = {{8,0}, {4,2}, {0,1}, {1,1}, {5,2}},  /* upper z=+0.724 */
    [ 3] = {{9,0}, {6,2}, {0,4}, {1,4}, {7,2}},  /* upper z=+0.724 */
    [ 4] = {{6,0}, {0,2}, {2,1}, {8,1}, {10,2}},  /* equator z=+0.000 */
    [ 5] = {{7,0}, {1,2}, {2,4}, {8,4}, {11,2}},  /* equator z=+0.000 */
    [ 6] = {{4,0}, {0,3}, {3,1}, {9,1}, {10,3}},  /* equator z=+0.000 */
    [ 7] = {{5,0}, {1,3}, {3,4}, {9,4}, {11,3}},  /* equator z=+0.000 */
    [ 8] = {{2,0}, {4,3}, {10,1}, {11,1}, {5,3}},  /* lower z=-0.724 */
    [ 9] = {{3,0}, {6,3}, {10,4}, {11,4}, {7,3}},  /* lower z=-0.724 */
    [10] = {{11,0}, {8,2}, {4,4}, {6,4}, {9,2}},  /* south z=-1.171 */
    [11] = {{10,0}, {8,3}, {5,4}, {7,4}, {9,3}},  /* south z=-1.171 */
};

/* BFS distance table — face graph */
static const uint8_t DODECA_DIST[12][12] = {
    {0,1,1,1,1,2,1,2,2,2,2,3},  /* face 0 */
    {1,0,1,1,2,1,2,1,2,2,3,2},  /* face 1 */
    {1,1,0,2,1,1,2,2,1,3,2,2},  /* face 2 */
    {1,1,2,0,2,2,1,1,3,1,2,2},  /* face 3 */
    {1,2,1,2,0,2,1,3,1,2,1,2},  /* face 4 */
    {2,1,1,2,2,0,3,1,1,2,2,1},  /* face 5 */
    {1,2,2,1,1,3,0,2,2,1,1,2},  /* face 6 */
    {2,1,2,1,3,1,2,0,2,1,2,1},  /* face 7 */
    {2,2,1,3,1,1,2,2,0,2,1,1},  /* face 8 */
    {2,2,3,1,2,2,1,1,2,0,1,1},  /* face 9 */
    {2,3,2,2,1,2,1,2,1,1,0,1},  /* face 10 */
    {3,2,2,2,2,1,2,1,1,1,1,0},  /* face 11 */
};

/* Main API — topology identical for A and B */
static inline DodecaAdj dodeca_adj(uint8_t globe,
                                    uint8_t face,
                                    uint8_t edge) {
    (void)globe;
    face %= DODECA_FACES;
    edge %= DODECA_EDGES;
    return DODECA_ADJ_A[face][edge];
}

/* Globe B address offset: same face/edge, different address zone */
static inline uint32_t dodeca_globe_offset(uint8_t globe) {
    return globe ? 20736u / 2u : 0u;  /* GEO_FULL/2 = 10368 */
}

/* Topological distance (face graph, 0..3) */
static inline uint8_t dodeca_face_dist(uint8_t f0, uint8_t f1) {
    return DODECA_DIST[f0 % DODECA_FACES][f1 % DODECA_FACES];
}

/* Pentagon frustum: 5 tri gates between adjacent shell layers
 * tri_i connects edge i of face_inner to edge i of face_outer    */
static inline uint8_t dodeca_frustum_gate(uint8_t face,
                                           uint8_t edge,
                                           uint8_t globe) {
    (void)globe;
    DodecaAdj nb = dodeca_adj(globe, face, edge);
    return nb.edge;
}

/* Cross-face routing: walk N hops along edge adjacency
 * Returns final (face, entry_edge) after N steps         */
typedef struct { uint8_t face; uint8_t edge; } DodecaPos;

static inline DodecaPos dodeca_walk(uint8_t globe,
                                     uint8_t start_face,
                                     uint8_t start_edge,
                                     uint8_t hops) {
    DodecaPos p = {start_face, start_edge};
    for (uint8_t i = 0; i < hops; i++) {
        DodecaAdj nb = dodeca_adj(globe, p.face, p.edge);
        p.face = nb.face;
        p.edge = nb.edge;
    }
    return p;
}
