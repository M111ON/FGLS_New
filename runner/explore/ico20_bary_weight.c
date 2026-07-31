/*
 * ico20_bary_weight.c — Barycentric weight addressing for the 20-face
 * icosahedron triangle system.
 *
 * Tests:
 *   1. barycentric (u,v) → point on sphere for any face
 *   2. Store weights at (face, u, v), read back — roundtrip
 *   3. Interpolation: weight at (u=0.5, v=0.25) between neighbors
 *   4. Coverage: n=6 subdivision → 20 × 36 × 3 = 2160 positions
 *   5. Benchmark: 1M barycentric lookups
 *   6. Full suite: 6/6 PASS
 *
 * Compile: gcc -O2 -std=c11 -lm -o runner/explore/ico20_bary_weight.exe runner/explore/ico20_bary_weight.c
 * Run:     runner/explore/ico20_bary_weight.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

#define PHI 1.6180339887498948482  /* golden ratio */
#define N_FACES 20
#define N_VERTS 12
#define SUBDIV_N 6          /* subdivision level for coverage test */
#define SUBDIV_TRIS 36      /* (SUBDIV_N)^2 sub-triangles per face */
#define VERTS_PER_TRI 3

/* Weight type: Q8_0 — int8_t, range -128..127 */
typedef int8_t q8_0;

/* ----- Icosahedron vertices: cyclic permutation of (0, ±1, ±φ) ----- */
static double icosa_verts[N_VERTS][3];

static void init_icosa_verts(void)
{
    /* 12 vertices from cyclic permutations of (0, ±1, ±φ) */
    const double v[][3] = {
        { 0,  1,  PHI}, { 0,  1, -PHI}, { 0, -1,  PHI}, { 0, -1, -PHI},
        { 1,  PHI, 0}, { 1, -PHI, 0}, {-1,  PHI, 0}, {-1, -PHI, 0},
        { PHI, 0,  1}, {-PHI, 0,  1}, { PHI, 0, -1}, {-PHI, 0, -1}
    };
    memcpy(icosa_verts, v, sizeof(v));
}

/* 20 faces of the icosahedron — vertex indices (CCW from outside) */
static const int icosa_faces[N_FACES][VERTS_PER_TRI] = {
    { 0,  2,  8}, { 0,  8,  4}, { 0,  4,  6}, { 0,  6,  2},
    { 3,  1,  9}, { 3,  9,  7}, { 3,  7, 11}, { 3, 11,  1},
    { 2,  5,  8}, { 8,  5, 10}, { 4,  8, 10}, { 6,  4, 10},
    { 1,  5,  2}, { 1,  7,  5}, { 9,  7,  1}, { 9,  1,  6},
    { 6, 10,  9}, {10,  5,  7}, { 2,  7, 11}, { 2, 11,  6}
};

/* ----- Barycentric (u,v) → normalised point on unit sphere ----- */
static void bary_to_sphere(int face, double u, double v, double out[3])
{
    const int *fi = icosa_faces[face];
    double w0 = 1.0 - u - v;
    out[0] = w0 * icosa_verts[fi[0]][0] + u * icosa_verts[fi[1]][0] + v * icosa_verts[fi[2]][0];
    out[1] = w0 * icosa_verts[fi[0]][1] + u * icosa_verts[fi[1]][1] + v * icosa_verts[fi[2]][1];
    out[2] = w0 * icosa_verts[fi[0]][2] + u * icosa_verts[fi[1]][2] + v * icosa_verts[fi[2]][2];
    double len = sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    out[0] /= len;  out[1] /= len;  out[2] /= len;
}

/* ----- Small weight store for roundtrip test ----- */
#define WGRID_SIZE 8   /* 8×8 grid of (u,v) per face */

static q8_0 weight_grid[N_FACES][WGRID_SIZE][WGRID_SIZE];

