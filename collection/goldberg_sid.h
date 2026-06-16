/*
 * goldberg_sid.h — Goldberg/Dodecahedron Spherical Domain for SID
 *
 * Maps flat 12-face × 120-position trihex grid onto a subdivided
 * dodecahedron (Goldberg polyhedron) surface.
 * Each face = pentagon on sphere, subdivided by 60° triangles →
 * hexagons emerge as natural Goldberg cells.
 *
 * SID face 0..11 maps to dodecahedron pentagon face σ(face).
 */

#ifndef GOLDBERG_SID_H
#define GOLDBERG_SID_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "tri_hex_tess.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Dodecahedron constants ── */
#define GOLDBERG_N_FACES  12
#define GOLDBERG_PHI      1.618033988749895  /* (1+√5)/2 */

/* ── 3D vertex ── */
typedef struct { double x, y, z; } GVec3;

/* ══════════════════════════════════════════════════════════════════
   Dodecahedron face centers (normalized)
   A regular dodecahedron centered at origin, oriented with
   one pentagon facing +Z.
   ══════════════════════════════════════════════════════════════════ */

/* 20 vertices of a regular dodecahedron */
static const GVec3 _goldberg_verts[20] = {
    { 1,  1,  1}, { 1,  1, -1}, { 1, -1,  1}, { 1, -1, -1},
    {-1,  1,  1}, {-1,  1, -1}, {-1, -1,  1}, {-1, -1, -1},
    { 0,  GOLDBERG_PHI,  1/GOLDBERG_PHI}, { 0,  GOLDBERG_PHI, -1/GOLDBERG_PHI},
    { 0, -GOLDBERG_PHI,  1/GOLDBERG_PHI}, { 0, -GOLDBERG_PHI, -1/GOLDBERG_PHI},
    { 1/GOLDBERG_PHI, 0,  GOLDBERG_PHI}, {-1/GOLDBERG_PHI, 0,  GOLDBERG_PHI},
    { 1/GOLDBERG_PHI, 0, -GOLDBERG_PHI}, {-1/GOLDBERG_PHI, 0, -GOLDBERG_PHI},
    { GOLDBERG_PHI,  1/GOLDBERG_PHI, 0}, { GOLDBERG_PHI, -1/GOLDBERG_PHI, 0},
    {-GOLDBERG_PHI,  1/GOLDBERG_PHI, 0}, {-GOLDBERG_PHI, -1/GOLDBERG_PHI, 0}
};

/* Face vertex indices (5 vertices per pentagon, CCW).
   σ(face) maps SID face → dodecahedron pentagon. */
static const uint8_t _goldberg_face_verts[12][5] = {
    { 0,  8,  4, 13, 12},  /* face 0: +X +Y +Z region */
    { 0, 12,  2, 16,  8},  /* face 1 */
    { 8, 16,  1, 14,  9},  /* face 2 */
    { 0, 12, 13,  4,  8},  /* face 3 */
    { 4, 13,  3, 15,  5},  /* face 4 */
    { 2, 16, 17, 10,  6},  /* face 5 */
    { 1, 14, 15,  3,  7},  /* face 6 */
    {10, 17, 19, 11,  6},  /* face 7 */
    { 5, 15, 14,  1,  9},  /* face 8 */
    { 9,  8,  0,  4,  5},  /* face 9 */
    { 6, 10, 11,  7,  3},  /* face 10 */
    {11, 19, 18,  2, 12},  /* face 11 */
};

/* Face centers (average of 5 vertices, normalized) */
static GVec3 _goldberg_centers[12];
static uint8_t _goldberg_init_flag = 0;

/* Normalize vector in-place */
static inline void _gv_norm(GVec3 *v) {
    double len = sqrt(v->x*v->x + v->y*v->y + v->z*v->z);
    if (len > 0) { v->x /= len; v->y /= len; v->z /= len; }
}

static inline void goldberg_init(void) {
    if (_goldberg_init_flag) return;
    for (int f = 0; f < 12; f++) {
        double sx = 0, sy = 0, sz = 0;
        for (int k = 0; k < 5; k++) {
            int vi = _goldberg_face_verts[f][k];
            sx += _goldberg_verts[vi].x;
            sy += _goldberg_verts[vi].y;
            sz += _goldberg_verts[vi].z;
        }
        _goldberg_centers[f] = (GVec3){sx/5, sy/5, sz/5};
        _gv_norm(&_goldberg_centers[f]);
    }
    _goldberg_init_flag = 1;
}

/* ══════════════════════════════════════════════════════════════════
   THCoord → 3D position on sphere
   ══════════════════════════════════════════════════════════════════ */

