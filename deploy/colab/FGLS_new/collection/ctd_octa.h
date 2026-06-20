/*
 * ctd_octa.h — Y3/Y6 Centroid Engine (Octahedron base)
 *
 * Y3 = 3 direction vectors from name hash → encode 3 features
 * Y6 = 2×Y3 rotated 60° → encode 6 features
 *
 * Octahedron domain: 8 faces (0-7), 3 compounds (0-2)
 * anchor = compound*8 + face,  cell = anchor + track*24
 *
 * Refs: ctd_pent5hex.h, ctd_goldberg.h
 */
#ifndef CTD_OCTA_H
#define CTD_OCTA_H

#include <stdint.h>
#include <string.h>
#include <math.h>

/* ── 3-vector helpers ── */
static inline float _v3_norm(float v[3])
{
    return sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

static inline void _v3_sub(float out[3], float a[3], float b[3])
{
    out[0]=a[0]-b[0]; out[1]=a[1]-b[1]; out[2]=a[2]-b[2];
}

/* 3×3 matrix invert via cofactors */
static inline void _m33_invert(float inv[3][3], float m[3][3])
{
    float d = m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])
            - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])
            + m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
    float id = 1.0f / (d + 1e-10f);
    inv[0][0] = (m[1][1]*m[2][2]-m[1][2]*m[2][1])*id;
    inv[0][1] = (m[0][2]*m[2][1]-m[0][1]*m[2][2])*id;
    inv[0][2] = (m[0][1]*m[1][2]-m[0][2]*m[1][1])*id;
    inv[1][0] = (m[1][2]*m[2][0]-m[1][0]*m[2][2])*id;
    inv[1][1] = (m[0][0]*m[2][2]-m[0][2]*m[2][0])*id;
    inv[1][2] = (m[1][0]*m[0][2]-m[0][0]*m[1][2])*id;
    inv[2][0] = (m[1][0]*m[2][1]-m[1][1]*m[2][0])*id;
    inv[2][1] = (m[0][1]*m[2][0]-m[0][0]*m[2][1])*id;
    inv[2][2] = (m[0][0]*m[1][1]-m[0][1]*m[1][0])*id;
}

/* Map 3D vector → octahedron face 0-7 */
static inline uint8_t _vec_to_face(float v[3])
{
    /* Octahedron: 6 vertices (±1,0,0)(0,±1,0)(0,0,±1) → 8 faces = sign octants */
    uint8_t f = 0;
    if (v[0] >= 0) f |= 1;
    if (v[1] >= 0) f |= 2;
    if (v[2] >= 0) f |= 4;
    return f;
}

/* Map shift magnitude → compound tier 0-2 */
static inline uint8_t _shift_to_compound(float s)
{
    /* compound 0 = near origin (stable), 1 = mid, 2 = far (high variance) */
    if (s < 0.5f) return 0;
    if (s < 2.0f) return 1;
    return 2;
}

/* ── FNV-1a name → seed ── */
static inline uint32_t _name_hash(const char *name)
{
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h;
}

/* ── Y3 State ── */
typedef struct {
    float   dirs[3][3];       /* 3 direction vectors (orthonormal-ish) */
    float   dirs_inv[3][3];   /* inverse matrix */
    float   origin[3];        /* snapshot origin */
    float   centroid[3];      /* encoded centroid */
    float   shift;            /* |centroid - origin| */
    uint8_t face;             /* 0-7 octa face */
    uint8_t phase;            /* 0=A, 1=B */
    uint8_t compound;         /* 0-2 */
    uint8_t pad;
} Y3State;

/* Init Y3: hash name → 3 direction vectors */
static inline void y3_init(Y3State *s, const char *name)
{
    uint32_t h = _name_hash(name);
    /* seed 3 directions */
    float seed[3];
    for (int i = 0; i < 3; i++) {
        h ^= 0x9e3779b9 + (i<<6) + (i>>2);
        h = h * 16777619u ^ (h>>16);
        seed[i] = (float)(h & 0xffff) / 32768.0f - 1.0f;
    }
    /* Gram-Schmidt orthonormalize */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < i; j++) {
            float dot = 0.0f;
            for (int k = 0; k < 3; k++)
                dot += seed[k] * s->dirs[j][k];
            for (int k = 0; k < 3; k++)
                seed[k] -= dot * s->dirs[j][k];
        }
        float n = sqrtf(seed[0]*seed[0]+seed[1]*seed[1]+seed[2]*seed[2]);
        if (n < 1e-10f) { seed[0]=1.0f; seed[1]=0.0f; seed[2]=0.0f; n=1.0f; }
        for (int k = 0; k < 3; k++)
            s->dirs[i][k] = seed[k] / n;
    }
    _m33_invert(s->dirs_inv, s->dirs);
    s->origin[0] = s->origin[1] = s->origin[2] = 0.0f;
    s->centroid[0] = s->centroid[1] = s->centroid[2] = 0.0f;
    s->shift = 0.0f;
    s->face = 0;
    s->phase = 0;
    s->compound = 0;
}

