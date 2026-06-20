/*
 * ctd_shell.h — Dodeca Shell Cross Centroid
 *
 * 2 pentagons, 36° apart → 10 shell vertices
 * Cross-connect: a[N] → b[(N+3)%5]
 *
 * Geometry:
 *   a[i] @ 72°×i       (pent_A, upright)
 *   b[i] @ 72°×i + 36° (pent_B, rotated)
 *
 *   edge[i] = (a[i] + b[(i+3)%5]) / 2
 *   centroid = avg(edge[0..4], pent_A, pent_B) / 12
 */
#ifndef CTD_SHELL_H
#define CTD_SHELL_H

#include "ctd_octa.h"
#include <math.h>
#include <string.h>

#define SHELL_VERT  5
#define SHELL_EDGE  5
#define SHELL_TOTAL 12  /* 5 edge mid + 5 pentA + 2 centers */

/* single cross offset = 3 → gcd(10,3)=1 → connected */
#define SHELL_CROSS 3

static inline void _shell_rot_z(float out[3], float in[3], float angle)
{
    float c = cosf(angle), si = sinf(angle);
    out[0] = in[0]*c - in[1]*si;
    out[1] = in[0]*si + in[1]*c;
    out[2] = in[2];
}

typedef struct {
    Y3State  pent_A;           /* f0,f1,f2 — content */
    Y3State  pent_B;           /* f3,f4,f5 — metadata, rot+36° */
    float    centroid[3];      /* shell weighted centroid */
    float    shift;            /* |centroid| */
    float    conf;             /* edge variance */
    uint8_t  face;             /* 0-11 dodeca */
    uint8_t  compound;         /* 0-2 */
    uint8_t  pad[2];
} ShellState;

static inline void shell_init(ShellState *s, const char *name)
{
    char buf[128];
    int nl = (int)strlen(name);
    if (nl > 118) nl = 118;

    memcpy(buf, name, nl);
    memcpy(buf+nl, "_SH_A", 6);
    y3_init(&s->pent_A, buf);
    s->pent_A.phase = 0;

    memcpy(buf, name, nl);
    memcpy(buf+nl, "_SH_B", 6);
    y3_init(&s->pent_B, buf);
    s->pent_B.phase = 1;

    /* rotate pent_B dirs by 36° */
    float c = cosf(0.6283185f), si = sinf(0.6283185f);
    for (int i = 0; i < 3; i++) {
        float x = s->pent_B.dirs[i][0], y = s->pent_B.dirs[i][1];
        s->pent_B.dirs[i][0] = x*c - y*si;
        s->pent_B.dirs[i][1] = x*si + y*c;
    }
    _m33_invert(s->pent_B.dirs_inv, s->pent_B.dirs);

    s->centroid[0] = s->centroid[1] = s->centroid[2] = 0.0f;
    s->shift = 0.0f;
    s->conf = 1.0f;
    s->face = 0;
    s->compound = 0;
}

static inline void shell_snapshot(ShellState *s)
{
    y3_snapshot(&s->pent_A);
    y3_snapshot(&s->pent_B);
    s->centroid[0] = s->centroid[1] = s->centroid[2] = 0.0f;
    s->shift = 0.0f;
    s->face = 0;
    s->compound = 0;
}

static inline void shell_encode(ShellState *s,
                                 float f0, float f1, float f2,
                                 float f3, float f4, float f5)
{
    y3_encode(&s->pent_A, f0, f1, f2);
    y3_encode(&s->pent_B, f3, f4, f5);

    /* project pent_A centroid through 5 vertices at 72° × i */
    float vert_A[5][3], vert_B[5][3];

    for (int i = 0; i < SHELL_VERT; i++) {
        float a = i * 1.25663706f;
        _shell_rot_z(vert_A[i], s->pent_A.centroid, a);
        _shell_rot_z(vert_B[i], s->pent_B.centroid, a + 0.6283185f);
    }

    /* cross-connect edges: a[i] → b[(i+3)%5] */
    float edges[5][3];
    for (int i = 0; i < SHELL_EDGE; i++) {
        int j = (i + SHELL_CROSS) % SHELL_VERT;
        edges[i][0] = (vert_A[i][0] + vert_B[j][0]) * 0.5f;
        edges[i][1] = (vert_A[i][1] + vert_B[j][1]) * 0.5f;
        edges[i][2] = (vert_A[i][2] + vert_B[j][2]) * 0.5f;
    }

    /* pair centroids: adjacent edge pairs → hex-like regions */
    float pair[5][3];
    for (int i = 0; i < SHELL_EDGE; i++) {
        int ni = (i + 1) % SHELL_EDGE;
        pair[i][0] = (edges[i][0] + edges[ni][0]) * 0.5f;
        pair[i][1] = (edges[i][1] + edges[ni][1]) * 0.5f;
        pair[i][2] = (edges[i][2] + edges[ni][2]) * 0.5f;
    }

    float sum[3] = {0,0,0};
    for (int i = 0; i < SHELL_EDGE; i++) {
        sum[0] += pair[i][0];
        sum[1] += pair[i][1];
        sum[2] += pair[i][2];
    }

    /* add pentagon centers */
    sum[0] += s->pent_A.centroid[0] + s->pent_B.centroid[0];
    sum[1] += s->pent_A.centroid[1] + s->pent_B.centroid[1];
    sum[2] += s->pent_A.centroid[2] + s->pent_B.centroid[2];

    s->centroid[0] = sum[0] / 7.0f;  /* 5 pairs + 2 centers */
    s->centroid[1] = sum[1] / 7.0f;
    s->centroid[2] = sum[2] / 7.0f;
    s->shift = _v3_norm(s->centroid);

    /* confidence: inverse of pair variance */
    float mean_len = 0.0f;
    float edge_len[5];
    for (int i = 0; i < SHELL_EDGE; i++) {
        int j = (i + SHELL_CROSS) % SHELL_VERT;
        float d[3] = {
            vert_A[i][0] - vert_B[j][0],
            vert_A[i][1] - vert_B[j][1],
            vert_A[i][2] - vert_B[j][2]
        };
        edge_len[i] = _v3_norm(d);
        mean_len += edge_len[i];
    }
    mean_len /= SHELL_EDGE;
    float var = 0.0f;
    for (int i = 0; i < SHELL_EDGE; i++) {
        float d = edge_len[i] - mean_len;
        var += d*d;
    }
    var /= SHELL_EDGE;
    s->conf = 1.0f / (1.0f + var);

    s->face = _vec_to_face(s->centroid);
    s->compound = _shift_to_compound(s->shift);
}

static inline void shell_recon(const ShellState *s, float out[6])
{
    y3_recon(&s->pent_A, out);     /* f0-f2 */
    y3_recon(&s->pent_B, out+3);  /* f3-f5 */
}

#endif /* CTD_SHELL_H */
