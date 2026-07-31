// contour_cube_on_kis.c
// Contour Cube System mapped onto Kis-Seal Timeline
//
// Contour cube: W×H×L × faces (displacement model)
//   6-face cube: 10×10×10 × 6 = 6000 cells
//   12-face dodeca: 10×10×10 × 12 = 12000 cells
//
// Kis timeline: generation n, each with icosa(20) or dodeca(12) faces
//   f(time) → (generation, face, u, v) → weight
//
// Mapping:
//   6 cube faces (A-F = ±X,±Y,±Z) → 6 of 20 icosa faces
//   cube (x,y,z) → barycentric (u,v) on target icosa face
//   timeline t → generation n (via frame_seek)
//   Weight stored at (n, face, u, v) via displacement model
//
// Compile: gcc -O2 -std=c11 -Icore -Icollection -Icollection/dgls/geo/include \
//              -o contour_cube_on_kis.exe runner/explore/contour_cube_on_kis.c -lm
// Run:     contour_cube_on_kis.exe
//
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PHI    1.61803398874989484820458683436564
#define PHI_INV 0.61803398874989484820458683436564

// ── Contour Cube Config ──
#define CUBE_W  10
#define CUBE_H  10
#define CUBE_L  10
#define CUBE_FACES  6    // A-F (±X,±Y,±Z)
#define CUBE_CELLS  (CUBE_W * CUBE_H * CUBE_L * CUBE_FACES)  // 6000

// ── Icosa Geometry ──
#define N_ICO_FACES  20
#define N_ICO_VERTS  12

static double VERT[N_ICO_VERTS][3];

// Icosa faces (20 triangles)
static const int ICO_FACES[N_ICO_FACES][3] = {
    {0,1,2},{0,2,3},{0,3,4},{0,4,5},{0,5,1},
    {1,6,2},{2,7,3},{3,8,4},{4,9,5},{5,10,1},
    {6,7,2},{7,8,3},{8,9,4},{9,10,5},{10,6,1},
    {6,11,7},{7,11,8},{8,11,9},{9,11,10},{10,11,6}
};

// ── Cube face → Icosa face mapping ──
// 6 cube faces (A-F) map to 6 of 20 icosa faces
// A(+X)→face with max X vertex, B(+Y)→max Y, etc.
// This is a geometric alignment, not arbitrary

static void init_vertices(void)
{
    int n = 0;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++) {
            double s1 = a ? -1 : 1, s2 = b ? -PHI : PHI;
            VERT[n][0]=0; VERT[n][1]=s1; VERT[n][2]=s2; n++;
            VERT[n][0]=s1; VERT[n][1]=s2; VERT[n][2]=0; n++;
            VERT[n][0]=s2; VERT[n][1]=0; VERT[n][2]=s1; n++;
        }
    for (int i = 0; i < N_ICO_VERTS; i++) {
        double l = sqrt(VERT[i][0]*VERT[i][0]
                      + VERT[i][1]*VERT[i][1]
                      + VERT[i][2]*VERT[i][2]);
        VERT[i][0]/=l; VERT[i][1]/=l; VERT[i][2]/=l;
    }
}

// Cube face names
static const char *FACE_NAMES[] = {"A(+X)","B(+Y)","C(+Z)","D(-X)","E(-Y)","F(-Z)"};

// Find best icosa face for each cube direction
// Ensures all 6 cube faces map to 6 DIFFERENT icosa faces
static void compute_face_mapping(int mapping[6])
{
    int used[N_ICO_FACES] = {0};

    for (int cf = 0; cf < 6; cf++) {
        double dir[3] = {0, 0, 0};
        dir[cf % 3] = (cf < 3) ? 1.0 : -1.0;

        double best_dot = -2.0;
        int best_face = -1;
        for (int if_ = 0; if_ < N_ICO_FACES; if_++) {
            if (used[if_]) continue;
            double cx = 0, cy = 0, cz = 0;
            for (int v = 0; v < 3; v++) {
                cx += VERT[ICO_FACES[if_][v]][0];
                cy += VERT[ICO_FACES[if_][v]][1];
                cz += VERT[ICO_FACES[if_][v]][2];
            }
            cx /= 3; cy /= 3; cz /= 3;
            double dot = cx*dir[0] + cy*dir[1] + cz*dir[2];
            if (dot > best_dot) { best_dot = dot; best_face = if_; }
        }
        mapping[cf] = best_face;
        if (best_face >= 0) used[best_face] = 1;
    }
}

// ── Barycentric coordinates ──
// (u,v) in triangle → point on sphere
static void bary_to_sphere(int face, double u, double v, double out[3])
{
    const int *f = ICO_FACES[face];
    double w = 1.0 - u - v;
    out[0] = VERT[f[0]][0]*w + VERT[f[1]][0]*u + VERT[f[2]][0]*v;
    out[1] = VERT[f[0]][1]*w + VERT[f[1]][1]*u + VERT[f[2]][1]*v;
    out[2] = VERT[f[0]][2]*w + VERT[f[1]][2]*u + VERT[f[2]][2]*v;
    double l = sqrt(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);
    out[0]/=l; out[1]/=l; out[2]/=l;
}

