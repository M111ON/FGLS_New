/*
 * contour_placement_approaches.c
 * =====================================================================
 * Explore 3 alternative approaches for placing 6000 contour cube cells
 * (6 faces × 10×10×10) onto 1440 geo_jump addresses (15 towers × 48 × 2).
 *
 * Current baseline (direct mapping): 4.17 cells/addr, 4560 collisions
 *
 * Approach 1: Fibonacci stride-37 based distribution
 *   Uses golden-ratio stride to scatter cells across addresses,
 *   breaking regularity that causes pile-ups.
 *
 * Approach 2: Barycentric-weighted distribution (u,v on icosahedral faces)
 *   Maps each contour cell (face,x,y,z) to barycentric (u,v) on an
 *   icosahedral triangle, then projects to geo_jump address.
 *
 * Approach 3: Resolution-scaled distribution (h-depth variable resolution)
 *   Uses variable resolution: cells near center of cube face get fewer
 *   geo addresses (lower res), cells near edges get more (higher res).
 *
 * For each: collision count, coverage uniformity, distribution skew.
 *
 * Compile: gcc -O2 -std=c11 -o runner/explore/contour_placement_approaches.exe \
 *              runner/explore/contour_placement_approaches.c -lm
 * Run:     runner/explore/contour_placement_approaches.exe
 * =====================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Constants ── */
#define CUBE_FACES   6
#define CUBE_W      10
#define CUBE_H      10
#define CUBE_L      10
#define CUBE_CELLS  (CUBE_FACES * CUBE_W * CUBE_H * CUBE_L)  /* 6000 */

#define N_TOWERS    15
#define TOWER_ADDR  48
#define N_POLAR     2
#define GEO_ADDRS   (N_TOWERS * TOWER_ADDR * N_POLAR)        /* 1440 */
#define GEO_FIBO_CLOCK 1440

#define PHI 1.6180339887498948482
#define FIBO_STRIDE 37   /* coprime with 1440, good scatter */

/* ── Icosahedron data (20-face) ── */
#define ICO_FACES 20
#define ICO_VERTS 12

static double ico_verts[ICO_VERTS][3];
static const int ico_face_idx[ICO_FACES][3] = {
    { 0,  2,  8}, { 0,  8,  4}, { 0,  4,  6}, { 0,  6,  2},
    { 3,  1,  9}, { 3,  9,  7}, { 3,  7, 11}, { 3, 11,  1},
    { 2,  5,  8}, { 8,  5, 10}, { 4,  8, 10}, { 6,  4, 10},
    { 1,  5,  2}, { 1,  7,  5}, { 9,  7,  1}, { 9,  1,  6},
    { 6, 10,  9}, {10,  5,  7}, { 2,  7, 11}, { 2, 11,  6}
};

static void init_ico_verts(void) {
    const double v[][3] = {
        { 0,  1,  PHI}, { 0,  1, -PHI}, { 0, -1,  PHI}, { 0, -1, -PHI},
        { 1,  PHI, 0}, { 1, -PHI, 0}, {-1,  PHI, 0}, {-1, -PHI, 0},
        { PHI, 0,  1}, {-PHI, 0,  1}, { PHI, 0, -1}, {-PHI, 0, -1}
    };
    memcpy(ico_verts, v, sizeof(v));
}

/* ── Contour cell struct ── */
typedef struct {
    int face, x, y, z;
    int global_idx;
} ContourCell;

static ContourCell all_cells[CUBE_CELLS];

static void init_cells(void) {
    int idx = 0;
    for (int f = 0; f < CUBE_FACES; f++)
        for (int z = 0; z < CUBE_L; z++)
            for (int y = 0; y < CUBE_H; y++)
                for (int x = 0; x < CUBE_W; x++) {
                    all_cells[idx].face = f;
                    all_cells[idx].x = x;
                    all_cells[idx].y = y;
                    all_cells[idx].z = z;
                    all_cells[idx].global_idx = idx;
                    idx++;
                }
}

