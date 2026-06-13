/*
 * ctd_goldberg_tri.h — Goldberg Triangle Variant
 *
 * Instead of full hex centroids (3 Y3 dirs), take 1 triangle per hex:
 *   2 dirs → triangle centroid,  1 dir → tip vertex
 *
 * GT1: 5 tri centroids + pentagon (w=5)  → total weight 10
 * GT2: 5 tri + 5 tips + pentagon (w=5)   → total weight 15
 */

#ifndef CTD_GOLDBERG_TRI_H
#define CTD_GOLDBERG_TRI_H

#include "ctd_octa.h"
#include <math.h>
#include <string.h>

#define GBT_HEX_COUNT   5
#define GBT_STEP        1.2566370f

typedef struct {
    Y3State  pent;
    Y3State  hex[5];           /* full Y3, but use subset of dirs */
    float    gt_center[3];
    float    gt_shift;
    float    conf;
    uint8_t  face;
    uint8_t  compound;
    uint8_t  pad[2];
} GTState;

static inline void _gt_rotate_z(Y3State *s, float angle) {
    float c = cosf(angle), si = sinf(angle);
    for (int i = 0; i < 3; i++) {
        float x = s->dirs[i][0], y = s->dirs[i][1];
        s->dirs[i][0] = x*c - y*si;
        s->dirs[i][1] = x*si + y*c;
    }
    _m33_invert(s->dirs_inv, s->dirs);
}

static inline void gt_init(GTState *s, const char *name) {
    char buf[128];
    int nl = (int)strlen(name);
    if (nl > 118) nl = 118;

    memcpy(buf, name, nl);
    memcpy(buf+nl, "_GB_P", 6);
    y3_init(&s->pent, buf);
    s->pent.phase = 0;

    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        memcpy(buf, name, nl);
        buf[nl]='_'; buf[nl+1]='G'; buf[nl+2]='T';
        buf[nl+3]='_'; buf[nl+4]='H'; buf[nl+5]='0'+i;
        buf[nl+6]='\0';
        y3_init(&s->hex[i], buf);
        s->hex[i].phase = 1;
        _gt_rotate_z(&s->hex[i], i * GBT_STEP);
    }

    memset(s->gt_center, 0, sizeof(s->gt_center));
    s->gt_shift = 0.f;
    s->conf = 1.f;
    s->face = 0;
    s->compound = 0;
}

static inline void gt_snapshot(GTState *s) {
    y3_snapshot(&s->pent);
    for (int i = 0; i < GBT_HEX_COUNT; i++) y3_snapshot(&s->hex[i]);
    memset(s->gt_center, 0, sizeof(s->gt_center));
    s->gt_shift = 0.f;
    s->face = 0;
    s->compound = 0;
}

/* ── GT1: 5 triangle centroids (2 dirs per hex) + pentagon ── */
static inline void gt1_encode(GTState *s,
                               float f0, float f1, float f2,
                               float f3, float f4, float f5) {
    y3_encode(&s->pent, f0, f1, f2);

    /* hex tri centroid = dir[0]*f3 + dir[1]*f4 (skip dir[2], no f5) */
    float sum[3] = {
        s->pent.centroid[0] * 5.f,
        s->pent.centroid[1] * 5.f,
        s->pent.centroid[2] * 5.f
    };
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        Y3State *h = &s->hex[i];
        float tri[3];
        for (int k = 0; k < 3; k++)
            tri[k] = h->dirs[0][k] * f3 + h->dirs[1][k] * f4;
        sum[0] += tri[0];
        sum[1] += tri[1];
        sum[2] += tri[2];
    }

    s->gt_center[0] = sum[0] / 10.f;
    s->gt_center[1] = sum[1] / 10.f;
    s->gt_center[2] = sum[2] / 10.f;
    s->gt_shift = _v3_norm(s->gt_center);

    float mean_hex = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) mean_hex += s->hex[i].shift;
    mean_hex /= GBT_HEX_COUNT;
    float var = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        float d = s->hex[i].shift - mean_hex;
        var += d*d;
    }
    s->conf = 1.f / (1.f + var/GBT_HEX_COUNT);

    s->face     = _vec_to_face(s->gt_center);
    s->compound = _shift_to_compound(s->gt_shift);
}

