/*
 * ctd_goldberg.h — Goldberg GT2 Tri+Tip Centroid Encoder
 *
 * Design: 1 pentagon + 5 hex → tri+tip split per hex
 *   tri   = hex_dir[0]*f3 + hex_dir[1]*f4   (2 directions, triangle face)
 *   tip   = hex_dir[2]*f5                    (1 direction, free vertex)
 *   Weight: pent×5 + (tri+tip)×1 per hex = 15 total
 *
 * Benchmark (10 tensors, vs Y6/P5H/SH):
 *   GT2 avg shift = 0.3550  (wins 6/10)
 *   GB  avg shift = 0.6587  (wins 1/10)
 *   SH  avg shift = 0.6323  (wins 1/10)
 *
 * Vision tensor (mlp.fc2): GT2=0.727 vs GB=3.119 (4× better)
 *
 * Rules:
 *   - No malloc, pure stack
 *   - O(1) encode/recon
 *   - Depends on ctd_octa.h (Y3State, y3_init, y3_encode, y3_recon)
 */

#ifndef CTD_GOLDBERG_H
#define CTD_GOLDBERG_H

#include "ctd_octa.h"
#include <math.h>
#include <string.h>

/* ── Constants ── */
#define GB_HEX_COUNT   5
#define GB_PENT_W      5.0f    /* pentagon weight in centroid avg */
#define GB_HEX_W       2.0f   /* each hex = tri(2dirs) + tip(1dir) */
#define GB_TOTAL_W     15.0f  /* 5 + 5×2 */
#define GB_HEX_STEP    1.2566370f  /* 72° = 2π/5 between hex centers */

/* ── State ── */
typedef struct {
    Y3State  pent;           /* center pentagon — content features */
    Y3State  hex[5];         /* 5 hex ring — metadata features */
    float    gb_center[3];   /* weighted centroid */
    float    gb_shift;       /* |gb_center| */
    float    conf;           /* hex ring variance → stability */
    uint8_t  face;           /* 0-11 dodeca face */
    uint8_t  compound;       /* 0-2 temporal tier */
    uint8_t  pad[2];
} GBState;

/* ── Internal: rotate Y3 dirs around Z axis ── */
static inline void _gb_rotate_z(Y3State *s, float angle) {
    float c = cosf(angle), si = sinf(angle);
    for (int i = 0; i < 3; i++) {
        float x = s->dirs[i][0], y = s->dirs[i][1];
        s->dirs[i][0] = x*c - y*si;
        s->dirs[i][1] = x*si + y*c;
        /* z unchanged */
    }
    _m33_invert(s->dirs_inv, s->dirs);
}

/* ── Init from tensor name ── */
static inline void gb_init(GBState *s, const char *name) {
    char buf[128];
    int nl = (int)strlen(name);
    if (nl > 118) nl = 118;

    /* Pentagon center */
    memcpy(buf, name, nl);
    memcpy(buf+nl, "_GB_P", 6);
    y3_init(&s->pent, buf);
    s->pent.phase = 0;

    /* 5 hex ring: each rotated 72°×i */
    for (int i = 0; i < GB_HEX_COUNT; i++) {
        memcpy(buf, name, nl);
        buf[nl]   = '_'; buf[nl+1] = 'G'; buf[nl+2] = 'B';
        buf[nl+3] = '_'; buf[nl+4] = 'H'; buf[nl+5] = '0'+i;
        buf[nl+6] = '\0';
        y3_init(&s->hex[i], buf);
        s->hex[i].phase = 1;
        _gb_rotate_z(&s->hex[i], i * GB_HEX_STEP);
    }

    memset(s->gb_center, 0, sizeof(s->gb_center));
    s->gb_shift = 0.f;
    s->conf     = 1.f;
    s->face     = 0;
    s->compound = 0;
}

/* ── Snapshot (freeze origin) ── */
static inline void gb_snapshot(GBState *s) {
    y3_snapshot(&s->pent);
    for (int i = 0; i < GB_HEX_COUNT; i++) y3_snapshot(&s->hex[i]);
    memset(s->gb_center, 0, sizeof(s->gb_center));
    s->gb_shift = 0.f;
    s->face     = 0;
    s->compound = 0;
}

/* ── Encode 6 features → Goldberg centroid ── */
/*
 * f0-f2: content features → pentagon center
 * f3-f5: metadata features → all 5 hex ring (tri+tip)
 *
 * Weighted centroid = (pent×5 + Σ(tri+tip)) / 15
 */
