/*
 * geo_hidden_pocket.h — Hidden Pocket: Invert-Dodeca Frustum
 *
 * Inverted pentagonal frustum at each dodecahedron face:
 *   Outer dodecahedron   → shell level 11 (dodeca surface)
 *   Inverted frustum pit  → trapezoid walls connecting outer→inner
 *   Inner mini-dodeca     → shell level 0 (innermost, scaled dodeca)
 *
 * 60 trapezoid walls = 12 faces × 5 edges per face.
 * Each trapezoid: outer_edge(i) → inner_edge(i) for edge i of pentagon.
 * The 12 bottom pentagons assemble into a complete mini dodecahedron.
 *
 * Frustum "inverted" = narrow end faces inward (the pit bottom is the
 * small pentagon, deep inside). The trapezoid walls angle inward from
 * the dodeca surface toward the mini dodeca core.
 *
 * All mapping uses existing GEO_FULL = 20736 address space:
 *   12 pentagons × 1728 nodes each
 *   Shell level 11 = outer pentagon (dodeca surface)
 *   Shell level 0  = inner pentagon (mini dodeca surface)
 *   Levels 1..10   = frustum interior (trapezoid walls)
 *
 * No malloc. No float. No new vertex storage.
 */

#ifndef GEO_HIDDEN_POCKET_H
#define GEO_HIDDEN_POCKET_H

#include <stdint.h>
#include <math.h>

/* ── Pocket constants ────────────────────────────────────── */
#define POCKET_FACES           12u   /* dodecahedron faces */
#define POCKET_EDGES            5u   /* edges per pentagon */
#define POCKET_TRAPEZOIDS      60u   /* 12 × 5: one per face-edge */
#define POCKET_INNER_FACES     12u   /* mini dodeca faces */

#define POCKET_SHELL_OUTER     11u   /* dodeca surface */
#define POCKET_SHELL_INNER      0u   /* mini dodeca surface */
#define POCKET_SHELL_COUNT     12u   /* total shell levels 0..11 */

/* Default mini dodeca scale: golden ratio conjugate squared.
 * Inner vertex = t × outer vertex (from origin).
 * t ≈ 0.382 gives face at ~0.526 from origin (nested nicely). */
#define POCKET_DEFAULT_T       0.3819660112501051  /* φ⁻² */
#define POCKET_MIN_T           0.1
#define POCKET_MAX_T           0.9

/* ── Trapezoid wall index ──────────────────────────────────
 * Each trapezoid is identified by (face, edge) where:
 *   face = 0..11 (dodeca face)
 *   edge = 0..4  (edge within pentagon)
 * Linear index = face * 5 + edge = 0..59 */

/* For a given trapezoid index, return the (face, edge) pair */
typedef struct {
    uint8_t face;
    uint8_t edge;
} PocketTrapId;

static inline PocketTrapId pocket_trap_decode(uint8_t trap_idx) {
    PocketTrapId t;
    t.face = trap_idx / 5u;
    t.edge = trap_idx % 5u;
    return t;
}

static inline uint8_t pocket_trap_encode(uint8_t face, uint8_t edge) {
    return (uint8_t)((face % POCKET_FACES) * 5u + (edge % POCKET_EDGES));
}

/* ── Per-edge helper: adjacent face for a given edge ───────
 * Returns the face on the other side of edge e of face f.
 * Uses the dodeca adjacency table. */

/* ── Outer pentagon vertices (shell level 11) ──────────────
 * These are the 5 vertices of pentagon face f on the dodeca surface.
 * Same as DODECA_FACE_VERTS but indexed by local sector/edge. */

/* 20 dodecahedron vertices (standard golden-ratio coordinates) */
static const double POCKET_DODECA_VERTS[20][3] = {
    { 1.000000,  1.000000,  1.000000},
    { 1.000000,  1.000000, -1.000000},
    { 1.000000, -1.000000,  1.000000},
    { 1.000000, -1.000000, -1.000000},
    {-1.000000,  1.000000,  1.000000},
    {-1.000000,  1.000000, -1.000000},
    {-1.000000, -1.000000,  1.000000},
    {-1.000000, -1.000000, -1.000000},
    { 0.000000,  1.618034,  0.618034},
    { 0.000000,  1.618034, -0.618034},
    { 0.000000, -1.618034,  0.618034},
    { 0.000000, -1.618034, -0.618034},
    { 0.618034,  0.000000,  1.618034},
    {-0.618034,  0.000000,  1.618034},
    { 0.618034,  0.000000, -1.618034},
    {-0.618034,  0.000000, -1.618034},
    { 1.618034,  0.618034,  0.000000},
    { 1.618034, -0.618034,  0.000000},
    {-1.618034,  0.618034,  0.000000},
    {-1.618034, -0.618034,  0.000000},
};

