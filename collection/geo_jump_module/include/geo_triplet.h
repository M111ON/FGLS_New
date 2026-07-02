/*
 * geo_triplet.h — Triplet World: Dodeca + Icosa + Pentakis + Icosphere
 *
 * Three overlapping polyhedral layers sharing the same vertex set:
 *   Layer 0: Dodecahedron   (20 vertices, 12 pentagon faces)
 *   Layer 1: Icosahedron     (12 vertices, dual: dodeca face centers)
 *   Layer 2: Pentakis-apex   (12 apexes = icosa vertices, subdivided
 *             each pentagon into 5 triangles → 60 faces total)
 *   Layer 3: Icosphere       (162 vertices at f=4 subdivision)
 *
 * Pentakis apex = dodeca face center + normal × h.
 * At h=0 the apex coincides with the icosa vertex (dual relationship).
 * Raising h makes the pentakis pyramid taller; apex stays on the same
 * radial line as the face center — no NEW vertices, just reinterpretation.
 *
 * Icosphere 162v = icosahedron each edge split into 4 segments (f=4).
 * Vertex count: 10n²+2 = 10×16+2 = 162. The 12 original icosa vertices
 * are a subset — the triplet icosa layer is the coarsest level.
 *
 * All layers map to the GEO_FULL = 20736 address space:
 *   12 pentagons × 1728 nodes each
 *   Each pentagon: 10 sectors × 6 slots = 60 positions per shell level
 *   12 shell levels × 144 per level = 1728
 *
 * No malloc. No float (except init helpers). No new vertex storage.
 */

#ifndef GEO_TRIPLET_H
#define GEO_TRIPLET_H

#include <stdint.h>
#include <math.h>

/* ── Layer IDs ───────────────────────────────────────────── */
#define TRIPLET_NONE        0xFFu

#define TRIPLET_DODECA      0u   /* dodecahedron (20v, 12 pentagon faces) */
#define TRIPLET_ICOSA       1u   /* icosahedron (12v, 20 triangle faces) */
#define TRIPLET_PENTAKIS    2u   /* pentakis dodecahedron (32v, 60 triangle faces) */
#define TRIPLET_ICOSPHERE   3u   /* icosphere f=4 (162v, 320 triangle faces) */

#define TRIPLET_LAYER_COUNT 4u

/* ── Vertex counts ───────────────────────────────────────── */
#define TRIPLET_DODECA_VERTS      20u
#define TRIPLET_ICOSA_VERTS       12u   /* = dodeca face centers */
#define TRIPLET_PENTAKIS_VERTS    32u   /* 20 original + 12 apex */
#define TRIPLET_ICOSPHERE_VERTS  162u   /* f=4: 10×4²+2 = 162 */
#define TRIPLET_TOTAL_UNIQUE      32u   /* 20 + 12 (no new vertices for pentakis) */

/* ── Face counts ─────────────────────────────────────────── */
#define TRIPLET_DODECA_FACES      12u
#define TRIPLET_ICOSA_FACES       20u
#define TRIPLET_PENTAKIS_FACES    60u   /* 12 pentagons × 5 triangles */
#define TRIPLET_ICOSPHERE_FACES  320u   /* 20 icosa faces × 16 sub-triangles */

/* ── Pentakis default apex height ────────────────────────── */
#define TRIPLET_PENTAKIS_H_DEFAULT  0.0    /* apex = icosa vertex (flat pentakis) */
#define TRIPLET_PENTAKIS_H_MAX      0.5528 /* apex reaches tangent plane */

/* ── Icosphere subdivision levels ────────────────────────── */
#define TRIPLET_ICOSPHERE_F     4u       /* subdivision factor */
#define TRIPLET_ICOSPHERE_EDGES 30u      /* 30 original icosa edges */
#define TRIPLET_ICOSPHERE_FACE_VERTS 3u  /* triangles */