/* ── Tower pair assignment (same as baseline) ── */
static int PAIR_A[15] = {0,0,0,0,0, 1,1,1,1, 2,2,2, 3,3, 4};
static int PAIR_B[15] = {1,2,3,4,5, 2,3,4,5, 3,4,5, 4,5, 5};
static int FACE_TOWERS[6][5];

static void init_face_towers(void) {
    for (int f = 0; f < 6; f++) {
        int tidx = 0;
        for (int t = 0; t < N_TOWERS; t++) {
            if (PAIR_A[t] == f || PAIR_B[t] == f)
                FACE_TOWERS[f][tidx++] = t;
        }
    }
}

/* ── Utility: compute statistics on addr_count array ── */
typedef struct {
    int min_hit, max_hit, zero_addrs;
    int covered;
    double mean, stddev;
    double skewness;
    int collisions;
    int histogram[32];  /* histogram[i] = # addrs with exactly i hits */
} AddrStats;

static AddrStats analyze_addrs(const int *addr_count) {
    AddrStats s;
    memset(&s, 0, sizeof(s));
    s.min_hit = 999999;
    s.max_hit = 0;
    s.covered = 0;

    /* clear histogram */
    for (int i = 0; i < 32; i++) s.histogram[i] = 0;

    double sum = 0;
    for (int i = 0; i < GEO_ADDRS; i++) {
        int c = addr_count[i];
        sum += c;
        if (c < s.min_hit) s.min_hit = c;
        if (c > s.max_hit) s.max_hit = c;
        if (c == 0) s.zero_addrs++;
        if (c > 0) s.covered++;
        int h = (c < 31) ? c : 31;
        s.histogram[h]++;
    }

    s.mean = sum / GEO_ADDRS;

    /* stddev */
    double var = 0;
    for (int i = 0; i < GEO_ADDRS; i++) {
        double d = addr_count[i] - s.mean;
        var += d * d;
    }
    s.stddev = sqrt(var / GEO_ADDRS);

    /* skewness */
    double m3 = 0;
    for (int i = 0; i < GEO_ADDRS; i++) {
        double d = (addr_count[i] - s.mean) / (s.stddev > 0.001 ? s.stddev : 1.0);
        m3 += d * d * d;
    }
    s.skewness = m3 / GEO_ADDRS;

    /* collisions */
    s.collisions = 0;
    for (int i = 0; i < GEO_ADDRS; i++) {
        if (addr_count[i] > 1) s.collisions += addr_count[i] - 1;
    }

    return s;
}

static void print_stats(const char *label, const AddrStats *s) {
    printf("  %s:\n", label);
    printf("    Collisions:    %d / %d cells  (%.1f%% collision rate)\n",
           s->collisions, CUBE_CELLS, 100.0 * s->collisions / CUBE_CELLS);
    printf("    Avg cells/addr:%.2f   Stddev: %.2f   Skewness: %.3f\n",
           s->mean, s->stddev, s->skewness);
    printf("    Min: %d   Max: %d   Zero-hit: %d   Covered: %d/%d (%.1f%%)\n",
           s->min_hit, s->max_hit, s->zero_addrs,
           s->covered, GEO_ADDRS, 100.0 * s->covered / GEO_ADDRS);

    /* uniformity = stddev / mean (lower = more uniform) */
    double uniformity = (s->mean > 0.001) ? s->stddev / s->mean : 999.0;
    printf("    Uniformity (CV): %.3f  (lower = better)\n", uniformity);

    printf("    Histogram (addr_count → count):\n");
    for (int h = 0; h < 32; h++) {
        if (s->histogram[h] > 0) {
            printf("      %2d hits: %4d addrs\n", h, s->histogram[h]);
        }
    }
}

/* ====================================================================
 * APPROACH 1: Fibonacci stride-37 based distribution
 *
 * For each contour cell (face,x,y,z):
 *   1. Compute a scalar hash: h = face*1000 + z*100 + y*10 + x
 *   2. Apply Fibonacci stride: addr_idx = (h * FIBO_STRIDE) % GEO_ADDRS
 *   3. Map addr_idx → (tower, addr, polar):
 *        tower = addr_idx / (TOWER_ADDR * N_POLAR)
 *        polar = (addr_idx / TOWER_ADDR) % N_POLAR
 *        addr  = addr_idx % TOWER_ADDR
 *
 * The stride-37 ensures cells are scattered with no regular pattern.
 * Since 37 and 1440 are coprime (gcd=1), the stride visits ALL 1440
 * addresses before repeating — maximum dispersion.
 * ==================================================================== */