/* Face → 5 vertex indices (CCW edge-connected order).
   Each face is defined by one of the 12 plane equations:
   x±φy=±φ², y±φz=±φ², z±φx=±φ².
   Every consecutive pair is an edge of the dodecahedron. */
static const uint8_t POCKET_FACE_VERTS[12][5] = {
    { 0,  8,  9,  1, 16},  /* face 0:  x+φy=+φ² */
    { 6, 10, 11,  7, 19},  /* face 1:  x+φy=-φ² */
    { 2, 10, 11,  3, 17},  /* face 2:  x-φy=+φ² */
    { 4,  8,  9,  5, 18},  /* face 3:  x-φy=-φ² */
    { 0,  8,  4, 13, 12},  /* face 4:  y+φz=+φ² */
    { 3, 11,  7, 15, 14},  /* face 5:  y+φz=-φ² */
    { 1,  9,  5, 15, 14},  /* face 6:  y-φz=+φ² */
    { 2, 10,  6, 13, 12},  /* face 7:  y-φz=-φ² */
    { 0, 12,  2, 17, 16},  /* face 8:  z+φx=+φ² */
    { 5, 15,  7, 19, 18},  /* face 9:  z+φx=-φ² */
    { 4, 13,  6, 19, 18},  /* face 10: z-φx=+φ² */
    { 1, 14,  3, 17, 16},  /* face 11: z-φx=-φ² */
};

/* Face centers = closest point to origin on each face plane.
   For plane ax+by+cz=d, center = (a,b,c)×d/(a²+b²+c²).
   All at distance a = φ²/√(1+φ²) ≈ 1.37638 from origin (INSIDE circumsphere R=√3). */
static const double POCKET_FACE_CENTERS[12][3] = {
    { 0.723607,  1.170820,  0.000000},  /* face 0: x+φy=+φ² */
    {-0.723607, -1.170820,  0.000000},  /* face 1: x+φy=-φ² */
    { 0.723607, -1.170820,  0.000000},  /* face 2: x-φy=+φ² */
    {-0.723607,  1.170820,  0.000000},  /* face 3: x-φy=-φ² */
    { 0.000000,  0.723607,  1.170820},  /* face 4: y+φz=+φ² */
    { 0.000000, -0.723607, -1.170820},  /* face 5: y+φz=-φ² */
    { 0.000000,  0.723607, -1.170820},  /* face 6: y-φz=+φ² */
    { 0.000000, -0.723607,  1.170820},  /* face 7: y-φz=-φ² */
    { 1.170820,  0.000000,  0.723607},  /* face 8: z+φx=+φ² */
    {-1.170820,  0.000000, -0.723607},  /* face 9: z+φx=-φ² */
    {-1.170820,  0.000000,  0.723607},  /* face 10: z-φx=+φ² */
    { 1.170820,  0.000000, -0.723607},  /* face 11: z-φx=-φ² */
};

/* ══════════════════════════════════════════════════════════════
   TRAPEZOID WALL GEOMETRY
   For face f, edge e (connecting vertex v[e] to v[(e+1)%5]):
     Top edge:    outer_v[e] → outer_v[(e+1)%5]   (at shell level 11)
     Bottom edge: inner_v[e] → inner_v[(e+1)%5]   (at shell level 0)
     Side edges:  outer_v[e] → inner_v[e] and outer_v[(e+1)%5] → inner_v[(e+1)%5]
   ══════════════════════════════════════════════════════════════ */

/* Get the 4 vertices of a trapezoid wall for face f, edge e.
 * inner_scale = t factor for inner dodeca (default POCKET_DEFAULT_T).
 * Out: v[0..3] = top-left, top-right, bottom-right, bottom-left (CCW when viewed from outside)
 * Each v[i] = {x, y, z} */
