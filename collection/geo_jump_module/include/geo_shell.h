#ifndef GEO_SHELL_H
#define GEO_SHELL_H
/*
 * geo_shell.h — Dodecahedron compound shell address space
 *
 * Architecture:
 *   Compound A + B (2 dodecahedra, B rotated +36°)
 *   24 global anchors = 12 pentagon centers A + 12 B (fixed forever)
 *   Each anchor has SHELL_RINGS=10 flowers (unfolded 5-dodeca net)
 *   Total: 24 × 10 = SHELL_TOTAL=240 flower positions
 *
 * Shell = radial onion layers (12 per dodeca), anchors don't drift
 * Recursive ring replace: each flower → 5-dodeca structure (5ⁿ)
 *
 * SHELL_SIDES = compound phase: A(0) or B(1), not inner/outer
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHELL_FACES         12u   /* pentagon faces per dodecahedron */
#define SHELL_RINGS         10u   /* flowers per anchor (5 dodeca net) */
#define SHELL_SIDES          2u   /* compound phase: A(0) / B(1) */
#define SHELL_TOTAL        240u   /* 12 faces × 10 rings × 2 sides = 24×10 */

#define SHELL_FULL       20736u   /* GEO_FULL — full sphere nodes */
#define SHELL_FACE_BLOCK  1728u   /* GEO_FULL / SHELL_FACES */
#define SHELL_RING_BLOCK   172u   /* SHELL_FACE_BLOCK / SHELL_RINGS = 1728/10 = ring stride within face */
#define SHELL_SIDE_BLOCK    86u   /* SHELL_RING_BLOCK / SHELL_SIDES = 172/2 = A/B half-ring stride */

#define SHELL_A              0u   /* dodecahedron A (fixed) */
#define SHELL_B              1u   /* dodecahedron B (rotated +36°) */

static inline uint32_t geo_shell_encode(uint32_t face, uint32_t ring, uint32_t side) {
    if (face >= SHELL_FACES) face = SHELL_FACES - 1u;
    if (ring >= SHELL_RINGS) ring = SHELL_RINGS - 1u;
    if (side >= SHELL_SIDES) side = SHELL_SIDES - 1u;
    return face * (SHELL_RINGS * SHELL_SIDES) + ring * SHELL_SIDES + side;
}

static inline void geo_shell_decode(uint32_t shell_id,
                                     uint32_t *face, uint32_t *ring, uint32_t *side) {
    shell_id %= SHELL_TOTAL;
    *side =  shell_id % SHELL_SIDES;
    *ring = (shell_id / SHELL_SIDES) % SHELL_RINGS;
    *face =  shell_id / (SHELL_RINGS * SHELL_SIDES);
}

static inline uint32_t geo_shell_to_node(uint32_t shell_id) {
    shell_id %= SHELL_TOTAL;
    uint32_t face, ring, side;
    geo_shell_decode(shell_id, &face, &ring, &side);
    return (face * SHELL_FACE_BLOCK) + (ring * SHELL_RING_BLOCK) + (side * SHELL_SIDE_BLOCK);
}

static inline uint32_t geo_node_to_shell(uint32_t node_id) {
    node_id %= SHELL_FULL;
    uint32_t face = node_id / SHELL_FACE_BLOCK;
    uint32_t rem  = node_id % SHELL_FACE_BLOCK;
    uint32_t ring = rem / SHELL_RING_BLOCK;
    uint32_t rem2 = rem % SHELL_RING_BLOCK;
    uint32_t side = rem2 / SHELL_SIDE_BLOCK;
    if (face >= SHELL_FACES) face = SHELL_FACES - 1u;
    if (ring >= SHELL_RINGS) ring = SHELL_RINGS - 1u;
    if (side >= SHELL_SIDES) side = SHELL_SIDES - 1u;
    return geo_shell_encode(face, ring, side);
}

static inline uint32_t geo_shell_face(uint32_t node_id) {
    return (node_id % SHELL_FULL) / SHELL_FACE_BLOCK;
}