/* ── Triplet vertex classification ───────────────────────── */
typedef enum {
    TRIPLET_VERT_DODECA   = 0,  /* original dodeca corner (1 of 20) */
    TRIPLET_VERT_ICOSA    = 1,  /* icosa vertex = pentagon center (1 of 12) */
    TRIPLET_VERT_PENTAKIS = 2,  /* pentakis apex = icosa vertex + height h */
    TRIPLET_VERT_ICOSUB   = 3,  /* icosphere subdivision vertex (f=4) */
} TripletVertKind;

/* ── Per-pentagon, per-edge pentakis triangle ────────────── */
typedef struct {
    uint8_t apex;       /* pentakis apex index (0..11 = face center) */
    uint8_t v0;         /* dodeca vertex at one end of the edge (0..19) */
    uint8_t v1;         /* dodeca vertex at the other end (0..19) */
    uint8_t face;       /* parent pentagon face (0..11) */
    uint8_t edge;       /* edge within pentagon (0..4) */
} TripletPentakisTri;

/* ── Icosphere vertex description ────────────────────────── */
typedef struct {
    uint8_t kind;             /* TRIPLET_VERT_ICOSA or TRIPLET_VERT_ICOSUB */
    uint8_t parent_icosa;     /* parent icosa vertex index (if kind=ICOSA) */
    uint8_t edge_a;           /* edge endpoint A (if kind=ICOSUB) */
    uint8_t edge_b;           /* edge endpoint B (if kind=ICOSUB) */
    uint8_t edge_t;           /* t = edge_segment / f (if kind=ICOSUB) */
} TripletIcosphereVert;

/* ══════════════════════════════════════════════════════════════
   PENTAKIS TRIANGLE TABLE
   For each pentagon face (0..11), 5 triangles.
   Triangle i connects: apex = face, v0 = edge vertex i, v1 = edge vertex (i+1)%5
   ══════════════════════════════════════════════════════════════ */

/* Face → 5 vertex indices for the 12 dodecahedron pentagon faces.
   Derived from the 12 face plane equations:
     x±φy=±φ² (4), y±φz=±φ² (4), z±φx=±φ² (4)
   Each face contains exactly the 5 vertices satisfying its plane eq.
   Ordering: CCW around outward normal.
   Triangle i uses: apex, v[i], v[(i+1)%5] */