/* Map local position (0..119) within pentagon face → barycentric
   point on pentagon, then project to sphere.
   Returns unit vector on sphere surface. */
static inline GVec3 goldberg_from_thcoord(THCoord c) {
    goldberg_init();
    GVec3 center = _goldberg_centers[c.face % 12];

    uint8_t is_tri, sector, slot;
    th_unpack(th_local(c.tring_pos), &is_tri, &sector, &slot);

    /* sector 0..9 within pentagon → edge direction.
       TW_N_SECTORS = 10 → 10 sectors per face.
       Each pentagon has 5 vertices. Map 10 sectors to 5 edges:
       sector%5 picks edge, sector>=5 picks opposite side. */
    int edge = sector % 5;
    int side = sector / 5;

    int v0i = _goldberg_face_verts[c.face % 12][edge];
    int v1i = _goldberg_face_verts[c.face % 12][(edge + 1) % 5];

    GVec3 v0 = _goldberg_verts[v0i];
    GVec3 v1 = _goldberg_verts[v1i];

    /* slot 0..5 along edge. slot=0 → v0, slot=5 → v1 */
    double t = (slot + 0.5) / 6.0;
    GVec3 p;
    if (side == 0) {
        /* inside pentagon: blend center → edge */
        p.x = center.x * (1 - t) + (v0.x * 0.5 + v1.x * 0.5) * t;
        p.y = center.y * (1 - t) + (v0.y * 0.5 + v1.y * 0.5) * t;
        p.z = center.z * (1 - t) + (v0.z * 0.5 + v1.z * 0.5) * t;
    } else {
        /* near vertex: blend v0 → v1 along edge */
        double u = (double)(slot) / 5.0;
        p.x = v0.x * (1 - u) + v1.x * u;
        p.y = v0.y * (1 - u) + v1.y * u;
        p.z = v0.z * (1 - u) + v1.z * u;
    }

    /* Normalize to project onto sphere */
    _gv_norm(&p);
    return p;
}

/* ══════════════════════════════════════════════════════════════════
   Spherical distance (geodesic) between two THCoords
   ══════════════════════════════════════════════════════════════════ */

static inline double goldberg_geodesic(THCoord a, THCoord b) {
    GVec3 va = goldberg_from_thcoord(a);
    GVec3 vb = goldberg_from_thcoord(b);
    double dot = va.x*vb.x + va.y*vb.y + va.z*vb.z;
    if (dot > 1.0) dot = 1.0;
    if (dot < -1.0) dot = -1.0;
    return acos(dot);
}

/* ── Spherical bond strength [0..1] from geodesic distance ── */
static inline float goldberg_bond_strength(THCoord a, THCoord b) {
    double rad = goldberg_geodesic(a, b);
    double s = 1.0 - rad / 1.6;
    return (float)(s > 0 ? (s < 1 ? s : 1) : 0);
}

/* ══════════════════════════════════════════════════════════════════
   BOND GRAPH + HOTNESS PREDICTION (same API as hex_grid.h)
   ══════════════════════════════════════════════════════════════════ */

typedef struct {
    int a, b;
    float strength;
    double geodesic;
} GoldbergBond;

typedef struct {
    GoldbergBond *bonds;
    int n_bonds, max_bonds;
} GoldbergBondGraph;

static inline void goldberg_bond_graph_init(GoldbergBondGraph *g, int max_bonds) {
    g->bonds = (GoldbergBond*)calloc((size_t)max_bonds, sizeof(GoldbergBond));
    g->n_bonds = 0;
    g->max_bonds = max_bonds;
}

static inline void goldberg_bond_graph_free(GoldbergBondGraph *g) {
    free(g->bonds); g->bonds = NULL;
    g->n_bonds = g->max_bonds = 0;
}

/* Discover bonds using goldberg geodesic threshold */
static inline void goldberg_discover_bonds(GoldbergBondGraph *g,
    THCoord *coords, int n_names, float geo_threshold) {
    for (int i = 0; i < n_names && g->n_bonds < g->max_bonds; i++) {
        for (int j = i + 1; j < n_names && g->n_bonds < g->max_bonds; j++) {
            double geo = goldberg_geodesic(coords[i], coords[j]);
            if (geo > geo_threshold) continue;
            GoldbergBond *b = &g->bonds[g->n_bonds++];
            b->a = i; b->b = j;
            b->geodesic = geo;
            b->strength = goldberg_bond_strength(coords[i], coords[j]);
        }
    }
}

/* ── Cardioid express initial hotness (same as hex_grid) ── */

/* Cardioid LUT for hotness (independent from th_grid's LUT) */
#define GB_CARDIOID_LEN 720u
static int16_t _gb_cos_lut[GB_CARDIOID_LEN];
static uint8_t _gb_cardioid_init = 0;