static inline void y3_snapshot(Y3State *s)
{
    s->origin[0] = s->centroid[0];
    s->origin[1] = s->centroid[1];
    s->origin[2] = s->centroid[2];
    s->centroid[0] = s->centroid[1] = s->centroid[2] = 0.0f;
    s->shift = 0.0f;
    s->face = 0;
    s->compound = 0;
}

static inline void y3_encode(Y3State *s, float f0, float f1, float f2)
{
    float v[3] = {f0, f1, f2};
    float c[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            c[i] += s->dirs[j][i] * v[j];
    s->centroid[0] = c[0];
    s->centroid[1] = c[1];
    s->centroid[2] = c[2];
    float rel[3] = {c[0]-s->origin[0], c[1]-s->origin[1], c[2]-s->origin[2]};
    s->shift = _v3_norm(rel);
    s->face = _vec_to_face(rel);
    s->compound = _shift_to_compound(s->shift);
}

static inline void y3_recon(const Y3State *s, float out[3])
{
    for (int i = 0; i < 3; i++) {
        out[i] = 0.0f;
        for (int j = 0; j < 3; j++)
            out[i] += s->dirs_inv[i][j] * (s->centroid[j] - s->origin[j]);
    }
}

/* ── Y6 State (hexagram = 2×Y3 averaged) ── */
typedef struct {
    Y3State  a;           /* Y3 upright */
    Y3State  b;           /* Y3 rotated 60° */
    float    hex_center[3];
    float    hex_shift;
    uint8_t  face;
    uint8_t  compound;
    uint8_t  pad[2];
} Y6State;

static inline void y6_init(Y6State *s, const char *name)
{
    char buf[128];
    int nl = (int)strlen(name);
    if (nl > 120) nl = 120;

    memcpy(buf, name, nl);
    memcpy(buf+nl, "_Y6A", 5);
    y3_init(&s->a, buf);
    s->a.phase = 0;

    memcpy(buf, name, nl);
    memcpy(buf+nl, "_Y6B", 5);
    y3_init(&s->b, buf);
    s->b.phase = 1;

    /* Rotate Y6B 60° around Z for hexagram */
    float c = 0.5f, si = 0.8660254f;
    for (int i = 0; i < 3; i++) {
        float x = s->b.dirs[i][0], y = s->b.dirs[i][1];
        s->b.dirs[i][0] = x*c - y*si;
        s->b.dirs[i][1] = x*si + y*c;
    }
    _m33_invert(s->b.dirs_inv, s->b.dirs);

    s->hex_center[0] = s->hex_center[1] = s->hex_center[2] = 0.0f;
    s->hex_shift = 0.0f;
    s->face = 0;
    s->compound = 0;
}

static inline void y6_snapshot(Y6State *s)
{
    y3_snapshot(&s->a);
    y3_snapshot(&s->b);
    s->hex_center[0] = s->hex_center[1] = s->hex_center[2] = 0.0f;
    s->hex_shift = 0.0f;
    s->face = 0;
    s->compound = 0;
}

static inline void y6_encode(Y6State *s,
                             float f0, float f1, float f2,
                             float f3, float f4, float f5)
{
    y3_encode(&s->a, f0, f1, f2);
    y3_encode(&s->b, f3, f4, f5);
    s->hex_center[0] = (s->a.centroid[0] + s->b.centroid[0]) * 0.5f;
    s->hex_center[1] = (s->a.centroid[1] + s->b.centroid[1]) * 0.5f;
    s->hex_center[2] = (s->a.centroid[2] + s->b.centroid[2]) * 0.5f;
    float rel[3] = {
        s->hex_center[0] - (s->a.origin[0]+s->b.origin[0])*0.5f,
        s->hex_center[1] - (s->a.origin[1]+s->b.origin[1])*0.5f,
        s->hex_center[2] - (s->a.origin[2]+s->b.origin[2])*0.5f,
    };
    s->hex_shift = _v3_norm(rel);
    s->face = _vec_to_face(rel);
    s->compound = _shift_to_compound(s->hex_shift);
}

static inline void y6_recon(const Y6State *s, float out[6])
{
    y3_recon(&s->a, out);
    y3_recon(&s->b, out+3);
}

#endif /* CTD_OCTA_H */