static int approach1_addr_count[GEO_ADDRS];

static void approach1_fibo_stride(void) {
    memset(approach1_addr_count, 0, sizeof(approach1_addr_count));

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        /* Hash: face,x,y,z → scalar 0..5999 */
        int h = c->face * 1000 + c->z * 100 + c->y * 10 + c->x;
        /* Fibonacci stride scatter */
        int addr_idx = ((unsigned)(h * FIBO_STRIDE)) % GEO_ADDRS;
        approach1_addr_count[addr_idx]++;
    }
}

/* ====================================================================
 * APPROACH 2: Barycentric-weighted distribution (u,v on icosahedral)
 *
 * Map each contour cell (face,x,y,z) to an icosahedral face, then
 * use barycentric (u,v) to produce a spatially coherent address:
 *
 *   1. 6 contour faces → 20 ico faces via modular mapping
 *      ico_face = (contour_face * 3 + z/3) % 20   (spreads across 20)
 *   2. u = x / 9.0, v = y / 9.0  (0..1 range on triangle)
 *   3. Clamp to barycentric: if u+v > 1, remap
 *   4. Project (u,v) → sphere point P = bary_to_sphere(ico_face, u, v)
 *   5. Quantize P to address:
 *        θ = acos(P[2]), φ = atan2(P[1], P[0])
 *        tower = (int)(φ / (2π) * 15) % 15
 *        addr  = (int)(θ / (π/2) * 48) % 48
 *        polar = z < 5 ? 0 : 1
 * ==================================================================== */

static void bary_to_sphere(int face, double u, double v, double out[3]) {
    const int *fi = ico_face_idx[face];
    double w0 = 1.0 - u - v;
    out[0] = w0 * ico_verts[fi[0]][0] + u * ico_verts[fi[1]][0] + v * ico_verts[fi[2]][0];
    out[1] = w0 * ico_verts[fi[0]][1] + u * ico_verts[fi[1]][1] + v * ico_verts[fi[2]][1];
    out[2] = w0 * ico_verts[fi[0]][2] + u * ico_verts[fi[1]][2] + v * ico_verts[fi[2]][2];
    double len = sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    if (len > 1e-12) { out[0] /= len; out[1] /= len; out[2] /= len; }
}

static int approach2_addr_count[GEO_ADDRS];

static void approach2_barycentric(void) {
    memset(approach2_addr_count, 0, sizeof(approach2_addr_count));

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];

        /* Map 6 contour faces → 20 icosahedral faces */
        int ico_face = (c->face * 3 + c->z / 3) % ICO_FACES;

        /* Barycentric coordinates */
        double u = c->x / 9.0;
        double v = c->y / 9.0;
        if (u + v > 1.0) { double t = u; u = 1.0 - v; v = 1.0 - t; }

        /* Project to sphere */
        double P[3];
        bary_to_sphere(ico_face, u, v, P);

        /* Quantize to address */
        double theta = acos(P[2]);                           /* 0..π */
        double phi   = atan2(P[1], P[0]);                    /* -π..π */
        if (phi < 0) phi += 2.0 * M_PI;                     /* 0..2π */

        int tower = (int)(phi / (2.0 * M_PI) * N_TOWERS) % N_TOWERS;
        int addr  = (int)(theta / (M_PI * 0.5) * TOWER_ADDR) % TOWER_ADDR;
        int polar = c->z < 5 ? 0 : 1;

        int geo_idx = tower * TOWER_ADDR * N_POLAR
                    + polar * TOWER_ADDR
                    + addr;
        approach2_addr_count[geo_idx]++;
    }
}

