/*
 * ctd_g10.h — 10-Triangle Radial Encoder (G10)
 *
 * 2 pentagons rotated 36° → 10 radial triangles
 * Each triangle: centroid + free tip vertex
 *
 * Mode 1: avg(10 tri centroids)
 * Mode 2: avg(10 tri centroids + 10 tips)
 */

#ifndef CTD_G10_H
#define CTD_G10_H

#include "ctd_octa.h"
#include <math.h>
#include <string.h>

#define G10_VERT  5
#define G10_TRI  10

static inline void _g10_rot_z(float out[3], float in[3], float angle) {
    float c = cosf(angle), si = sinf(angle);
    out[0] = in[0]*c - in[1]*si;
    out[1] = in[0]*si + in[1]*c;
    out[2] = in[2];
}

typedef struct {
    Y3State  pent_A;           /* f0-f2, upright */
    Y3State  pent_B;           /* f3-f5, rot+36° */
    float    g10_center[3];
    float    g10_shift;
    float    conf;
    uint8_t  face;
    uint8_t  compound;
    uint8_t  pad[2];
} G10State;

static inline void g10_init(G10State *s, const char *name) {
    char buf[128];
    int nl = (int)strlen(name);
    if (nl > 118) nl = 118;

    memcpy(buf, name, nl);
    memcpy(buf+nl, "_G1_A", 6);
    y3_init(&s->pent_A, buf);
    s->pent_A.phase = 0;

    memcpy(buf, name, nl);
    memcpy(buf+nl, "_G1_B", 6);
    y3_init(&s->pent_B, buf);
    s->pent_B.phase = 1;

    float c = cosf(0.6283185f), si = sinf(0.6283185f);
    for (int i = 0; i < 3; i++) {
        float x = s->pent_B.dirs[i][0], y = s->pent_B.dirs[i][1];
        s->pent_B.dirs[i][0] = x*c - y*si;
        s->pent_B.dirs[i][1] = x*si + y*c;
    }
    _m33_invert(s->pent_B.dirs_inv, s->pent_B.dirs);

    memset(s->g10_center, 0, sizeof(s->g10_center));
    s->g10_shift = 0.f;
    s->conf = 1.f;
    s->face = 0;
    s->compound = 0;
}

static inline void g10_snapshot(G10State *s) {
    y3_snapshot(&s->pent_A);
    y3_snapshot(&s->pent_B);
    memset(s->g10_center, 0, sizeof(s->g10_center));
    s->g10_shift = 0.f;
    s->face = 0;
    s->compound = 0;
}

/* project vertices for both pentagons */
static inline void _g10_project(G10State *s, float vA[5][3], float vB[5][3]) {
    for (int i = 0; i < G10_VERT; i++) {
        float a = i * 1.25663706f;
        _g10_rot_z(vA[i], s->pent_A.centroid, a);
        _g10_rot_z(vB[i], s->pent_B.centroid, a + 0.6283185f);
    }
}

/* ── G10 Mode 1: 10 tri centroids only ── */
static inline void g10m1_encode(G10State *s,
                                 float f0, float f1, float f2,
                                 float f3, float f4, float f5) {
    y3_encode(&s->pent_A, f0, f1, f2);
    y3_encode(&s->pent_B, f3, f4, f5);

    float vA[5][3], vB[5][3];
    _g10_project(s, vA, vB);

    float sum[3] = {0,0,0};
    for (int i = 0; i < G10_VERT; i++) {
        int ni = (i + 1) % G10_VERT;
        /* tri[2i] = (vA[i], vB[i], vA[ni]) */
        sum[0] += vA[i][0] + vB[i][0] + vA[ni][0];
        sum[1] += vA[i][1] + vB[i][1] + vA[ni][1];
        sum[2] += vA[i][2] + vB[i][2] + vA[ni][2];
        /* tri[2i+1] = (vB[i], vA[ni], vB[ni]) */
        sum[0] += vB[i][0] + vA[ni][0] + vB[ni][0];
        sum[1] += vB[i][1] + vA[ni][1] + vB[ni][1];
        sum[2] += vB[i][2] + vA[ni][2] + vB[ni][2];
    }
    s->g10_center[0] = sum[0] / (G10_TRI * 3);
    s->g10_center[1] = sum[1] / (G10_TRI * 3);
    s->g10_center[2] = sum[2] / (G10_TRI * 3);
    s->g10_shift = _v3_norm(s->g10_center);

    /* confidence from tri centroid variance */
    float tri_c[G10_TRI][3];
    for (int i = 0; i < G10_VERT; i++) {
        int ni = (i + 1) % G10_VERT;
        tri_c[2*i][0] = (vA[i][0] + vB[i][0] + vA[ni][0]) / 3.f;
        tri_c[2*i][1] = (vA[i][1] + vB[i][1] + vA[ni][1]) / 3.f;
        tri_c[2*i][2] = (vA[i][2] + vB[i][2] + vA[ni][2]) / 3.f;
        tri_c[2*i+1][0] = (vB[i][0] + vA[ni][0] + vB[ni][0]) / 3.f;
        tri_c[2*i+1][1] = (vB[i][1] + vA[ni][1] + vB[ni][1]) / 3.f;
        tri_c[2*i+1][2] = (vB[i][2] + vA[ni][2] + vB[ni][2]) / 3.f;
    }
    float mean[3] = {0}; float var = 0.f;
    for (int i = 0; i < G10_TRI; i++) {
        mean[0] += tri_c[i][0]; mean[1] += tri_c[i][1]; mean[2] += tri_c[i][2];
    }
    mean[0]/=G10_TRI; mean[1]/=G10_TRI; mean[2]/=G10_TRI;
    for (int i = 0; i < G10_TRI; i++) {
        float d = (tri_c[i][0]-mean[0])*(tri_c[i][0]-mean[0])
                + (tri_c[i][1]-mean[1])*(tri_c[i][1]-mean[1])
                + (tri_c[i][2]-mean[2])*(tri_c[i][2]-mean[2]);
        var += d;
    }
    s->conf = 1.f / (1.f + var/G10_TRI);
    s->face     = _vec_to_face(s->g10_center);
    s->compound = _shift_to_compound(s->g10_shift);
}