/* ── GT2: 5 tri centroids + 5 tips + pentagon ── */
static inline void gt2_encode(GTState *s,
                               float f0, float f1, float f2,
                               float f3, float f4, float f5) {
    y3_encode(&s->pent, f0, f1, f2);

    float sum[3] = {
        s->pent.centroid[0] * 5.f,
        s->pent.centroid[1] * 5.f,
        s->pent.centroid[2] * 5.f
    };
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        Y3State *h = &s->hex[i];
        /* tri centroid = dir[0]*f3 + dir[1]*f4 */
        sum[0] += h->dirs[0][0] * f3 + h->dirs[1][0] * f4;
        sum[1] += h->dirs[0][1] * f3 + h->dirs[1][1] * f4;
        sum[2] += h->dirs[0][2] * f3 + h->dirs[1][2] * f4;
        /* tip = dir[2]*f5 */
        sum[0] += h->dirs[2][0] * f5;
        sum[1] += h->dirs[2][1] * f5;
        sum[2] += h->dirs[2][2] * f5;
    }

    s->gt_center[0] = sum[0] / 15.f;
    s->gt_center[1] = sum[1] / 15.f;
    s->gt_center[2] = sum[2] / 15.f;
    s->gt_shift = _v3_norm(s->gt_center);

    float mean_hex = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) mean_hex += s->hex[i].shift;
    mean_hex /= GBT_HEX_COUNT;
    float var = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        float d = s->hex[i].shift - mean_hex;
        var += d*d;
    }
    s->conf = 1.f / (1.f + var/GBT_HEX_COUNT);

    s->face     = _vec_to_face(s->gt_center);
    s->compound = _shift_to_compound(s->gt_shift);
}

/* ── GT3: same as GT2 but pentagon weight = 1 (equal to tri/tip) ── */
static inline void gt3_encode(GTState *s,
                               float f0, float f1, float f2,
                               float f3, float f4, float f5) {
    y3_encode(&s->pent, f0, f1, f2);

    float sum[3] = {
        s->pent.centroid[0],
        s->pent.centroid[1],
        s->pent.centroid[2]
    };
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        Y3State *h = &s->hex[i];
        sum[0] += h->dirs[0][0] * f3 + h->dirs[1][0] * f4;
        sum[1] += h->dirs[0][1] * f3 + h->dirs[1][1] * f4;
        sum[2] += h->dirs[0][2] * f3 + h->dirs[1][2] * f4;
        sum[0] += h->dirs[2][0] * f5;
        sum[1] += h->dirs[2][1] * f5;
        sum[2] += h->dirs[2][2] * f5;
    }
    /* pent=1, each hex contributes 2 (tri+tip) = 10, total=11 */
    s->gt_center[0] = sum[0] / 11.f;
    s->gt_center[1] = sum[1] / 11.f;
    s->gt_center[2] = sum[2] / 11.f;
    s->gt_shift = _v3_norm(s->gt_center);

    float mean_hex = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) mean_hex += s->hex[i].shift;
    mean_hex /= GBT_HEX_COUNT;
    float var = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        float d = s->hex[i].shift - mean_hex;
        var += d*d;
    }
    s->conf = 1.f / (1.f + var/GBT_HEX_COUNT);

    s->face     = _vec_to_face(s->gt_center);
    s->compound = _shift_to_compound(s->gt_shift);
}

/* ── GT4: tri+tip only, NO pentagon ── */
static inline void gt4_encode(GTState *s,
                               float f0, float f1, float f2,
                               float f3, float f4, float f5) {
    y3_encode(&s->pent, f0, f1, f2);

    float sum[3] = {0,0,0};
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        Y3State *h = &s->hex[i];
        sum[0] += h->dirs[0][0] * f3 + h->dirs[1][0] * f4 + h->dirs[2][0] * f5;
        sum[1] += h->dirs[0][1] * f3 + h->dirs[1][1] * f4 + h->dirs[2][1] * f5;
        sum[2] += h->dirs[0][2] * f3 + h->dirs[1][2] * f4 + h->dirs[2][2] * f5;
    }
    s->gt_center[0] = sum[0] / 5.f;
    s->gt_center[1] = sum[1] / 5.f;
    s->gt_center[2] = sum[2] / 5.f;
    s->gt_shift = _v3_norm(s->gt_center);

    float mean_hex = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) mean_hex += s->hex[i].shift;
    mean_hex /= GBT_HEX_COUNT;
    float var = 0.f;
    for (int i = 0; i < GBT_HEX_COUNT; i++) {
        float d = s->hex[i].shift - mean_hex;
        var += d*d;
    }
    s->conf = 1.f / (1.f + var/GBT_HEX_COUNT);

    s->face     = _vec_to_face(s->gt_center);
    s->compound = _shift_to_compound(s->gt_shift);
}

#endif /* CTD_GOLDBERG_TRI_H */