/* ====================================================================
 * APPROACH 3: Resolution-scaled distribution (h-depth variable res)
 *
 * Variable resolution: cells near the center of a cube face use
 * a COARSER address mapping (wider bins), while cells near edges
 * use a FINER mapping (narrower bins). This prevents center pile-up.
 *
 *   1. Compute radial distance from face center:
 *        dx = x - 4.5, dy = y - 4.5, r = sqrt(dx²+dy²) / max_r
 *        max_r ≈ 6.364 (corner of 10×10 square)
 *   2. Scale factor: res = 1 + r * (k-1) where k = resolution multiplier
 *      - At center (r=0): res=1 → coarse mapping
 *      - At edge (r=1): res=k → fine mapping
 *   3. Effective address space per face = TOWER_ADDR * res
 *   4. tower = face assignment (from FACE_TOWERS based on (x+y+z)%5)
 *   5. addr = quantize based on rescaled (x,y)
 *   6. polar = z < 5 ? 0 : 1
 * ==================================================================== */

static int approach3_addr_count[GEO_ADDRS];

static void approach3_resolution_scaled(void) {
    memset(approach3_addr_count, 0, sizeof(approach3_addr_count));

    double max_r = sqrt(4.5*4.5 + 4.5*4.5); /* ≈ 6.364 */
    double k = 3.0; /* resolution multiplier at edges */

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];

        /* Radial distance from face center (0..1) */
        double dx = c->x - 4.5;
        double dy = c->y - 4.5;
        double r = sqrt(dx*dx + dy*dy) / max_r;

        /* Variable resolution scale */
        double res = 1.0 + r * (k - 1.0);  /* 1..3 */

        /* Rescaled coordinates */
        double sx = c->x * res;
        double sy = c->y * res;

        /* Select tower from face (5 towers per face) */
        int tower_local = (c->x + c->y + c->z) % 5;
        int tower = FACE_TOWERS[c->face][tower_local];

        /* Quantize address with resolution scaling */
        int addr = ((int)(sx * 10 + sy)) % TOWER_ADDR;
        if (addr < 0) addr += TOWER_ADDR;
        int polar = c->z < 5 ? 0 : 1;

        int geo_idx = tower * TOWER_ADDR * N_POLAR
                    + polar * TOWER_ADDR
                    + addr;
        approach3_addr_count[geo_idx]++;
    }
}

/* ====================================================================
 * BASELINE: Direct mapping (for comparison)
 * ==================================================================== */

static int baseline_addr_count[GEO_ADDRS];

static void baseline_direct(void) {
    memset(baseline_addr_count, 0, sizeof(baseline_addr_count));

    for (int i = 0; i < CUBE_CELLS; i++) {
        ContourCell *c = &all_cells[i];
        int tower_local = (c->x + c->y + c->z) % 5;
        int tower = FACE_TOWERS[c->face][tower_local];
        int addr  = (c->x * 10 + c->y) % TOWER_ADDR;
        int polar = c->z % N_POLAR;

        int geo_idx = tower * TOWER_ADDR * N_POLAR
                    + polar * TOWER_ADDR
                    + addr;
        baseline_addr_count[geo_idx]++;
    }
}