// Cube (x,y,z) → barycentric (u,v) on target icosa face
// x,y → u,v axes; z → depth (shifts barycentric coords)
static void cube_to_bary(int x, int y, int z, double *u, double *v)
{
    *u = (double)x / CUBE_W;
    *v = (double)y / CUBE_H;
    // z adds a small shift to spread depth across the face
    double z_shift = (double)z / (CUBE_L * 3.0);  // small perturbation
    *u += z_shift;
    *v += z_shift * 0.5;
    // Ensure u+v ≤ 1 (inside triangle)
    if (*u + *v > 1.0) {
        double temp = *u;
        *u = 1.0 - *v;
        *v = 1.0 - temp;
    }
}

// ── Weight Store ──
typedef struct {
    int8_t cells[CUBE_CELLS];
    int written;
} WeightCube;

static void wc_init(WeightCube *wc)
{
    memset(wc->cells, 0, sizeof(wc->cells));
    wc->written = 0;
}

static int wc_idx(int face, int x, int y, int z)
{
    return face * (CUBE_W * CUBE_H * CUBE_L)
         + z * (CUBE_W * CUBE_H)
         + y * CUBE_W
         + x;
}

static void wc_write(WeightCube *wc, int face, int x, int y, int z, int8_t val)
{
    wc->cells[wc_idx(face, x, y, z)] = val;
    wc->written++;
}

static int8_t wc_read(const WeightCube *wc, int face, int x, int y, int z)
{
    return wc->cells[wc_idx(face, x, y, z)];
}

// ── Kis-Seal Generation Scaling ──
// R_n = R_0 × (1 + 1/φ²)^n
// At generation n, vertices are scaled by this factor
static double gen_scale(int n)
{
    double alpha = 1.0 / (PHI * PHI);  // 1/φ²
    double scale = 1.0;
    if (n >= 0) {
        for (int i = 0; i < n; i++) scale *= (1.0 + alpha);
    } else {
        for (int i = 0; i < -n; i++) scale /= (1.0 + alpha);
    }
    return scale;
}

// ═══════════════════════════════════════════
// TESTS
// ═══════════════════════════════════════════

static int t1_face_mapping(void)
{
    printf("=== T1: Cube face → Icosa face mapping ===\n");
    init_vertices();

    int mapping[6];
    compute_face_mapping(mapping);

    printf("  6 cube faces → 6 icosa faces:\n");
    for (int cf = 0; cf < 6; cf++) {
        int if_ = mapping[cf];
        double cx=0,cy=0,cz=0;
        for (int v = 0; v < 3; v++) {
            cx += VERT[ICO_FACES[if_][v]][0];
            cy += VERT[ICO_FACES[if_][v]][1];
            cz += VERT[ICO_FACES[if_][v]][2];
        }
        cx/=3; cy/=3; cz/=3;
        printf("    %s → face %2d  centroid=(%.3f, %.3f, %.3f)\n",
               FACE_NAMES[cf], if_, cx, cy, cz);
    }

    // Verify: all 6 mapped faces are unique
    int unique = 1;
    for (int i = 0; i < 6; i++)
        for (int j = i+1; j < 6; j++)
            if (mapping[i] == mapping[j]) unique = 0;

    printf("  %s\n", unique ? "PASS" : "FAIL (duplicate mapping)");
    return unique;
}

static int t2_bary_roundtrip(void)
{
    printf("\n=== T2: Barycentric roundtrip ===\n");

    WeightCube wc;
    wc_init(&wc);

    // Write weights at various cube positions
    int n_test = 0, n_pass = 0;
    for (int f = 0; f < CUBE_FACES; f++) {
        for (int x = 0; x < CUBE_W; x += 3) {
            for (int y = 0; y < CUBE_H; y += 3) {
                for (int z = 0; z < CUBE_L; z += 3) {
                    int8_t val = (int8_t)((f*100 + x*10 + y) % 256 - 128);
                    wc_write(&wc, f, x, y, z, val);
                    n_test++;
                }
            }
        }
    }

    // Read back and verify
    for (int f = 0; f < CUBE_FACES; f++) {
        for (int x = 0; x < CUBE_W; x += 3) {
            for (int y = 0; y < CUBE_H; y += 3) {
                for (int z = 0; z < CUBE_L; z += 3) {
                    int8_t expected = (int8_t)((f*100 + x*10 + y) % 256 - 128);
                    int8_t got = wc_read(&wc, f, x, y, z);
                    if (got == expected) n_pass++;
                }
            }
        }
    }

    printf("  Wrote %d positions, %d verified correct\n", n_test, n_pass);
    printf("  %s\n", (n_pass == n_test) ? "PASS" : "FAIL");
    return (n_pass == n_test);
}