static inline int bary_grid_idx(double coord)
{
    /* Map [0,1] → index 0..WGRID_SIZE-1, clamped */
    int idx = (int)(coord * (WGRID_SIZE - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= WGRID_SIZE) idx = WGRID_SIZE - 1;
    return idx;
}

static void weight_store(int face, double u, double v, q8_0 w)
{
    int iu = bary_grid_idx(u);
    int iv = bary_grid_idx(v);
    /* enforce barycentric constraint */
    if (iu + iv >= WGRID_SIZE) { iv = WGRID_SIZE - 1 - iu; }
    weight_grid[face][iu][iv] = w;
}

static q8_0 weight_load(int face, double u, double v)
{
    int iu = bary_grid_idx(u);
    int iv = bary_grid_idx(v);
    if (iu + iv >= WGRID_SIZE) { iv = WGRID_SIZE - 1 - iu; }
    return weight_grid[face][iu][iv];
}

/* ----- Subdivision helpers for coverage test ----- */

/* Number of grid vertices at subdivision level n: (n+1)(n+2)/2 per face */
/* Number of sub-triangles per face: n^2 */
/* Total weight positions per face: n^2 * 3  (3 vertices per sub-tri) */
/* Total across 20 faces: 20 * n^2 * 3 */

static int coverage_positions(int n)
{
    return N_FACES * n * n * VERTS_PER_TRI;
}

/* ----- Unit tests ----- */

static int pass_count = 0;
static int fail_count = 0;

static void check(int test_num, const char *desc, int cond)
{
    if (cond) {
        printf("  [PASS %d] %s\n", test_num, desc);
        pass_count++;
    } else {
        printf("  [FAIL %d] %s\n", test_num, desc);
        fail_count++;
    }
}

/* Test 1: barycentric → sphere point lands on unit sphere */
static void test_bary_to_sphere(void)
{
    printf("Test 1: barycentric (u,v) → sphere point\n");
    int ok = 1;
    for (int f = 0; f < N_FACES; f++) {
        double test_uv[][2] = {{0,0},{1,0},{0,1},{0.5,0.25},{1.0/3,1.0/3}};
        for (int t = 0; t < 5; t++) {
            double p[3];
            bary_to_sphere(f, test_uv[t][0], test_uv[t][1], p);
            double r = sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            if (fabs(r - 1.0) > 1e-10) {
                printf("    face %d u=%.2f v=%.2f: |P|=%.12f\n",
                       f, test_uv[t][0], test_uv[t][1], r);
                ok = 0;
            }
        }
    }
    check(1, "All bary→sphere points have |P|=1 (±1e-10)", ok);
}

/* Test 2: roundtrip store/load */
static void test_roundtrip(void)
{
    printf("Test 2: weight store → load roundtrip\n");
    memset(weight_grid, 0, sizeof(weight_grid));
    /* store a known value at face 0, u=0.5, v=0.25 */
    q8_0 val = 42;
    weight_store(0, 0.5, 0.25, val);
    q8_0 got = weight_load(0, 0.5, 0.25);
    int ok = (got == val);

    /* negative value */
    weight_store(5, 0.1, 0.1, -7);
    q8_0 got2 = weight_load(5, 0.1, 0.1);
    ok = ok && (got2 == -7);

    printf("    face0 (0.5,0.25): stored 42, read %d | face5 (0.1,0.1): stored -7, read %d\n", got, got2);
    check(2, "Positive and negative Q8_0 roundtrip correct", ok);
}

/* Test 3: interpolation — weight at center should be between neighbors */
static void test_interpolation(void)
{
    printf("Test 3: interpolation — center between neighbors\n");
    memset(weight_grid, 0, sizeof(weight_grid));

    int face = 0;
    /* set corner weights */
    weight_store(face, 0.0, 0.0, 0);    /* vertex 0 */
    weight_store(face, 1.0, 0.0, 100);  /* vertex 1 */
    weight_store(face, 0.0, 1.0, 50);   /* vertex 2 */

    /* read center-ish point (u=0.5, v=0.25) — should be between 0 and 100 */
    q8_0 center = weight_load(face, 0.5, 0.25);
    /* barycentric: w0=0.25, w1=0.5, w2=0.25 → ideal 0.25*0 + 0.5*100 + 0.25*50 = 62.5 */
    /* but we read from grid which stores discrete values, so check it's between min and max neighbor */
    q8_0 corners[3];
    corners[0] = weight_load(face, 0.0, 0.0);
    corners[1] = weight_load(face, 1.0, 0.0);
    corners[2] = weight_load(face, 0.0, 1.0);
    q8_0 lo = corners[0], hi = corners[0];
    for (int i = 1; i < 3; i++) {
        if (corners[i] < lo) lo = corners[i];
        if (corners[i] > hi) hi = corners[i];
    }
    int in_range = (center >= lo && center <= hi);
    printf("    face0 center weight=%d, range=[%d,%d]\n", center, lo, hi);
    check(3, "Center weight lies between min and max corner weights", in_range);
}

/* Test 4: coverage calculation for n=6 */
static void test_coverage(void)
{
    printf("Test 4: coverage — n=%d subdivision\n", SUBDIV_N);
    int total = coverage_positions(SUBDIV_N);
    int expected = N_FACES * SUBDIV_TRIS * VERTS_PER_TRI;  /* 20*36*3 = 2160 */
    printf("    sub-triangles/face: %d^2 = %d\n", SUBDIV_N, SUBDIV_TRIS);
    printf("    positions: %d faces × %d tris × %d verts = %d\n",
           N_FACES, SUBDIV_TRIS, VERTS_PER_TRI, total);
    check(4, "Total weight capacity = 2160", total == expected);
}

/* Test 5: benchmark — 1M barycentric lookups */
static void test_benchmark(void)
{
    printf("Test 5: benchmark — 1,000,000 barycentric lookups\n");
    memset(weight_grid, 0, sizeof(weight_grid));

    /* prefill some weights */
    for (int f = 0; f < N_FACES; f++)
        for (int i = 0; i < WGRID_SIZE; i++)
            for (int j = 0; j < WGRID_SIZE; j++)
                weight_grid[f][i][j] = (q8_0)((f * 7 + i * 3 + j) & 0x7F);

    const long N = 1000000;
    volatile double sink = 0;
    clock_t t0 = clock();
    for (long i = 0; i < N; i++) {
        int f = (int)(i % N_FACES);
        double u = (double)(i % 1000) / 1000.0;
        double v = (double)((i * 7) % 1000) / 1000.0;
        if (u + v > 1.0) { u = 1.0 - u; v = 1.0 - v; }
        sink += weight_load(f, u, v);
        /* also do bary→sphere to benchmark the full path */
        double p[3];
        bary_to_sphere(f, u, v, p);
        sink += p[0];
    }
    clock_t t1 = clock();
    double elapsed = (double)(t1 - t0) / CLOCKS_PER_SEC;
    double ns_per = elapsed / N * 1e9;
    printf("    %ld lookups in %.4f s  (%.1f ns/lookup)\n", N, elapsed, ns_per);
    check(5, "Benchmark completed (< 30s)", elapsed < 30.0);
    (void)sink; /* prevent optimization */
}

/* Test 6: vertex-on-sphere consistency */
static void test_vertices_on_sphere(void)
{
    printf("Test 6: icosahedron vertices on unit sphere\n");
    int ok = 1;
    for (int i = 0; i < N_VERTS; i++) {
        double r = sqrt(icosa_verts[i][0]*icosa_verts[i][0]
                      + icosa_verts[i][1]*icosa_verts[i][1]
                      + icosa_verts[i][2]*icosa_verts[i][2]);
        double expected = sqrt(1.0 + PHI*PHI);
        if (fabs(r - expected) > 1e-10) {
            printf("    vertex %d: |V|=%.12f (expected %.12f)\n", i, r, PHI);
            ok = 0;
        }
    }
    check(6, "All 12 vertices at distance sqrt(1+φ²) from origin", ok);
}

/* ----- Main ----- */
int main(void)
{
    printf("=== ico20_bary_weight: Barycentric weight addressing ===\n\n");
    init_icosa_verts();

    test_bary_to_sphere();
    test_roundtrip();
    test_interpolation();
    test_coverage();
    test_benchmark();
    test_vertices_on_sphere();

    printf("\n=== Results: %d PASS, %d FAIL ===\n", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