static inline void pocket_trapezoid_verts(uint32_t face,
                                           uint32_t edge,
                                           double inner_scale,
                                           double v[4][3])
{
    face  %= 12u;
    edge  %=  5u;
    uint8_t vi  = POCKET_FACE_VERTS[face][edge];
    uint8_t vin = POCKET_FACE_VERTS[face][(edge + 1u) % 5u];

    double cx = POCKET_FACE_CENTERS[face][0];
    double cy = POCKET_FACE_CENTERS[face][1];
    double cz = POCKET_FACE_CENTERS[face][2];

    /* Outer vertices (shell level 11) */
    v[0][0] = POCKET_DODECA_VERTS[vi][0];
    v[0][1] = POCKET_DODECA_VERTS[vi][1];
    v[0][2] = POCKET_DODECA_VERTS[vi][2];

    v[1][0] = POCKET_DODECA_VERTS[vin][0];
    v[1][1] = POCKET_DODECA_VERTS[vin][1];
    v[1][2] = POCKET_DODECA_VERTS[vin][2];

    /* Inner vertices = blend from face center toward outer:
     * v_inner = fc + t * (v_outer - fc) = t*v_outer + (1-t)*fc */
    double s = inner_scale;
    double t = 1.0 - s;

    v[2][0] = s * POCKET_DODECA_VERTS[vin][0] + t * cx;
    v[2][1] = s * POCKET_DODECA_VERTS[vin][1] + t * cy;
    v[2][2] = s * POCKET_DODECA_VERTS[vin][2] + t * cz;

    v[3][0] = s * POCKET_DODECA_VERTS[vi][0] + t * cx;
    v[3][1] = s * POCKET_DODECA_VERTS[vi][1] + t * cy;
    v[3][2] = s * POCKET_DODECA_VERTS[vi][2] + t * cz;
}

/* ══════════════════════════════════════════════════════════════
   INNER MINI-DODECAHEDRON
   12 inner pentagons (one per face) form a complete dodecahedron
   when scaled by t and displaced toward face centers.
   ══════════════════════════════════════════════════════════════ */

/* Get the 5 inner pentagon vertices for a given face.
 * inner_scale = t factor (default POCKET_DEFAULT_T).
 * Out: inner_verts[5][3] = 5 vertices of the inner pentagon */
static inline void pocket_inner_pentagon(uint32_t face,
                                          double inner_scale,
                                          double inner_verts[5][3])
{
    face %= 12u;
    double s = inner_scale;
    double t = 1.0 - s;

    double cx = POCKET_FACE_CENTERS[face][0];
    double cy = POCKET_FACE_CENTERS[face][1];
    double cz = POCKET_FACE_CENTERS[face][2];

    for (uint32_t e = 0; e < 5; e++) {
        uint8_t vi = POCKET_FACE_VERTS[face][e];
        inner_verts[e][0] = s * POCKET_DODECA_VERTS[vi][0] + t * cx;
        inner_verts[e][1] = s * POCKET_DODECA_VERTS[vi][1] + t * cy;
        inner_verts[e][2] = s * POCKET_DODECA_VERTS[vi][2] + t * cz;
    }
}

/* ══════════════════════════════════════════════════════════════
   SHELL-LEVEL MAPPING
   Node_id → which pocket feature it belongs to.
   ══════════════════════════════════════════════════════════════ */

/* Shell level 0..11 within a pentagon (144 nodes per level).
 * Level 11 = outer dodeca surface (top of pit).
 * Level 0  = inner mini-dodeca (bottom of pit).
 * Levels 1..10 = frustum interior. */
static inline uint32_t pocket_shell_level(uint32_t node_id) {
    return (node_id / 144u) % 12u;
}

/* Does this node_id sit on a trapezoid wall?
 * Returns the trapezoid index (0..59), or 0xFF if not on a wall.
 * A node is on a wall if it's in the frustum region (shell level 1..10)
 * and positioned on an edge-aligned sector (sectors 0,2,4,6,8). */
static inline uint8_t pocket_trap_for_node(uint32_t node_id) {
    uint32_t face = (node_id / 1728u) % 12u;
    uint32_t local = node_id % 1728u;
    uint32_t level = local / 144u;

    if (level == 0u || level == 11u)
        return 0xFF;  /* on pentagon faces, not walls */

    uint32_t intra = local % 144u;
    uint32_t sector = intra / 6u;   /* 0..9 */

    /* Edge-aligned sectors: 0,2,4,6,8 → map to edges 0..4 */
    if (sector % 2u != 0u)
        return 0xFF;  /* interior sector, not on a wall edge */

    uint8_t edge = (uint8_t)(sector / 2u);
    return pocket_trap_encode((uint8_t)face, edge);
}

