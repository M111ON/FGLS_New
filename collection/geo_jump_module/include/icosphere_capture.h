/*
 * icosphere_capture.h — On-demand coordinate-driven icosphere capture
 *
 * Unlike brute-force nearest-vertex (162 distance²), this computes
 * the vertex directly from input coordinates via:
 *   1. Stereographic projection: 2D (vx, vy) → 3D sphere point P
 *   2. Face finding: 20 dot products with icosa face normals → containing face
 *   3. Barycentric: project P to face plane, compute (α, β, γ)
 *   4. Round to f=4: i=round(α*4), j=round(β*4), k=round(γ*4), i+j+k=4
 *   5. Vertex position = (i·A + j·B + k·C) / 4  (no table lookup)
 *   6. Key = encode(face, i, j, k)
 *
 * Grid is a MATHEMATICAL FUNCTION — no pre-computed positions stored.
 * Only vertices that data lands on are materialized.
 *
 * Design: no float trig, no malloc, no table larger than 20×3 ints + 20×3 doubles.
 * All operations are O(1) with small constant: ~20 multiply-add + barycentric.
 */

#ifndef ICOSPHERE_CAPTURE_H
#define ICOSPHERE_CAPTURE_H

#include <stdint.h>
#include <math.h>

/*
 * ── Constants ──────────────────────────────────────────────
 */

/* Icosphere subdivision factor (f=4 → 162 vertices) */
#define ICOSA_F4   4
#define ICOSA_F4P1 5

/* Max vertices per face at f=4 = (f+1)(f+2)/2 = 15 */
#define ICOSA_VERTS_PER_FACE 15

/* Sphere radius of icosa vertices (dodeca face centers) */
#define ICOSA_R 1.3763819205

/* ── Icosa faces: 20 faces × 3 vertex indices ────────────── */
/* Each vertex index selects from the 12 dodeca face centers. */
static const int ICOSA_FACES[20][3] = {
    {0, 4, 8},  {0, 8, 2},  {0, 2, 6},  {0, 6, 10}, {0, 10, 4},
    {1, 9, 5},  {1, 5, 11}, {1, 11, 3}, {1, 3, 7},  {1, 7, 9},
    {4, 9, 7},  {4, 7, 8},  {8, 7, 3},  {8, 3, 2},  {2, 3, 11},
    {2, 11, 6}, {6, 11, 5}, {6, 5, 10}, {10, 5, 9}, {10, 9, 4},
};

/* ── Icosa vertex positions (12 dodeca face centers) ─────── */
/* These are the centers of the 12 dodeca faces at circumradius R. */
static const double ICOSA_VERTS[12][3] = {
    { 0.7236067977,  1.1708203932,  0.0000000000},   /*  0 */
    {-0.7236067977, -1.1708203932,  0.0000000000},   /*  1 */
    { 0.7236067977, -1.1708203932,  0.0000000000},   /*  2 */
    {-0.7236067977,  1.1708203932,  0.0000000000},   /*  3 */
    { 0.0000000000,  0.7236067977,  1.1708203932},   /*  4 */
    { 0.0000000000, -0.7236067977, -1.1708203932},   /*  5 */
    { 0.0000000000,  0.7236067977, -1.1708203932},   /*  6 */
    { 0.0000000000, -0.7236067977,  1.1708203932},   /*  7 */
    { 1.1708203932,  0.0000000000,  0.7236067977},   /*  8 */
    {-1.1708203932,  0.0000000000, -0.7236067977},   /*  9 */
    {-1.1708203932,  0.0000000000,  0.7236067977},   /* 10 */
    { 1.1708203932,  0.0000000000, -0.7236067977},   /* 11 */
};

/* ── Icosa face normals (outward, unit) ──────────────────── */
/* Pre-computed from cross(B-A, C-A) for each face. */
static const double ICOSA_FACE_NORMALS[20][3] = {
    { 0.3568220898,  0.5773502692,  0.7347236112},  /*  0: 0-4-8  */
    { 0.9341723590,  0.0000000000,  0.3568220898},  /*  1: 0-8-2  */
    { 0.3568220898,  0.5773502692, -0.7347236112},  /*  2: 0-2-6  */
    {-0.3568220898,  0.5773502692,  0.7347236112},  /*  3: 0-6-10 */
    {-0.9341723590,  0.0000000000,  0.3568220898},  /*  4: 0-10-4 */
    {-0.9341723590,  0.0000000000, -0.3568220898},  /*  5: 1-9-5  */
    {-0.3568220898, -0.5773502692, -0.7347236112},  /*  6: 1-5-11 */
    { 0.3568220898, -0.5773502692, -0.7347236112},  /*  7: 1-11-3 */
    { 0.9341723590,  0.0000000000, -0.3568220898},  /*  8: 1-3-7  */
    { 0.3568220898, -0.5773502692,  0.7347236112},  /*  9: 1-7-9  */
    {-0.5773502692, -0.3568220898,  0.7347236112},  /* 10: 4-9-7  */
    { 0.5773502692, -0.3568220898,  0.7347236112},  /* 11: 4-7-8  */
    { 0.7347236112, -0.5773502692, -0.3568220898},  /* 12: 8-7-3  */
    { 0.7347236112,  0.5773502692, -0.3568220898},  /* 13: 8-3-2  */
    {-0.3568220898, -0.5773502692,  0.7347236112},  /* 14: 2-3-11 */
    {-0.7347236112,  0.5773502692,  0.3568220898},  /* 15: 2-11-6 */
    {-0.7347236112, -0.5773502692,  0.3568220898},  /* 16: 6-11-5 */
    {-0.5773502692,  0.3568220898, -0.7347236112},  /* 17: 6-5-10 */
    { 0.5773502692,  0.3568220898, -0.7347236112},  /* 18: 10-5-9 */
    { 0.3568220898,  0.5773502692, -0.7347236112},  /* 19: 10-9-4 */
};

