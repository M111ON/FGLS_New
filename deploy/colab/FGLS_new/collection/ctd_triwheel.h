/*
 * ctd_triwheel.h — Triangle Wheel Substrate Generator
 *
 * 1 base edge + 60° rotation → chain equilateral Δ
 * 6 Δ = virtual hexagon (grouping, not structure)
 * Core: 2 Δ → 10 Δ @ 36° around origin
 *
 * Tensor data "drags" on wheel → active Δ → centroid (free) + zone
 *
 * Convention:
 *   n_core = 10  (pentagon core, 36° each)
 *   n_ring = 60  (outer ring, 10 hex × 6 Δ)
 *   n_tot  = 70
 *
 * 3D coordinates: (x,y,0) for now, z reserved for future
 */

#ifndef CTD_TRIWHEEL_H
#define CTD_TRIWHEEL_H

#include <stdint.h>
#include <math.h>

#define TW_CORE    10
#define TW_HEX      6
#define TW_N_HEX   10
#define TW_RING    (TW_HEX * TW_N_HEX)
#define TW_TOT     (TW_CORE + TW_RING)
#define TW_DEG36   0.62831853f   /* 36° radians */
#define TW_DEG60   1.04719755f   /* 60° radians */
#define TW_DEG30   0.52359878f   /* 30° radians */

typedef struct {
    /* centroids: triangle[0..TW_CORE-1] = core, [TW_CORE..TW_TOT-1] = ring */
    float centroids[TW_TOT][3];
    /* vertices of each triangle: 3 per tri */
    float vertices[TW_TOT][3][3];
    /* triangle → hex group index (-1 = core) */
    int8_t hex_group[TW_TOT];
    /* normalized radius from origin */
    float radius;
    float core_radius;
} TriWheel;

/* Generate triangle wheel from a single base edge direction angle */
static inline void tw_init(TriWheel *w, float base_angle, float radius)
{
    w->radius = radius;
    w->core_radius = radius * 0.5f; /* core spans half the hex radius */

    /* ── Generate core: 10 Δ around origin @ 36° intervals ── */
    float c36 = cosf(TW_DEG36), s36 = sinf(TW_DEG36);
    float r = w->core_radius;

    for (int i = 0; i < TW_CORE; i++) {
        float a0 = i * TW_DEG36 + base_angle;
        float a1 = (i + 1) * TW_DEG36 + base_angle;

        /* vertex 0 = origin */
        w->vertices[i][0][0] = 0.f;
        w->vertices[i][0][1] = 0.f;
        w->vertices[i][0][2] = 0.f;

        /* vertex 1 = on circle */
        w->vertices[i][1][0] = r * cosf(a0);
        w->vertices[i][1][1] = r * sinf(a0);
        w->vertices[i][1][2] = 0.f;

        /* vertex 2 = on circle (next angle) */
        w->vertices[i][2][0] = r * cosf(a1);
        w->vertices[i][2][1] = r * sinf(a1);
        w->vertices[i][2][2] = 0.f;

        /* centroid = average of 3 vertices */
        w->centroids[i][0] = (w->vertices[i][0][0] + w->vertices[i][1][0] + w->vertices[i][2][0]) / 3.f;
        w->centroids[i][1] = (w->vertices[i][0][1] + w->vertices[i][1][1] + w->vertices[i][2][1]) / 3.f;
        w->centroids[i][2] = 0.f;

        w->hex_group[i] = -1; /* core */
    }

    /* ── Generate ring: 10 hex × 6 equilateral Δ ── */
    /* Each hex shares one core triangle's outer edge (v1-v2) */
    /* Then 6 equilateral triangles fill outward */

    for (int h = 0; h < TW_N_HEX; h++) {
        /* hex center direction = midway between core vertices h and h+1 */
        float h_angle = (h + 0.5f) * TW_DEG36 + base_angle;
        float hex_cx = w->core_radius * cosf(h_angle) * 1.2f;   /* approximate */
        float hex_cy = w->core_radius * sinf(h_angle) * 1.2f;

        /* 6 equilateral triangles around hex center */
        for (int t = 0; t < TW_HEX; t++) {
            int idx = TW_CORE + h * TW_HEX + t;

            /* triangle angle offset: first shares core edge, then outward */
            float tri_angle = h_angle + (t - 0.5f) * TW_DEG60;
            float tri_len = w->core_radius * 0.6f;

            /* vertices: equilateral Δ with one edge outward */
            float v0[3] = { hex_cx, hex_cy, 0.f };   /* hex center */
            float v1[3] = { hex_cx + tri_len * cosf(tri_angle - TW_DEG30),
                            hex_cy + tri_len * sinf(tri_angle - TW_DEG30), 0.f };
            float v2[3] = { hex_cx + tri_len * cosf(tri_angle + TW_DEG30),
                            hex_cy + tri_len * sinf(tri_angle + TW_DEG30), 0.f };

            for (int k = 0; k < 3; k++) {
                w->vertices[idx][0][k] = v0[k];
                w->vertices[idx][1][k] = v1[k];
                w->vertices[idx][2][k] = v2[k];
            }
            w->centroids[idx][0] = (v0[0] + v1[0] + v2[0]) / 3.f;
            w->centroids[idx][1] = (v0[1] + v1[1] + v2[1]) / 3.f;
            w->centroids[idx][2] = 0.f;
            w->hex_group[idx] = h;
        }
    }
}

/* Activate triangles closest to a given direction vector */
/* Returns number of activated triangles (writes to indices/confidences out) */
static inline int tw_activate(TriWheel *w,
                               float dir[3],
                               int max_activate,
                               int *out_indices,
                               float *out_conf)
{
    float angle = atan2f(dir[1], dir[0]);
    int count = 0;

    for (int i = 0; i < TW_TOT && count < max_activate; i++) {
        float ca = atan2f(w->centroids[i][1], w->centroids[i][0]);
        float diff = fabsf(angle - ca);

        /* normalize to [-π, π] */
        if (diff > 3.14159265f) diff = 2.f * 3.14159265f - diff;

        /* confidence = 1/(1 + diff/0.1) — drops off with angular distance */
        float conf = 1.f / (1.f + diff / 0.1f);

        if (conf > 0.5f) {
            out_indices[count] = i;
            out_conf[count] = conf;
            count++;
        }
    }
    return count;
}

/* Get centroid of triangle i */
static inline void tw_centroid(TriWheel *w, int i, float out[3])
{
    out[0] = w->centroids[i][0];
    out[1] = w->centroids[i][1];
    out[2] = w->centroids[i][2];
}

/* Get zone: core=0..9, hex N tri M = 10 + N*6 + M */
static inline int tw_zone_id(int tri_idx)
{
    if (tri_idx < TW_CORE) return tri_idx;         /* core 0-9 */
    return tri_idx;                                 /* ring 10-69 */
}

#endif /* CTD_TRIWHEEL_H */