/* ══════════════════════════════════════════════════════════════
   PIT ROUTING — navigate through the pocket structure
   ══════════════════════════════════════════════════════════════ */

/* From an outer face node (shell level 11), go straight down the
 * frustum to the corresponding inner face node (shell level 0).
 * Same (face, intra-face position), different shell level.
 * Returns node_id of the same intra-pentagon position at level 0. */
static inline uint32_t pocket_outer_to_inner(uint32_t node_id) {
    uint32_t face = node_id / 1728u;
    uint32_t local = node_id % 1728u;
    uint32_t intra = local % 144u;   /* position within shell level */
    /* At level 0: base = face * 1728 + intra */
    return face * 1728u + intra;
}

/* From an inner face node (shell level 0), go up to the outer
 * dodeca surface (shell level 11) — same face, same intra position. */
static inline uint32_t pocket_inner_to_outer(uint32_t node_id) {
    uint32_t face = node_id / 1728u;
    uint32_t local = node_id % 1728u;
    uint32_t intra = local % 144u;   /* position within shell level */
    /* At level 11: base = face * 1728 + 11*144 + intra */
    return face * 1728u + 11u * 144u + intra;
}

/* Cross-face: from a node on a trapezoid wall at edge e of face f,
 * find the corresponding node on the adjacent face's trapezoid wall.
 * This is how the 60 trapezoid walls "tile" perfectly — each wall is
 * shared by 2 adjacent pits. */
static inline uint32_t pocket_cross_edge(uint32_t node_id) {
    uint32_t face = (node_id / 1728u) % 12u;
    uint32_t local = node_id % 1728u;
    uint32_t intra = local % 144u;
    uint32_t sector = intra / 6u;

    if (sector % 2u != 0u)
        return node_id;  /* not on an edge wall — stay put */

    uint32_t edge = sector / 2u;

    /* Map to adjacent face via DODECA_ADJ.
     * Edge e of face f connects to neighbor face g with entry edge e'.
     * The mapping is: neighbor's edge = the edge within neighbor that
     * this edge connects to. For standard dodeca, edge e of face f
     * maps to neighbor = DODECA_ADJ[f][e].face, and the entry edge
     * on the neighbor side is DODECA_ADJ[f][e].edge. */

    /* Adjacency for the 12 corrected dodeca faces.
     * Each entry: {neighbor_face, entry_edge_on_neighbor}
     * Verified: ADJ[f][e] → neighbor AND ADJ[neighbor][entry_edge] → f */
    static const uint8_t ADJ[12][5][2] = {
        {{4,0},{3,1},{6,0},{11,4},{8,4}},   /* face 0 */
        {{7,1},{2,1},{5,1},{9,2},{10,2}},   /* face 1 */
        {{7,0},{1,1},{5,0},{11,2},{8,2}},   /* face 2 */
        {{4,1},{0,1},{6,1},{9,4},{10,4}},   /* face 3 */
        {{0,0},{3,0},{10,0},{7,3},{8,0}},   /* face 4 */
        {{2,2},{1,2},{9,1},{6,3},{11,1}},   /* face 5 */
        {{0,2},{3,2},{9,0},{5,3},{11,0}},   /* face 6 */
        {{2,0},{1,0},{10,1},{4,3},{8,1}},   /* face 7 */
        {{4,4},{7,4},{2,4},{11,3},{0,4}},   /* face 8 */
        {{6,2},{5,2},{1,3},{10,3},{3,3}},   /* face 9 */
        {{4,2},{7,2},{1,4},{9,3},{3,4}},    /* face 10 */
        {{6,4},{5,4},{2,3},{8,3},{0,3}},    /* face 11 */
    };

    uint8_t nb_face = ADJ[face][edge][0];
    uint8_t nb_edge = ADJ[face][edge][1];

    /* The intra-level position on the neighbor's trapezoid:
     * neighbor's sector = nb_edge * 2 (edge-aligned sector) */
    uint32_t nb_sector = (uint32_t)nb_edge * 2u;
    uint32_t nb_intra = nb_sector * 6u + (intra % 6u);
    uint32_t nb_local = (local / 144u) * 144u + nb_intra;

    return (uint32_t)nb_face * 1728u + nb_local;
}