/* ── G10 Mode 2: 10 tri centroids + 10 free tips ── */
static inline void g10m2_encode(G10State *s,
                                 float f0, float f1, float f2,
                                 float f3, float f4, float f5) {
    y3_encode(&s->pent_A, f0, f1, f2);
    y3_encode(&s->pent_B, f3, f4, f5);

    float vA[5][3], vB[5][3];
    _g10_project(s, vA, vB);

    float sum[3] = {0,0,0};
    for (int i = 0; i < G10_VERT; i++) {
        int ni = (i + 1) % G10_VERT;
        /* tri[2i] centroid = (vA[i]+vB[i]+vA[ni])/3, tip = vA[ni] */
        sum[0] += vA[i][0] + vB[i][0] + vA[ni][0] + vA[ni][0]*3;
        sum[1] += vA[i][1] + vB[i][1] + vA[ni][1] + vA[ni][1]*3;
        sum[2] += vA[i][2] + vB[i][2] + vA[ni][2] + vA[ni][2]*3;
        /* tri[2i+1] centroid = (vB[i]+vA[ni]+vB[ni])/3, tip = vB[ni] */
        sum[0] += vB[i][0] + vA[ni][0] + vB[ni][0] + vB[ni][0]*3;
        sum[1] += vB[i][1] + vA[ni][1] + vB[ni][1] + vB[ni][1]*3;
        sum[2] += vB[i][2] + vA[ni][2] + vB[ni][2] + vB[ni][2]*3;
    }

    /* centroid: 10 tri centroids (each /3) + 10 tips */
    /* tri numerator = sum of 3 vertices, tip = 1 vertex */
    /* total numerator = sum of (3v+1v) per tri = sum of 4v per tri = 40v total */
    /* each / (10*4) = /40 for average */
    s->g10_center[0] = sum[0] / 40.f;
    s->g10_center[1] = sum[1] / 40.f;
    s->g10_center[2] = sum[2] / 40.f;
    s->g10_shift = _v3_norm(s->g10_center);

    /* confidence from tri centroid variance (same as m1) */
    float tri_c[G10_TRI][3];
    for (int i = 0; i < G10_VERT; i++) {
        int ni = (i + 1) % G10_VERT;
        tri_c[2*i][0] = (vA[i][0] + vB[i][0] + vA[ni][0]) / 3.f;
        tri_c[2*i][1] = (vA[i][1] + vB[i][1] + vA[ni][1]) / 3.f;
        tri_c[2*i][2] = (vA[i][2] + vB[i][2] + vA[ni][2]) / 3.f;
        tri_c[2*i+1][0] = (vB[i][0] + vA[ni][0] + vB[ni][0]) / 3.f;
        tri_c[2*i+1][1] = (vB[i][1] + vA[ni][1] + vB[ni][1]) / 3.f;
        tri_c[2*i+1][2] = (vB[i][2] + vA[ni][2] + vB[ni][2]) / 3.f;
    }
    float mean[3] = {0}; float var = 0.f;
    for (int i = 0; i < G10_TRI; i++) {
        mean[0] += tri_c[i][0]; mean[1] += tri_c[i][1]; mean[2] += tri_c[i][2];
    }
    mean[0]/=G10_TRI; mean[1]/=G10_TRI; mean[2]/=G10_TRI;
    for (int i = 0; i < G10_TRI; i++) {
        float d = (tri_c[i][0]-mean[0])*(tri_c[i][0]-mean[0])
                + (tri_c[i][1]-mean[1])*(tri_c[i][1]-mean[1])
                + (tri_c[i][2]-mean[2])*(tri_c[i][2]-mean[2]);
        var += d;
    }
    s->conf = 1.f / (1.f + var/G10_TRI);
    s->face     = _vec_to_face(s->g10_center);
    s->compound = _shift_to_compound(s->g10_shift);
}

#endif /* CTD_G10_H */