static inline uint32_t geo_shell_ring(uint32_t node_id) {
    uint32_t r = ((node_id % SHELL_FULL) % SHELL_FACE_BLOCK) / SHELL_RING_BLOCK;
    if (r >= SHELL_RINGS) r = SHELL_RINGS - 1u;
    return r;
}

static inline uint32_t geo_shell_side(uint32_t node_id) {
    uint32_t s = (((node_id % SHELL_FULL) % SHELL_FACE_BLOCK) % SHELL_RING_BLOCK) / SHELL_SIDE_BLOCK;
    if (s >= SHELL_SIDES) s = SHELL_SIDES - 1u;
    return s;
}

static inline uint32_t geo_shell_face_base(uint32_t torus_face) {
    return (torus_face % SHELL_FACES) * SHELL_FACE_BLOCK;
}

#include "geo_dodeca_adj.h"
#include "geo_jump.h"

static inline uint32_t geo_shell_jump(uint32_t shell_id, GeoJumpType type, uint32_t param) {
    uint32_t node = geo_shell_to_node(shell_id);
    return geo_node_to_shell(geo_jump(node, type, param));
}

static inline uint32_t geo_shell_jump_r(uint32_t shell_id, const GeoJumpRouter *r) {
    uint32_t node = geo_shell_to_node(shell_id);
    return geo_node_to_shell(geo_jump_r(node, r));
}

static inline void geo_shell_jump_batch(const uint32_t *shell_ids, uint32_t n,
                                         GeoJumpType type, uint32_t param, uint32_t *out) {
    for (uint32_t i = 0; i < n; i++)
        out[i] = geo_shell_jump(shell_ids[i], type, param);
}

static inline uint32_t geo_shell_distance(uint32_t a, uint32_t b) {
    a %= SHELL_TOTAL; b %= SHELL_TOTAL;
    uint32_t fa, fb, ra, rb, sa, sb;
    geo_shell_decode(a, &fa, &ra, &sa);
    geo_shell_decode(b, &fb, &rb, &sb);
    uint32_t df = (fa > fb) ? (fa - fb) : (fb - fa);
    uint32_t dr = (ra > rb) ? (ra - rb) : (rb - ra);
    uint32_t ds = (sa != sb) ? 1u : 0u;
    uint32_t min_df = (df < SHELL_FACES - df) ? df : (SHELL_FACES - df);
    return min_df * (SHELL_RINGS * SHELL_SIDES) + dr * SHELL_SIDES + ds;
}

/* GEO_INCIRCLE / GEO_MIDDLE / GEO_BETWEEN / GEO_OUTSIDE defined in geo_jump.h */

typedef struct {
    uint32_t centroid;     // anchor center node
    uint32_t inner_r;      // incircle radius (shell 0)
    uint32_t outer_r;      // circumcircle radius (shell 11)
} CentroidContainer;

static inline CentroidContainer geo_container_from_anchor(uint32_t anchor_id) {
    CentroidContainer c;
    anchor_id %= 24u;
    c.centroid = anchor_id * (SHELL_FULL / 24u) + (SHELL_FULL / 48u);
    c.inner_r  = GEO_ZONE_INNER_R;
    c.outer_r  = GEO_ZONE_OUTER_R;
    return c;
}

static inline uint32_t geo_container_dist(const CentroidContainer *c, uint32_t node) {
    int32_t diff = (int32_t)(c->centroid - node);
    if (diff < 0) diff = -diff;
    return (uint32_t)diff;
}

static inline uint32_t geo_container_zone(const CentroidContainer *c, uint32_t node) {
    uint32_t d = geo_container_dist(c, node);
    if (d < c->inner_r) return GEO_INCIRCLE;
    if (d < c->outer_r) return GEO_MIDDLE;
    if (d < c->outer_r * 3u) return GEO_BETWEEN;
    return GEO_OUTSIDE;
}

#ifdef __cplusplus
}
#endif

#endif