static inline void gb_encode(GBState *s,
                              float f0, float f1, float f2,
                              float f3, float f4, float f5) {
    /* Pentagon */
    y3_encode(&s->pent, f0, f1, f2);

    /* 5 hex ring — same features, different rotated dirs */
    for (int i = 0; i < GB_HEX_COUNT; i++)
        y3_encode(&s->hex[i], f3, f4, f5);

    /* GT2 tri+tip split: tri = dir[0]*f3 + dir[1]*f4, tip = dir[2]*f5 */
    float sum[3] = {
        s->pent.centroid[0] * GB_PENT_W,
        s->pent.centroid[1] * GB_PENT_W,
        s->pent.centroid[2] * GB_PENT_W
    };
    for (int i = 0; i < GB_HEX_COUNT; i++) {
        Y3State *h = &s->hex[i];
        sum[0] += h->dirs[0][0]*f3 + h->dirs[1][0]*f4;  /* tri */
        sum[1] += h->dirs[0][1]*f3 + h->dirs[1][1]*f4;
        sum[2] += h->dirs[0][2]*f3 + h->dirs[1][2]*f4;
        sum[0] += h->dirs[2][0]*f5;                       /* tip */
        sum[1] += h->dirs[2][1]*f5;
        sum[2] += h->dirs[2][2]*f5;
    }
    s->gb_center[0] = sum[0] / GB_TOTAL_W;
    s->gb_center[1] = sum[1] / GB_TOTAL_W;
    s->gb_center[2] = sum[2] / GB_TOTAL_W;
    s->gb_shift = _v3_norm(s->gb_center);

    /* Confidence: inverse of hex shift variance */
    float mean_hex = 0.f;
    for (int i = 0; i < GB_HEX_COUNT; i++) mean_hex += s->hex[i].shift;
    mean_hex /= GB_HEX_COUNT;
    float var = 0.f;
    for (int i = 0; i < GB_HEX_COUNT; i++) {
        float d = s->hex[i].shift - mean_hex;
        var += d*d;
    }
    s->conf = 1.f / (1.f + var/GB_HEX_COUNT);

    s->face     = _vec_to_face(s->gb_center);
    s->compound = _shift_to_compound(s->gb_shift);
}

/* ── GBT: original GB dirs but triangle-only hex (2 dirs, no f5) ── */
static inline void gbt_encode(GBState *s,
                               float f0, float f1, float f2,
                               float f3, float f4, float f5) {
    y3_encode(&s->pent, f0, f1, f2);

    float sum[3] = {
        s->pent.centroid[0] * GB_PENT_W,
        s->pent.centroid[1] * GB_PENT_W,
        s->pent.centroid[2] * GB_PENT_W
    };
    for (int i = 0; i < GB_HEX_COUNT; i++) {
        Y3State *h = &s->hex[i];
        /* tri centroid = dir[0]*f3 + dir[1]*f4 (skip dir[2], no f5) */
        for (int k = 0; k < 3; k++)
            sum[k] += h->dirs[0][k] * f3 + h->dirs[1][k] * f4;
    }
    s->gb_center[0] = sum[0] / (GB_PENT_W + GB_HEX_COUNT);
    s->gb_center[1] = sum[1] / (GB_PENT_W + GB_HEX_COUNT);
    s->gb_center[2] = sum[2] / (GB_PENT_W + GB_HEX_COUNT);
    s->gb_shift = _v3_norm(s->gb_center);

    float mean_h = 0.f;
    for (int i = 0; i < GB_HEX_COUNT; i++) mean_h += s->hex[i].shift;
    mean_h /= GB_HEX_COUNT;
    float var = 0.f;
    for (int i = 0; i < GB_HEX_COUNT; i++) {
        float d = s->hex[i].shift - mean_h;
        var += d*d;
    }
    s->conf = 1.f / (1.f + var/GB_HEX_COUNT);

    s->face     = _vec_to_face(s->gb_center);
    s->compound = _shift_to_compound(s->gb_shift);
}

/* ── GBT2: original GB dirs but tri+tip split per hex (same as GT2) ── */
static inline void gbt2_encode(GBState *s,
                                float f0, float f1, float f2,
                                float f3, float f4, float f5) {
    y3_encode(&s->pent, f0, f1, f2);

    float sum[3] = {
        s->pent.centroid[0] * GB_PENT_W,
        s->pent.centroid[1] * GB_PENT_W,
        s->pent.centroid[2] * GB_PENT_W
    };
    for (int i = 0; i < GB_HEX_COUNT; i++) {
        Y3State *h = &s->hex[i];
        sum[0] += h->dirs[0][0] * f3 + h->dirs[1][0] * f4;
        sum[1] += h->dirs[0][1] * f3 + h->dirs[1][1] * f4;
        sum[2] += h->dirs[0][2] * f3 + h->dirs[1][2] * f4;
        sum[0] += h->dirs[2][0] * f5;
        sum[1] += h->dirs[2][1] * f5;
        sum[2] += h->dirs[2][2] * f5;
    }
    s->gb_center[0] = sum[0] / (GB_PENT_W + GB_HEX_COUNT*2);
    s->gb_center[1] = sum[1] / (GB_PENT_W + GB_HEX_COUNT*2);
    s->gb_center[2] = sum[2] / (GB_PENT_W + GB_HEX_COUNT*2);
    s->gb_shift = _v3_norm(s->gb_center);

    float mean_h = 0.f;
    for (int i = 0; i < GB_HEX_COUNT; i++) mean_h += s->hex[i].shift;
    mean_h /= GB_HEX_COUNT;
    float var = 0.f;
    for (int i = 0; i < GB_HEX_COUNT; i++) {
        float d = s->hex[i].shift - mean_h;
        var += d*d;
    }
    s->conf = 1.f / (1.f + var/GB_HEX_COUNT);

    s->face     = _vec_to_face(s->gb_center);
    s->compound = _shift_to_compound(s->gb_shift);
}

/* ── Reconstruct 6 features ── */
static inline void gb_recon(const GBState *s, float out[6]) {
    y3_recon(&s->pent,     out);     /* f0-f2 from pentagon */
    y3_recon(&s->hex[0],   out+3);  /* f3-f5 from first hex */
}

/* ── Verify: same name+features → same face ── */
static inline int gb_verify(const GBState *a, const GBState *b) {
    return (a->face == b->face) && (a->compound == b->compound);
}

#endif /* CTD_GOLDBERG_H */