/* ══════════════════════════════════════════════════════════════
   FRUSTUM SHELL INTERPOLATION
   For a given node at shell level L (1..10), compute interpolated
   3D position on the frustum wall between outer (L=11) and inner (L=0).
   ══════════════════════════════════════════════════════════════ */

/* Linear interpolation factor for shell level L.
 * L=11 → factor=0.0 (outer), L=0 → factor=1.0 (inner) */
static inline double pocket_interp_factor(uint32_t shell_level) {
    if (shell_level >= 11u) return 0.0;
    return (11.0 - (double)shell_level) / 11.0;
}

/* Interpolate vertex position on frustum wall.
 * node_id: any node in the pocket frustum region.
 * inner_scale: t factor for inner dodeca.
 * out_x/y/z: interpolated 3D position. */
static inline void pocket_interp_pos(uint32_t node_id,
                                      double inner_scale,
                                      double *out_x,
                                      double *out_y,
                                      double *out_z)
{
    uint32_t face = (node_id / 1728u) % 12u;
    uint32_t local = node_id % 1728u;
    uint32_t level = local / 144u;
    uint32_t intra = local % 144u;
    uint32_t sector = intra / 6u;

    double t = pocket_interp_factor(level);  /* 0=outer, 1=inner */

    double cx = POCKET_FACE_CENTERS[face][0];
    double cy = POCKET_FACE_CENTERS[face][1];
    double cz = POCKET_FACE_CENTERS[face][2];

    double s = inner_scale;
    double blend = 1.0 - (1.0 - s) * t;  /* interpolate s factor */

    /* Get the two nearest edge vertices for this sector */
    uint32_t edge = sector / 2u;
    if (edge > 4u) edge = 4u;
    uint8_t vi  = POCKET_FACE_VERTS[face][edge];
    uint8_t vin = POCKET_FACE_VERTS[face][(edge + 1u) % 5u];

    /* Slot within sector (0..5) for sub-position along the edge */
    uint32_t slot = intra % 6u;
    double slerp = (slot + 0.5) / 6.0;

    /* Edge midpoint */
    double emx = (POCKET_DODECA_VERTS[vi][0] + POCKET_DODECA_VERTS[vin][0]) * 0.5;
    double emy = (POCKET_DODECA_VERTS[vi][1] + POCKET_DODECA_VERTS[vin][1]) * 0.5;
    double emz = (POCKET_DODECA_VERTS[vi][2] + POCKET_DODECA_VERTS[vin][2]) * 0.5;

    /* Blend between face-center edge (sector=0) and edge-midpoint (sector=9) */
    double edge_blend = (double)sector / 9.0;

    /* Outer position on dodeca surface */
    double px, py, pz;
    if (edge_blend < 0.5) {
        /* Near face center */
        double u = edge_blend * 2.0;
        px = cx * (1 - u) + emx * u;
        py = cy * (1 - u) + emy * u;
        pz = cz * (1 - u) + emz * u;
    } else {
        /* Near edge midpoint */
        double u = (edge_blend - 0.5) * 2.0;
        double slot_offset = (slot - 2.5) / 6.0;
        px = emx * (1 - u) + (POCKET_DODECA_VERTS[vi][0] * (1 - slot_offset)
                           + POCKET_DODECA_VERTS[vin][0] * slot_offset) * u;
        py = emy * (1 - u) + (POCKET_DODECA_VERTS[vi][1] * (1 - slot_offset)
                           + POCKET_DODECA_VERTS[vin][1] * slot_offset) * u;
        pz = emz * (1 - u) + (POCKET_DODECA_VERTS[vi][2] * (1 - slot_offset)
                           + POCKET_DODECA_VERTS[vin][2] * slot_offset) * u;
    }

    /* Inner position on mini dodeca */
    double ix = s * px + (1.0 - s) * cx;
    double iy = s * py + (1.0 - s) * cy;
    double iz = s * pz + (1.0 - s) * cz;

    /* Interpolate between outer (t=0) and inner (t=1) */
    *out_x = px * (1.0 - t) + ix * t;
    *out_y = py * (1.0 - t) + iy * t;
    *out_z = pz * (1.0 - t) + iz * t;
}

#endif /* GEO_HIDDEN_POCKET_H */
