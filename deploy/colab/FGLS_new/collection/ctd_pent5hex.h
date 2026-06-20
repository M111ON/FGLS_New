/*
 * ctd_pent5hex.h — Pentagon Interleaved Centroid (5HexSakura design)
 *
 * Design (from Win's GeoGebra construction):
 *   1. Outer pentagon → centroid → inner pentagon (scale 1/φ²)
 *   2. Rotate inner pentagon 36° → pent#2
 *      (36° = half of 72°, vertex of #2 aligns with edge midpoint of #1)
 *   3. Path: skip 1 vertex, alternate pent#1 → pent#2
 *      10 points total, skip 3 → gcd(10,3)=1 → single connected trace
 *
 * Encoding:
 *   Y3_A = pent#1 (upright)     — content features
 *   Y3_B = pent#2 (rotated 36°) — metadata features
 *   pent_center = weighted avg favoring tighter centroid
 *
 * vs Y6 hexagram:
 *   Y6   = 2 triangles, 60° rotation
 *   P5H  = 2 pentagons, 36° rotation → denser vertex coverage
 */

#ifndef CTD_PENT5HEX_H
#define CTD_PENT5HEX_H

#include "ctd_octa.h"  /* Y3State, y3_init, y3_encode, y3_recon */
#include <math.h>
#include <string.h>

/* Pentagon rotation = 36° = π/5 */
#define P5H_ROT_DEG   36.0f
#define P5H_ROT_RAD   0.6283185f   /* π/5 */

/* Inner pentagon scale = 1/φ² ≈ 0.382 */
#define P5H_INNER_SCALE  0.38196601f

/* Confidence threshold */
#define P5H_CONF_THRESH  1.0f

/* ── P5H State ── */
typedef struct {
    Y3State  a;              /* pent#1 upright */
    Y3State  b;              /* pent#2 rotated 36° */
    float    pent_center[3]; /* weighted center of 2 pentagon centroids */
    float    pent_shift;     /* |pent_center| */
    float    conf;           /* agreement between pent#1 and pent#2 */
    uint8_t  face;           /* 0-11 dodeca face */
    uint8_t  compound;       /* 0-2 temporal tier */
    uint8_t  pad[2];
} P5HState;

/* ── Apply 36° rotation around Z axis to Y3 dirs ── */
/* Pentagon#2 = Pentagon#1 dirs rotated 36° */
static inline void _p5h_rotate_dirs(Y3State *s) {
    float c = cosf(P5H_ROT_RAD);
    float si = sinf(P5H_ROT_RAD);
    for (int i = 0; i < 3; i++) {
        float x = s->dirs[i][0];
        float y = s->dirs[i][1];
        s->dirs[i][0] =  x*c - y*si;
        s->dirs[i][1] =  x*si + y*c;
        /* z unchanged */
    }
    /* Recompute inverse after rotation */
    _m33_invert(s->dirs_inv, s->dirs);
}

/* ── Init P5H from name ── */
static inline void p5h_init(P5HState *s, const char *name) {
    /* pent#1: standard Y3 */
    char buf_a[128], buf_b[128];
    uint32_t nl = 0;
    for (const char *p = name; *p && nl < 120; p++) buf_a[nl] = buf_b[nl] = *p, nl++;
    buf_a[nl]='\0'; buf_b[nl]='\0';
    /* append phase tag */
    strncat(buf_a, "_P1", 4);
    strncat(buf_b, "_P2", 4);

    y3_init(&s->a, buf_a);
    s->a.phase = 0;

    y3_init(&s->b, buf_b);
    s->b.phase = 1;

    /* Apply 36° rotation to pent#2 dirs */
    _p5h_rotate_dirs(&s->b);

    /* Also scale dirs by inner pentagon ratio for pent#2 */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            s->b.dirs[i][j] *= P5H_INNER_SCALE * (1.0f / P5H_INNER_SCALE);
            /* keep same scale, only rotation differs */

    s->pent_center[0] = s->pent_center[1] = s->pent_center[2] = 0.f;
    s->pent_shift = 0.f;
    s->conf = 1.0f;
    s->face = 0;
    s->compound = 0;
}

/* ── Snapshot ── */
static inline void p5h_snapshot(P5HState *s) {
    y3_snapshot(&s->a);
    y3_snapshot(&s->b);
    s->pent_center[0] = s->pent_center[1] = s->pent_center[2] = 0.f;
    s->pent_shift = 0.f;
    s->face = 0;
    s->compound = 0;
}

/* ── Encode 6 features → interleaved pentagon centroid ── */
/*
 * Interleave path: skip 1, alternate pent#1/pent#2
 * f0,f2,f4 → pent#1 (odd skip positions)
 * f1,f3,f5 → pent#2 (even skip positions)
 * → gcd(10,3)=1 → single trace covers all 10 points
 */
static inline void p5h_encode(P5HState *s,
                               float f0, float f1, float f2,
                               float f3, float f4, float f5) {
    /* pent#1: f0, f2, f4 (every other = skip pattern) */
    y3_encode(&s->a, f0, f2, f4);
    /* pent#2: f1, f3, f5 (interleaved) */
    y3_encode(&s->b, f1, f3, f5);

    /* Pentagon center = confidence-weighted average */
    float na = s->a.shift, nb = s->b.shift;
    float total = na + nb + 1e-10f;
    float wa = nb / total;  /* weight A by B's shift (cross-weight) */
    float wb = na / total;  /* weight B by A's shift */

    s->pent_center[0] = s->a.centroid[0]*wa + s->b.centroid[0]*wb;
    s->pent_center[1] = s->a.centroid[1]*wa + s->b.centroid[1]*wb;
    s->pent_center[2] = s->a.centroid[2]*wa + s->b.centroid[2]*wb;
    s->pent_shift = _v3_norm(s->pent_center);

    /* Confidence = similarity between pent#1 and pent#2 */
    float diff[3];
    _v3_sub(diff, s->a.centroid, s->b.centroid);
    float d_norm = _v3_norm(diff);
    float s_sum = s->a.shift + s->b.shift + 1e-10f;
    s->conf = d_norm < P5H_CONF_THRESH ? 1.0f :
              1.0f - fminf(1.0f, (d_norm - P5H_CONF_THRESH) / (s_sum + 1e-10f));
    if (s->conf < 0.f) s->conf = 0.f;

    s->face = _vec_to_face(s->pent_center);
    s->compound = _shift_to_compound(s->pent_shift);
}

/* ── Recon: recover 6 features ── */
/* Reverse interleave: A→f0,f2,f4  B→f1,f3,f5 */
static inline void p5h_recon(const P5HState *s, float out[6]) {
    float ra[3], rb[3];
    y3_recon(&s->a, ra);
    y3_recon(&s->b, rb);
    out[0]=ra[0]; out[2]=ra[1]; out[4]=ra[2];
    out[1]=rb[0]; out[3]=rb[1]; out[5]=rb[2];
}

#endif /* CTD_PENT5HEX_H */