/* ══════════════════════════════════════════════════════════════
   ON-DEMAND CAPTURE
   ══════════════════════════════════════════════════════════════ */

/*
 * Map 2D signal (sig_x, sig_y) scaled by TW_SCALE to f=4 grid key.
 *
 * Input: vx, vy ∈ [-TW_SCALE, TW_SCALE]  (from tensor capture)
 * Output: capture key = encode(face, i, j, k)
 *   face: 0..19
 *   i, j, k: 0..4, i+j+k=4  (barycentric coordinates × f4)
 *
 * The vertex position can be computed from:
 *   A = ICOSA_VERTS[ICOSA_FACES[face][0]]
 *   B = ICOSA_VERTS[ICOSA_FACES[face][1]]
 *   C = ICOSA_VERTS[ICOSA_FACES[face][2]]
 *   V = (i·A + j·B + k·C) / 4
 *
 * Key encoding:
 *   key = face * 15 + i + j*5 - j*(j-1)/2
 *   where 15 = ICOSA_VERTS_PER_FACE = (f+1)(f+2)/2
 */
static inline uint32_t icosa_capture_on_demand(int64_t vx, int64_t vy) {
    /* Step 1: Stereographic projection 2D → sphere */
    double sx = (double)vx * (1.0 / 207360.0);
    double sy = (double)vy * (1.0 / 207360.0);
    double r2 = sx * sx + sy * sy;
    double denom = 1.0 / (1.0 + r2);
    double px = (2.0 * sx * denom) * ICOSA_R;
    double py = (2.0 * sy * denom) * ICOSA_R;
    double pz = ((1.0 - r2) * denom) * ICOSA_R;

    /* Step 2: Find containing face (max dot with normals) */
    int face = 0;
    double best_dot = -1e30;
    for (int f = 0; f < 20; f++) {
        double dot = px * ICOSA_FACE_NORMALS[f][0]
                   + py * ICOSA_FACE_NORMALS[f][1]
                   + pz * ICOSA_FACE_NORMALS[f][2];
        if (dot > best_dot) { best_dot = dot; face = f; }
    }

    /* Step 3: Get face corners */
    int iA = ICOSA_FACES[face][0];
    int iB = ICOSA_FACES[face][1];
    int iC = ICOSA_FACES[face][2];

    /* Step 4: Project P to face plane, compute barycentric (α, β, γ) */
    double Ax = ICOSA_VERTS[iA][0], Ay = ICOSA_VERTS[iA][1], Az = ICOSA_VERTS[iA][2];
    double Bx = ICOSA_VERTS[iB][0], By = ICOSA_VERTS[iB][1], Bz = ICOSA_VERTS[iB][2];
    double Cx = ICOSA_VERTS[iC][0], Cy = ICOSA_VERTS[iC][1], Cz = ICOSA_VERTS[iC][2];

    /* Edge vectors: v1 = B-A, v2 = C-A */
    double v1x = Bx - Ax, v1y = By - Ay, v1z = Bz - Az;
    double v2x = Cx - Ax, v2y = Cy - Ay, v2z = Cz - Az;

    /* P relative to A */
    double pvx = px - Ax, pvy = py - Ay, pvz = pz - Az;

    /* Dot products */
    double d00 = v1x*v1x + v1y*v1y + v1z*v1z;
    double d01 = v1x*v2x + v1y*v2y + v1z*v2z;
    double d11 = v2x*v2x + v2y*v2y + v2z*v2z;
    double d20 = pvx*v1x + pvy*v1y + pvz*v1z;
    double d21 = pvx*v2x + pvy*v2y + pvz*v2z;

    double det = d00 * d11 - d01 * d01;
    if (det < 1e-30) return 0; /* degenerate, safe fallback */

    double inv_det = 1.0 / det;
    double beta  = (d11 * d20 - d01 * d21) * inv_det;  /* weight for B */
    double gamma = (d00 * d21 - d01 * d20) * inv_det;  /* weight for C */
    double alpha = 1.0 - beta - gamma;                  /* weight for A */

    /* Step 5: Round to f=4 grid */
    double fi = alpha * ICOSA_F4;
    double fj = beta  * ICOSA_F4;
    double fk = gamma * ICOSA_F4;

    int i = (int)(fi + 0.5);
    int j = (int)(fj + 0.5);
    int k = (int)(fk + 0.5);

    /* Clamp to [0, f4] */
    if (i < 0) i = 0; if (i > ICOSA_F4) i = ICOSA_F4;
    if (j < 0) j = 0; if (j > ICOSA_F4) j = ICOSA_F4;
    if (k < 0) k = 0; if (k > ICOSA_F4) k = ICOSA_F4;

    /* Normalize to i+j+k = f4 */
    int sum = i + j + k;
    if (sum > ICOSA_F4) {
        /* Need to subtract — find the dimension with largest rounding-up error */
        /* Error = original non-integer value - rounded value (negative = rounded up) */
        double ei = fi - i;  /* ≤ 0 if rounded up */
        double ej = fj - j;
        double ek = fk - k;
        while (sum > ICOSA_F4) {
            if (ei <= ej && ei <= ek) { i--; ei = -999; }
            else if (ej <= ek) { j--; ej = -999; }
            else { k--; ek = -999; }
            sum--;
        }
    } else if (sum < ICOSA_F4) {
        /* Need to add — find dimension with largest rounding-down error */
        double ei = fi - i;  /* ≥ 0 if rounded down */
        double ej = fj - j;
        double ek = fk - k;
        while (sum < ICOSA_F4) {
            if (ei >= ej && ei >= ek) { i++; ei = -999; }
            else if (ej >= ek) { j++; ej = -999; }
            else { k++; ek = -999; }
            sum++;
        }
    }

    /* Step 6: Encode key = face * 15 + index within face */
    /* Linear index: idx = i + j*5 - j*(j-1)/2  for f=4 */
    int idx = i + j * ICOSA_F4P1 - j * (j - 1) / 2;
    if (idx < 0) idx = 0;
    if (idx >= ICOSA_VERTS_PER_FACE) idx = ICOSA_VERTS_PER_FACE - 1;

    return (uint32_t)(face * ICOSA_VERTS_PER_FACE + idx);
}