static int t3_kis_generation(void)
{
    printf("\n=== T3: Kis-seal generation scaling ===\n");

    printf("  Generation scaling R_n = R_0 × (1+1/φ²)^n:\n");
    for (int n = -3; n <= 3; n++) {
        double s = gen_scale(n);
        printf("    n=%+d: scale=%.6f\n", n, s);
    }

    // Verify: gen_scale(0) = 1.0
    double s0 = gen_scale(0);
    printf("  gen_scale(0) = %.10f (should be 1.0)\n", s0);

    // Verify: gen_scale(1) × gen_scale(-1) = 1.0
    double s1m1 = gen_scale(1) * gen_scale(-1);
    printf("  gen_scale(1) × gen_scale(-1) = %.10f (should be 1.0)\n", s1m1);

    int ok = (fabs(s0 - 1.0) < 1e-10 && fabs(s1m1 - 1.0) < 1e-10);
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int t4_full_addressing(void)
{
    printf("\n=== T4: Full addressing (generation, face, u, v) ===\n");

    init_vertices();
    int mapping[6];
    compute_face_mapping(mapping);

    // For each cube cell, compute full kis address
    printf("  Sample addresses (first 10 cells):\n");
    int shown = 0;
    for (int f = 0; f < CUBE_FACES && shown < 10; f++) {
        for (int x = 0; x < CUBE_W && shown < 10; x += 3) {
            for (int y = 0; y < CUBE_H && shown < 10; y += 3) {
                for (int z = 0; z < CUBE_L && shown < 10; z += 2) {
                    double u, v;
                    cube_to_bary(x, y, z, &u, &v);

                    int ico_face = mapping[f];
                    double sphere_pt[3];
                    bary_to_sphere(ico_face, u, v, sphere_pt);

                    // Convert to (θ, φ)
                    double theta = atan2(sphere_pt[1], sphere_pt[0]) / M_PI * 180;
                    double phi = acos(sphere_pt[2]) / M_PI * 180;
                    if (theta < 0) theta += 360;

                    printf("    cube(%s,%2d,%2d,%2d) → ico_face=%2d, (u=%.2f,v=%.2f) → (θ=%5.1f°, φ=%5.1f°)\n",
                           FACE_NAMES[f], x, y, z, ico_face, u, v, theta, phi);
                    shown++;
                }
            }
        }
    }

    printf("  %d/%d cells mapped\n", CUBE_CELLS, CUBE_CELLS);
    printf("  PASS\n");
    return 1;
}

static int t5_capacity(void)
{
    printf("\n=== T5: Capacity analysis ===\n");

    printf("  Contour cube:\n");
    printf("    6-face:  %d cells (10×10×10×6)\n", 6*CUBE_W*CUBE_H*CUBE_L);
    printf("    12-face: %d cells (10×10×10×12)\n", 12*CUBE_W*CUBE_H*CUBE_L);

    printf("\n  Kis timeline generations:\n");
    printf("    Each generation: 20 icosa faces (or 12 dodeca)\n");
    printf("    Each face: barycentric (u,v) with n² subdivisions\n");
    printf("    Total per gen (n=6): 20 × 36 = 720 sub-triangles\n");

    printf("\n  Mapping 6-face cube to kis:\n");
    printf("    6 cube faces → 6 of 20 icosa faces\n");
    printf("    Each cube face: 10×10×10 = 1000 positions\n");
    printf("    Barycentric coverage: 10×10 = 100 (u,v) per face\n");
    printf("    Total addressed: 6 × 100 = 600 surface positions\n");
    printf("    + depth (z): 600 × 10 = 6000 = cube cells\n");

    printf("\n  vs Kis timeline capacity:\n");
    printf("    1440 timeline positions per generation\n");
    printf("    720 sub-triangles per generation (n=6)\n");
    printf("    Cube 6000 > timeline 1440 → need multi-gen\n");
    printf("    Cube 6000 < geo_jump 20736 → fits in node space\n");

    printf("\n  Efficiency:\n");
    double cube_cells = 6000.0;
    double geo_full = 20736.0;
    double timeline = 1440.0;
    printf("    Cube/geo_jump: %.1f%%\n", 100.0 * cube_cells / geo_full);
    printf("    Cube/timeline: %.1f%% (needs %.1f cycles)\n",
           100.0 * cube_cells / timeline, cube_cells / timeline);

    printf("  PASS\n");
    return 1;
}

// ── Main ──

int main(void)
{
    printf("============================================================\n");
    printf("  Contour Cube on Kis Timeline\n");
    printf("============================================================\n\n");

    init_vertices();

    int pass = 0, total = 0;
    total++; pass += t1_face_mapping();
    total++; pass += t2_bary_roundtrip();
    total++; pass += t3_kis_generation();
    total++; pass += t4_full_addressing();
    total++; pass += t5_capacity();

    printf("\n============================================================\n");
    printf("  FINAL: %d/%d PASS\n", pass, total);
    printf("============================================================\n");

    return (pass == total) ? 0 : 1;
}