/* ====================================================================
 * MAIN
 * ==================================================================== */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  Contour Cube → GeoJump Placement Approaches                  ║\n");
    printf("║  6000 cells (6×10×10×10) → 1440 addresses (15×48×2)          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    printf("  Cells: %d   Addresses: %d   Target avg: %.2f cells/addr\n\n",
           CUBE_CELLS, GEO_ADDRS, (double)CUBE_CELLS / GEO_ADDRS);

    init_cells();
    init_ico_verts();
    init_face_towers();

    /* ── Run all 4 approaches ── */
    baseline_direct();
    approach1_fibo_stride();
    approach2_barycentric();
    approach3_resolution_scaled();

    /* ── Analyze and print results ── */
    AddrStats bs = analyze_addrs(baseline_addr_count);
    AddrStats s1 = analyze_addrs(approach1_addr_count);
    AddrStats s2 = analyze_addrs(approach2_addr_count);
    AddrStats s3 = analyze_addrs(approach3_addr_count);

    printf("══════════════════════════════════════════════════════════════════\n");
    printf("  BASELINE: Direct Mapping  (face→tower, (x,y,z)→addr)\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    print_stats("Direct", &bs);

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  APPROACH 1: Fibonacci Stride-37\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    print_stats("FiboStride", &s1);

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  APPROACH 2: Barycentric-Weighted (icosahedral u,v)\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    print_stats("Barycentric", &s2);

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  APPROACH 3: Resolution-Scaled (h-depth variable)\n");
    printf("══════════════════════════════════════════════════════════════════\n");
    print_stats("ResScale", &s3);

    /* ── Comparative summary ── */
    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║  COMPARATIVE SUMMARY                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    printf("  %-25s  Collisions  Coverage  Uniformity  Skew\n", "Approach");
    printf("  %-25s  ──────────  ────────  ──────────  ────\n", "─────────────────────────");

    printf("  %-25s  %6d      %5.1f%%     %.3f       %.3f\n",
           "Baseline (direct)", bs.collisions,
           100.0 * bs.covered / GEO_ADDRS,
           (bs.mean > 0.001) ? bs.stddev / bs.mean : 0.0,
           bs.skewness);

    printf("  %-25s  %6d      %5.1f%%     %.3f       %.3f\n",
           "Fibo stride-37", s1.collisions,
           100.0 * s1.covered / GEO_ADDRS,
           (s1.mean > 0.001) ? s1.stddev / s1.mean : 0.0,
           s1.skewness);

    printf("  %-25s  %6d      %5.1f%%     %.3f       %.3f\n",
           "Barycentric (ico)", s2.collisions,
           100.0 * s2.covered / GEO_ADDRS,
           (s2.mean > 0.001) ? s2.stddev / s2.mean : 0.0,
           s2.skewness);

    printf("  %-25s  %6d      %5.1f%%     %.3f       %.3f\n",
           "Resolution-scaled", s3.collisions,
           100.0 * s3.covered / GEO_ADDRS,
           (s3.mean > 0.001) ? s3.stddev / s3.mean : 0.0,
           s3.skewness);

    printf("\n  Lower collisions = less overwriting (better for storage)\n");
    printf("  Lower uniformity (CV) = more even spread across addresses\n");
    printf("  Skew near 0 = symmetric distribution\n");
    printf("  Higher coverage = more addresses utilized\n");

    /* ── Find the best approach ── */
    printf("\n── Best approach by metric ──\n");

    struct { const char *name; int val; } best_coll = {"Baseline", bs.collisions};
    if (s1.collisions < best_coll.val) { best_coll.name = "Fibo stride-37"; best_coll.val = s1.collisions; }
    if (s2.collisions < best_coll.val) { best_coll.name = "Barycentric";    best_coll.val = s2.collisions; }
    if (s3.collisions < best_coll.val) { best_coll.name = "Resolution-scaled"; best_coll.val = s3.collisions; }
    printf("  Fewest collisions: %s (%d)\n", best_coll.name, best_coll.val);

    double cv_bs = (bs.mean > 0.001) ? bs.stddev / bs.mean : 999;
    double cv_s1 = (s1.mean > 0.001) ? s1.stddev / s1.mean : 999;
    double cv_s2 = (s2.mean > 0.001) ? s2.stddev / s2.mean : 999;
    double cv_s3 = (s3.mean > 0.001) ? s3.stddev / s3.mean : 999;
    double best_cv = cv_bs;
    const char *best_cv_name = "Baseline";
    if (cv_s1 < best_cv) { best_cv = cv_s1; best_cv_name = "Fibo stride-37"; }
    if (cv_s2 < best_cv) { best_cv = cv_s2; best_cv_name = "Barycentric"; }
    if (cv_s3 < best_cv) { best_cv = cv_s3; best_cv_name = "Resolution-scaled"; }
    printf("  Most uniform:     %s (CV=%.3f)\n", best_cv_name, best_cv);

    printf("\n══════════════════════════════════════════════════════════════════\n");
    printf("  PASS — All approaches computed.\n");
    printf("══════════════════════════════════════════════════════════════════\n");

    return 0;
}