/*
 * ── Decode capture key to vertex position ──
 * Reconstructs 3D position from (face, i, j, k) encoding.
 * No table lookup — computes blend of corner positions.
 */
static inline void icosa_decode_position(uint32_t key,
                                          double *ox, double *oy, double *oz) {
    int face = (int)(key / ICOSA_VERTS_PER_FACE);
    int idx  = (int)(key % ICOSA_VERTS_PER_FACE);

    /* Decode idx → (i, j) for f=4 */
    int j = 0, cum = 0;
    for (; j <= ICOSA_F4; j++) {
        int row = ICOSA_F4 - j + 1; /* vertices in this row */
        if (idx < cum + row) break;
        cum += row;
    }
    int i = idx - cum;
    int k = ICOSA_F4 - i - j;

    /* Get face corners */
    int iA = ICOSA_FACES[face][0];
    int iB = ICOSA_FACES[face][1];
    int iC = ICOSA_FACES[face][2];

    double inv_f4 = 1.0 / ICOSA_F4;
    *ox = (i * ICOSA_VERTS[iA][0] + j * ICOSA_VERTS[iB][0] + k * ICOSA_VERTS[iC][0]) * inv_f4;
    *oy = (i * ICOSA_VERTS[iA][1] + j * ICOSA_VERTS[iB][1] + k * ICOSA_VERTS[iC][1]) * inv_f4;
    *oz = (i * ICOSA_VERTS[iA][2] + j * ICOSA_VERTS[iB][2] + k * ICOSA_VERTS[iC][2]) * inv_f4;
}

/*
 * ── Get (i, j, k) from key ──
 */
static inline void icosa_decode_ijk(uint32_t key, int *oi, int *oj, int *ok) {
    int face = (int)(key / ICOSA_VERTS_PER_FACE);
    int idx  = (int)(key % ICOSA_VERTS_PER_FACE);
    (void)face;

    int j = 0, cum = 0;
    for (; j <= ICOSA_F4; j++) {
        int row = ICOSA_F4 - j + 1;
        if (idx < cum + row) break;
        cum += row;
    }
    *oi = idx - cum;
    *oj = j;
    *ok = ICOSA_F4 - *oi - *oj;
}

/*
 * ── Get face index from key ──
 */
static inline int icosa_key_to_face(uint32_t key) {
    return (int)(key / ICOSA_VERTS_PER_FACE);
}

#endif /* ICOSPHERE_CAPTURE_H */