static const uint8_t TRIPLET_FACE_VERTS[12][5] = {
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

/* 60 pentakis triangles: flat array, 5 per face.
   Index = face*5 + edge_in_face (edge-in-face from CCW order).
   Each entry: (apex_face, v0_global, v1_global) */
static const uint8_t TRIPLET_PENTAKIS_TRI[60][3] = {
    /* face 0: {0,8,9,1,16} */ {0,0,8}, {0,8,9}, {0,9,1}, {0,1,16}, {0,16,0},
    /* face 1: {6,10,11,7,19} */ {1,6,10}, {1,10,11}, {1,11,7}, {1,7,19}, {1,19,6},
    /* face 2: {2,10,11,3,17} */ {2,2,10}, {2,10,11}, {2,11,3}, {2,3,17}, {2,17,2},
    /* face 3: {4,8,9,5,18} */ {3,4,8}, {3,8,9}, {3,9,5}, {3,5,18}, {3,18,4},
    /* face 4: {0,8,4,13,12} */ {4,0,8}, {4,8,4}, {4,4,13}, {4,13,12}, {4,12,0},
    /* face 5: {3,11,7,15,14} */ {5,3,11}, {5,11,7}, {5,7,15}, {5,15,14}, {5,14,3},
    /* face 6: {1,9,5,15,14} */ {6,1,9}, {6,9,5}, {6,5,15}, {6,15,14}, {6,14,1},
    /* face 7: {2,10,6,13,12} */ {7,2,10}, {7,10,6}, {7,6,13}, {7,13,12}, {7,12,2},
    /* face 8: {0,12,2,17,16} */ {8,0,12}, {8,12,2}, {8,2,17}, {8,17,16}, {8,16,0},
    /* face 9: {5,15,7,19,18} */ {9,5,15}, {9,15,7}, {9,7,19}, {9,19,18}, {9,18,5},
    /* face 10: {4,13,6,19,18} */ {10,4,13}, {10,13,6}, {10,6,19}, {10,19,18}, {10,18,4},
    /* face 11: {1,14,3,17,16} */ {11,1,14}, {11,14,3}, {11,3,17}, {11,17,16}, {11,16,1},
};

/* Icosphere f=4 vertex positions (162 vertices).
   All positions are on the sphere at face-center radius ≈ 1.376382.
   Icosahedron = dual of dodecahedron: 12 vertices = 12 dodeca face centers.
   Subdivided 4× per icosa edge, projected to sphere. */
static const float TRIPLET_ICOSPHERE_POS[162][3] = {
#include "icosphere_f4_table.inc"
};

/* 20 icosahedron faces (triplets of mutually adjacent dodeca face centers).
   Each row = 3 face indices (0..11) whose centers form an icosa triangle. */
static const uint8_t TRIPLET_ICOSA_FACES_TABLE[20][3] = {
#include "icosa_faces_table.inc"
};

/* 30 icosahedron edges (pairs of adjacent dodeca face centers).
   Each row = 2 face indices (0..11) sharing an edge on the icosahedron. */
static const uint8_t TRIPLET_ICOSA_EDGES[30][2] = {
#include "icosa_edges_table.inc"
};

/* ══════════════════════════════════════════════════════════════
   NODE-TO-TRIPLET CLASSIFICATION
   Classify any node_id (0..20735) into triplet layer + vertex kind.
   ══════════════════════════════════════════════════════════════ */

/* Shell level → which layer the node belongs to */
static inline uint32_t triplet_shell_level(uint32_t node_id) {
    return (node_id / 144u) % 12u;
}

/* Is this node at the outermost shell = on dodeca surface? */
static inline int triplet_is_dodeca_surface(uint32_t node_id) {
    return (triplet_shell_level(node_id) == 11u);
}

/* Is this node at the innermost shell = on mini dodeca surface? */
static inline int triplet_is_inner_surface(uint32_t node_id) {
    return (triplet_shell_level(node_id) == 0u);
}

/* Classify a node_id into triplet vertex kind.
 * The 12 pentagon centers at shell level 0 = innermost point.
 * Shell level 11 = outermost = dodecahedron surface. */
static inline TripletVertKind triplet_vert_kind(uint32_t node_id) {
    uint32_t local = node_id % 1728u;    /* within pentagon */
    uint32_t level = local / 144u;        /* shell level 0..11 */

    if (level == 11u)
        return TRIPLET_VERT_DODECA;
    if (level == 0u) {
        uint32_t intra_level = local % 144u;
        /* center nodes: when intra_level is near 0 (center of pentagon) */
        if (intra_level < 8u)
            return TRIPLET_VERT_ICOSA;
        return TRIPLET_VERT_PENTAKIS;
    }
    /* intermediate shell levels — belong to frustum/body */
    return TRIPLET_VERT_ICOSUB;
}

/* ── Pentakis apex height ─────────────────────────────────
 * The apex sits at: face_center + normal × h
 * face_center is the icosa vertex (normalized on circumsphere).
 * h=0 → apex = icosa vertex (no subdivision bulge)
 * h>0 → apex pushed outward, pentakis becomes "pointy"
 *
 * The face normal = direction from origin to face center = icosa vertex direction. */

/* Compute apex point 3D coordinates.
 * face_idx: 0..11
 * h: height above face center (0 = flat pentakis, max ~0.55 for tangent)
 * out_x/y/z: output coordinates on circumsphere radius R = sqrt(3) ≈ 1.732
 * Returns the scale factor applied (1.0 for h=0, larger for h>0) */
static inline double triplet_pentakis_apex(uint32_t face_idx,
                                            double h,
                                            double *out_x,
                                            double *out_y,
                                            double *out_z)
{
    /* Icosa vertex = dodeca face center, from face vertex average.
     * For a regular dodeca on circumsphere radius sqrt(3):
     *   center[f] = normalize(sum(v[0..4])/5) * R_circum
     * We compute using the known icosa vertex directions. */

    /* Hard-code the 12 icosa vertex = pentagon center directions.
     * These are the face centers of the dodecahedron, normalized. */
    static const double icosa_dir[12][3] = {
        { 0.000000,  0.525731,  0.850651 },  /* face 0: north */
        { 0.000000,  0.525731, -0.850651 },  /* face 1 */
        { 0.850651,  0.000000,  0.525731 },  /* face 2 */
        { 0.850651,  0.000000, -0.525731 },  /* face 3 */
        { 0.525731,  0.850651,  0.000000 },  /* face 4 */
        { 0.525731, -0.850651,  0.000000 },  /* face 5 */
        {-0.525731,  0.850651,  0.000000 },  /* face 6 */
        {-0.525731, -0.850651,  0.000000 },  /* face 7 */
        {-0.850651,  0.000000,  0.525731 },  /* face 8 */
        {-0.850651,  0.000000, -0.525731 },  /* face 9 */
        { 0.000000, -0.525731,  0.850651 },  /* face 10: south */
        { 0.000000, -0.525731, -0.850651 },  /* face 11 */
    };
    /* These are unit vectors. The circumradius R = sqrt(3) ≈ 1.73205.
     * The face center on the circumsphere = icosa_dir[f] * R. */

    /* Elevate along the normal = icosa_dir[f] by height h.
     * Apex = R * icosa_dir + h * icosa_dir = (R + h) * icosa_dir */
    face_idx %= 12u;
    double R  = 1.7320508075688772;  /* sqrt(3) */
    double scale = R + h;
    *out_x = icosa_dir[face_idx][0] * scale;
    *out_y = icosa_dir[face_idx][1] * scale;
    *out_z = icosa_dir[face_idx][2] * scale;
    return scale;
}

/* ── Icosphere vertex count for a given f ────────────────── */
static inline uint32_t triplet_icosphere_vert_count(uint32_t f) {
    return 10u * f * f + 2u;
}

static inline uint32_t triplet_icosphere_face_count(uint32_t f) {
    return 20u * f * f;
}

/* ── Triplet layer detection from shell_id ─────────────────
 * Maps shell level + node position → which triplet layer.
 * Used to determine which face routing to use for a given node. */
static inline uint32_t triplet_layer_for_shell(uint32_t shell_level) {
    if (shell_level == 11u) return TRIPLET_DODECA;   /* outermost */
    if (shell_level == 0u)  return TRIPLET_ICOSA;    /* innermost = icosa */
    /* intermediate: could be pentakis frustum or icosphere body */
    if (shell_level <= 3u)  return TRIPLET_PENTAKIS;  /* near core */
    return TRIPLET_ICOSPHERE;                           /* mid-range */
}

/* ── Pentakis triangle from node_id ─────────────────────────
 * Given a node_id, find which pentakis triangle (0..59) it maps to.
 * Returns 0xFF if the node is not on a pentakis triangle. */
static inline uint8_t triplet_pentakis_tri_for_node(uint32_t node_id) {
    uint32_t face = (node_id / 1728u) % 12u;
    uint32_t local = node_id % 1728u;
    uint32_t level = local / 144u;
    if (level != 11u && level != 0u)
        return 0xFF;  /* not on pentakis surface */

    uint32_t intra = local % 144u;
    uint32_t sector = intra / 6u;   /* 0..9 (but only 0,2,4,6,8 are edges) */

    /* Pentakis edges map to sectors 0,2,4,6,8 (the 5 edge-aligned sectors).
     * Map sector 0,2,4,6,8 → edge 0,1,2,3,4 */
    uint32_t edge = sector / 2u;
    if (edge > 4u) edge = 4u;
    return (uint8_t)(face * 5u + edge);
}

/* ── Icosphere vertex position lookup ─────────────────────
 * Get the 3D position of an icosphere f=4 vertex by its index (0..161).
 * The position is at the face-center sphere (R ≈ 1.376382). */
static inline void triplet_icosphere_pos(uint32_t idx,
                                          float *out_x, float *out_y, float *out_z)
{
    if (idx >= 162u) idx = 0u;
    *out_x = TRIPLET_ICOSPHERE_POS[idx][0];
    *out_y = TRIPLET_ICOSPHERE_POS[idx][1];
    *out_z = TRIPLET_ICOSPHERE_POS[idx][2];
}

#endif /* GEO_TRIPLET_H */