static inline void _gb_cardioid_lut_init(void) {
    if (_gb_cardioid_init) return;
    for (uint32_t i = 0; i < GB_CARDIOID_LEN; i++) {
        double theta = (2.0 * 3.141592653589793 * i) / GB_CARDIOID_LEN;
        _gb_cos_lut[i] = (int16_t)(cos(theta) * 255.0);
    }
    _gb_cardioid_init = 1;
}

#define GB_CARDIOID_A      256
#define GB_CARDIOID_GEO_MIN 140

static inline float goldberg_cardioid_hotness(int layer, int n_layers) {
    _gb_cardioid_lut_init();
    uint16_t pos = (uint16_t)((uint32_t)layer * GB_CARDIOID_LEN
                   / (uint32_t)(n_layers > 0 ? n_layers : 1)) % GB_CARDIOID_LEN;
    int32_t cos_val = _gb_cos_lut[pos];
    int32_t r_geo = GB_CARDIOID_A * (GB_CARDIOID_A + cos_val);
    int32_t r_geo_q8 = r_geo >> 8;
    if (r_geo_q8 < GB_CARDIOID_GEO_MIN) return 0.1f;
    uint16_t signal = (uint16_t)((uint32_t)(layer * 17 + 42) & 0xFF);
    int pass = (((int32_t)signal << 8) <= r_geo);
    return pass ? 1.0f : 0.5f;
}

/* ── Predict hotness via cardioid + goldberg bond propagation ──
   Takes external initial_hotness array (cardioid computed by caller). */
static inline float *goldberg_predict_hotness(const GoldbergBondGraph *g,
    const float *initial_hotness, int n_names) {
    float *h = (float*)calloc((size_t)n_names, sizeof(float));
    memcpy(h, initial_hotness, (size_t)n_names * sizeof(float));

    /* 3-pass propagation */
    for (int pass = 0; pass < 3; pass++) {
        float *acc = (float*)calloc((size_t)n_names, sizeof(float));
        float *wsum = (float*)calloc((size_t)n_names, sizeof(float));
        for (int b = 0; b < g->n_bonds; b++) {
            int ai = g->bonds[b].a;
            int bi = g->bonds[b].b;
            float w = g->bonds[b].strength;
            acc[ai] += h[bi] * w; wsum[ai] += w;
            acc[bi] += h[ai] * w; wsum[bi] += w;
        }
        for (int i = 0; i < n_names; i++) {
            if (wsum[i] > 0.0f) {
                float avg = acc[i] / wsum[i];
                h[i] = h[i] * 0.7f + avg * 0.3f;
                if (h[i] < 0.0f) h[i] = 0.0f;
                if (h[i] > 1.0f) h[i] = 1.0f;
            }
        }
        free(acc); free(wsum);
    }
    return h;
}

static inline void goldberg_predict_print(const char **names, int n_names,
    const float *hotness) {
    int n_hot = 0, n_warm = 0, n_cold = 0;
    for (int i = 0; i < n_names; i++) {
        if (hotness[i] >= 0.9f) n_hot++;
        else if (hotness[i] >= 0.3f) n_warm++;
        else n_cold++;
    }
    fprintf(stderr, "[goldberg] hot=%.1f%% warm=%.1f%% cold=%.1f%%\n",
        100.0 * n_hot / n_names, 100.0 * n_warm / n_names,
        100.0 * n_cold / n_names);
    fprintf(stderr, "[goldberg] top-5 hottest:\n");
    uint8_t *visited = (uint8_t*)calloc((size_t)n_names, 1);
    for (int rank = 0; rank < 5 && rank < n_names; rank++) {
        int best = 0;
        for (int i = 1; i < n_names; i++)
            if (!visited[i] && hotness[i] > hotness[best]) best = i;
        visited[best] = 1;
        fprintf(stderr, "  %d. %.4f  %s\n", rank + 1, hotness[best], names[best]);
    }
    free(visited);
}

/* ══════════════════════════════════════════════════════════════════
   Debug: print spherical coordinates for each tensor
   ══════════════════════════════════════════════════════════════════ */

static inline void goldberg_print_coords(const char **names, int n_names,
                                          THCoord *coords) {
    fprintf(stderr, "[goldberg] spherical domain:\n");
    for (int i = 0; i < n_names; i++) {
        GVec3 v = goldberg_from_thcoord(coords[i]);
        fprintf(stderr, "  (%.4f, %.4f, %.4f) face=%d pos=%d %s\n",
                v.x, v.y, v.z, coords[i].face, coords[i].tring_pos, names[i]);
    }
}

#endif
