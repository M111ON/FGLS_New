/*
 * ico20_geo_jump_link.c — Connect icosahedron triangle subdivision to geo_jump tower system
 *
 * Maps icosahedron faces (20 × n² triangles) into GEO_JUMP address space.
 * Key bridge: n=6 → 720 triangles, 720 × 2 = 1440 = GEO_FIBO_CLOCK.
 *
 * Compile:
 *   gcc -O2 -std=c11 -Icollection/dgls/geo/include -Icore -IHfolder \
 *       -o runner/explore/ico20_geo_jump_link.exe \
 *       runner/explore/ico20_geo_jump_link.c \
 *       collection/dgls/geo/src/geo_jump.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "geo_jump.h"

/* ── Icosahedron subdivision math ── */

#define ICO20_FACES      20u
#define ICO20_STRIDE     37u          /* coprime with both 1440 and 20736 */
#define BENCHMARK_N    1000000u

static inline uint32_t ico20_triangles(uint32_t n) { return ICO20_FACES * n * n; }
static inline uint32_t ico20_vertices(uint32_t n) { return 10u * n * n + 2u; }

/* Map triangle (face, i, j) at subdivision n → linear triangle_id.
 * face in [0,20), i,j in [0,n), i+j < n gives n² triangles per face.
 * We use the full n² grid for simplicity (each face has n² triangles). */
static inline uint32_t ico20_tri_id(uint32_t face, uint32_t i, uint32_t j, uint32_t n) {
    return face * n * n + i * n + j;
}

/* ── Mapping: triangle_id → geo_jump node ──
 *
 * Strategy: stride-37 modular mapping (coprime with GEO_FIBO_CLOCK and GEO_FULL).
 *   gcd(37, 1440) = 1  and  gcd(37, 20736) = 1
 * → (tri_id * 37) mod M is injective for tri_id < M.
 *
 * Two address spaces:
 *   FIBO: node = (tri_id * 37) % GEO_FIBO_CLOCK  — maps into 1440-slot clock
 *   FULL: node = (tri_id * 37) % GEO_FULL        — maps into 20736 full space
 */

static inline uint32_t tri_to_fibo(uint32_t tri_id) {
    return (tri_id * ICO20_STRIDE) % GEO_FIBO_CLOCK;
}

static inline uint32_t tri_to_full(uint32_t tri_id) {
    return (tri_id * ICO20_STRIDE) % GEO_FULL;
}

/* Alternative: tower-based mapping using geo_jump API directly.
 * Each triangle node is sent through a JUMP_HILBERT router. */
static inline uint32_t tri_to_geojump(uint32_t tri_id) {
    return geo_jump(tri_id, JUMP_HILBERT, 1);
}

/* ── Test helpers ── */

static int g_pass = 0, g_fail = 0;

#define CHECK(name, cond) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", name); } \
    else      { g_fail++; printf("  [FAIL] %s\n", name); } \
} while(0)

/* ── TEST 1: Subdivision levels ── */

static int test_subdiv_levels(void) {
    printf("\n=== TEST 1: Icosahedron subdivision levels ===\n");

    struct { uint32_t n; uint32_t expect_tri; uint32_t expect_vtx; } levels[] = {
        { 1,   20,     12 },
        { 6,  720,    362 },
        { 12, 2880,  1442 },
        { 36, 25920, 12962 },
    };
    int ok = 1;
    for (int t = 0; t < 4; t++) {
        uint32_t n   = levels[t].n;
        uint32_t tri = ico20_triangles(n);
        uint32_t vtx = ico20_vertices(n);
        uint32_t fit_full  = tri > 0 ? GEO_FULL / tri : 0;
        uint32_t fit_fibo  = tri > 0 ? GEO_FIBO_CLOCK / tri : 0;
        double   frac_full = (double)tri / GEO_FULL;
        double   frac_fibo = (double)tri / GEO_FIBO_CLOCK;

        printf("  n=%2u: tri=%u  vtx=%u  |  GEO_FULL:  %u fit (%.1fx coverage)  |  FIBO_CLOCK: %u fit (%.1fx coverage)\n",
               n, tri, vtx, fit_full, frac_full, fit_fibo, frac_fibo);

        if (tri != levels[t].expect_tri || vtx != levels[t].expect_vtx) {
            printf("  [FAIL] n=%u: expected tri=%u vtx=%u\n", n, levels[t].expect_tri, levels[t].expect_vtx);
            ok = 0;
        }
    }
    CHECK("n=1:20 tri, n=6:720, n=12:2880, n=36:25920", ok);
    CHECK("n=6: 720 = GEO_FIBO_CLOCK/2 (1 island = 2×360)",
          ico20_triangles(6) * 2 == GEO_FIBO_CLOCK);
    CHECK("n=12: 2880 = 2 × GEO_FIBO_CLOCK (2 fibo cycles)",
          ico20_triangles(12) == 2u * GEO_FIBO_CLOCK);
    CHECK("GEO_TOWER × GEO_TOWER = GEO_FULL",
          GEO_TOWER * GEO_TOWER == GEO_FULL);
    return 0;
}

/* ── TEST 2: Map n=6 to geo_jump addresses ── */

static int test_n6_mapping(void) {
    printf("\n=== TEST 2: n=6 mapping to geo_jump FIBO addresses ===\n");
    uint32_t n = 6, total = ico20_triangles(n);
    uint32_t fibo_hit[GEO_FIBO_CLOCK];
    memset(fibo_hit, 0, sizeof(fibo_hit));

    uint32_t min_node = GEO_FIBO_CLOCK, max_node = 0;
    for (uint32_t f = 0; f < ICO20_FACES; f++) {
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = 0; j < n; j++) {
                uint32_t tid = ico20_tri_id(f, i, j, n);
                uint32_t node = tri_to_fibo(tid);
                fibo_hit[node]++;
                if (node < min_node) min_node = node;
                if (node > max_node) max_node = node;
            }
        }
    }

    /* Count unique nodes and collisions */
    uint32_t unique = 0, collisions = 0;
    for (uint32_t k = 0; k < GEO_FIBO_CLOCK; k++) {
        if (fibo_hit[k] > 0) {
            unique++;
            if (fibo_hit[k] > 1) collisions += fibo_hit[k] - 1;
        }
    }

    printf("  Total triangles: %u\n", total);
    printf("  GEO_FIBO_CLOCK:  %u slots\n", GEO_FIBO_CLOCK);
    printf("  Unique nodes hit: %u / %u (%.1f%%)\n", unique, GEO_FIBO_CLOCK,
           100.0 * unique / GEO_FIBO_CLOCK);
    printf("  Collisions: %u  (extra hits)\n", collisions);
    printf("  Node range: [%u, %u]\n", min_node, max_node);

    CHECK("n=6: 720 unique nodes (stride-37 injective on FIBO clock)", unique == total && collisions == 0);
    CHECK("n=6: 50% FIBO clock coverage", unique == GEO_FIBO_CLOCK / 2);
    return 0;
}

/* ── TEST 3: Map n=12 to geo_jump addresses ── */

static int test_n12_mapping(void) {
    printf("\n=== TEST 3: n=12 mapping — 2 fibo cycles ===\n");
    uint32_t n = 12, total = ico20_triangles(n);
    uint32_t fibo_hit[GEO_FIBO_CLOCK];
    memset(fibo_hit, 0, sizeof(fibo_hit));

    for (uint32_t f = 0; f < ICO20_FACES; f++) {
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = 0; j < n; j++) {
                uint32_t tid = ico20_tri_id(f, i, j, n);
                uint32_t node = tri_to_fibo(tid);
                fibo_hit[node]++;
            }
        }
    }

    uint32_t unique = 0, hit_twice = 0;
    for (uint32_t k = 0; k < GEO_FIBO_CLOCK; k++) {
        if (fibo_hit[k] > 0) unique++;
        if (fibo_hit[k] == 2) hit_twice++;
    }

    printf("  Total triangles: %u\n", total);
    printf("  Expected: 2 × GEO_FIBO_CLOCK = %u\n", 2u * GEO_FIBO_CLOCK);
    printf("  Unique nodes: %u (of %u)\n", unique, GEO_FIBO_CLOCK);
    printf("  Nodes hit exactly twice: %u\n", hit_twice);

    CHECK("n=12: total = 2 × GEO_FIBO_CLOCK", total == 2u * GEO_FIBO_CLOCK);
    CHECK("n=12: 100% FIBO clock coverage (every slot hit)", unique == GEO_FIBO_CLOCK);
    CHECK("n=12: every slot hit exactly 2 times (2 fibo cycles)", hit_twice == GEO_FIBO_CLOCK);
    return 0;
}

/* ── TEST 4: Triangle → geo_jump node relationship ── */

static int test_relationship(void) {
    printf("\n=== TEST 4: triangle_id → geo_jump node relationship ===\n");
    printf("  Mapping: node = (triangle_id × %u) %% %u\n", ICO20_STRIDE, GEO_FIBO_CLOCK);
    printf("  Mapping: node = (triangle_id × %u) %% %u\n", ICO20_STRIDE, GEO_FULL);
    printf("  geo_jump API: node = geo_jump(tri_id, JUMP_HILBERT, 1)\n\n");

    /* Show sample mappings for n=6, face 0 */
    uint32_t n = 6;
    printf("  Sample (n=6, face 0):\n");
    printf("  %-6s  %-8s  %-12s  %-12s  %-12s\n", "(i,j)", "tri_id", "FIBO_node", "FULL_node", "HILBERT");
    for (uint32_t i = 0; i < 6; i++) {
        for (uint32_t j = 0; j < 6; j++) {
            if (i + j >= n) continue;
            uint32_t tid = ico20_tri_id(0, i, j, n);
            printf("  (%u,%-3u)  %-8u  %-12u  %-12u  %-12u\n",
                   i, j, tid, tri_to_fibo(tid), tri_to_full(tid), tri_to_geojump(tid));
        }
    }

    /* Verify: node = triangle_id * GEO_TOWER / 720 + offset
     * This is an alternative formula: for n=6, the scale factor GEO_FIBO_CLOCK / 720 = 2.
     * node = tri_id * 2 + offset gives consecutive-pair coverage. */
    printf("\n  Alternative: node = tri_id × (FIBO_CLOCK/720) + offset\n");
    printf("  Scale factor: GEO_FIBO_CLOCK / 720 = %u\n", GEO_FIBO_CLOCK / ico20_triangles(6));
    printf("  720 × 2 = %u = GEO_FIBO_CLOCK ✓\n", ico20_triangles(6) * 2);

    CHECK("720 × (GEO_FIBO_CLOCK/720) = GEO_FIBO_CLOCK",
          ico20_triangles(6) * (GEO_FIBO_CLOCK / ico20_triangles(6)) == GEO_FIBO_CLOCK);
    CHECK("scale_factor = 2 connects 720→1440",
          GEO_FIBO_CLOCK / ico20_triangles(6) == 2);
    return 0;
}

/* ── TEST 5: Bijectivity verification for n=6 ── */

static int test_bijective(void) {
    printf("\n=== TEST 5: Bijectivity test (n=6) ===\n");
    uint32_t n = 6, total = ico20_triangles(n);

    /* Allocate seen-flags for FIBO and FULL */
    uint32_t *fibo_seen = (uint32_t *)calloc(GEO_FIBO_CLOCK, sizeof(uint32_t));
    uint32_t *full_seen = (uint32_t *)calloc(GEO_FULL, sizeof(uint32_t));
    if (!fibo_seen || !full_seen) { printf("  [FAIL] allocation\n"); return 1; }

    uint32_t fibo_collisions = 0, full_collisions = 0;

    for (uint32_t f = 0; f < ICO20_FACES; f++) {
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t j = 0; j < n; j++) {
                uint32_t tid = ico20_tri_id(f, i, j, n);

                /* FIBO mapping */
                uint32_t fn = tri_to_fibo(tid);
                fibo_seen[fn]++;
                if (fibo_seen[fn] > 1) fibo_collisions++;

                /* FULL mapping */
                uint32_t fln = tri_to_full(tid);
                full_seen[fln]++;
                if (full_seen[fln] > 1) full_collisions++;
            }
        }
    }

    /* Count unique hits */
    uint32_t fibo_unique = 0, full_unique = 0;
    for (uint32_t k = 0; k < GEO_FIBO_CLOCK; k++) if (fibo_seen[k]) fibo_unique++;
    for (uint32_t k = 0; k < GEO_FULL; k++) if (full_seen[k]) full_unique++;

    printf("  Total triangles: %u\n", total);
    printf("  FIBO mapping: %u unique nodes, %u collisions → %s\n",
           fibo_unique, fibo_collisions, fibo_collisions == 0 ? "BIJECTIVE ✓" : "NOT injective");
    printf("  FULL mapping: %u unique nodes, %u collisions → %s\n",
           full_unique, full_collisions, full_collisions == 0 ? "BIJECTIVE ✓" : "NOT injective");

    /* Verify coprimality */
    uint32_t g1 = ICO20_STRIDE, m1 = GEO_FIBO_CLOCK;
    while (m1) { uint32_t t = g1; g1 = m1; m1 = t % m1; }
    uint32_t g2 = ICO20_STRIDE, m2 = GEO_FULL;
    while (m2) { uint32_t t = g2; g2 = m2; m2 = t % m2; }

    printf("  gcd(%u, %u) = %u (must be 1 for bijectivity)\n", ICO20_STRIDE, GEO_FIBO_CLOCK, g1);
    printf("  gcd(%u, %u) = %u (must be 1 for bijectivity)\n", ICO20_STRIDE, GEO_FULL, g2);

    CHECK("FIBO mapping is bijective (no collisions)", fibo_collisions == 0);
    CHECK("FULL mapping is bijective (no collisions)", full_collisions == 0);
    CHECK("gcd(stride, FIBO_CLOCK) = 1", g1 == 1);
    CHECK("gcd(stride, GEO_FULL) = 1", g2 == 1);

    free(fibo_seen);
    free(full_seen);
    return 0;
}

/* ── TEST 6: Benchmark 1M lookups ── */

static int test_benchmark(void) {
    printf("\n=== TEST 6: Benchmark %u triangle→geo_jump lookups ===\n", BENCHMARK_N);

    volatile uint32_t sink = 0;

    /* Benchmark 1: tri_to_fibo (pure math) */
    clock_t t0 = clock();
    for (uint32_t i = 0; i < BENCHMARK_N; i++) {
        sink += tri_to_fibo(i);
    }
    clock_t t1 = clock();
    double ms_fibo = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("  tri_to_fibo:    %8.2f ms  (%.1f ns/lookup)\n", ms_fibo,
           1000000.0 * ms_fibo / BENCHMARK_N);

    /* Benchmark 2: tri_to_full (pure math) */
    t0 = clock();
    for (uint32_t i = 0; i < BENCHMARK_N; i++) {
        sink += tri_to_full(i);
    }
    t1 = clock();
    double ms_full = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("  tri_to_full:    %8.2f ms  (%.1f ns/lookup)\n", ms_full,
           1000000.0 * ms_full / BENCHMARK_N);

    /* Benchmark 3: geo_jump JUMP_HILBERT (API call) */
    t0 = clock();
    for (uint32_t i = 0; i < BENCHMARK_N; i++) {
        sink += geo_jump(i, JUMP_HILBERT, 1);
    }
    t1 = clock();
    double ms_hilb = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("  geo_jump(HILBERT): %8.2f ms  (%.1f ns/lookup)\n", ms_hilb,
           1000000.0 * ms_hilb / BENCHMARK_N);

    /* Benchmark 4: geo_jump_batch (batch API) */
    uint32_t batch_sz = 10000;
    uint32_t *batch_in  = (uint32_t *)malloc(batch_sz * sizeof(uint32_t));
    uint32_t *batch_out = (uint32_t *)malloc(batch_sz * sizeof(uint32_t));
    if (batch_in && batch_out) {
        for (uint32_t i = 0; i < batch_sz; i++) batch_in[i] = i;
        uint32_t iters = BENCHMARK_N / batch_sz;
        t0 = clock();
        for (uint32_t r = 0; r < iters; r++) {
            geo_jump_batch(batch_in, batch_sz, JUMP_HILBERT, 1, batch_out);
        }
        t1 = clock();
        double ms_batch = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
        printf("  geo_jump_batch:   %8.2f ms  (%.1f ns/lookup, %u per call)\n",
               ms_batch, 1000000.0 * ms_batch / BENCHMARK_N, batch_sz);
    }
    free(batch_in);
    free(batch_out);

    (void)sink;
    CHECK("Benchmark completed (sink prevents dead-code elimination)", sink != 0);
    printf("  Sink: %u (prevents optimization)\n", sink);
    return 0;
}

/* ── Summary ── */

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  ico20_geo_jump_link — Icosahedron ↔ geo_jump bridge       ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("  GEO_FULL=%u  GEO_TOWER=%u  GEO_BLOCK=%u  GEO_FIBO_CLOCK=%u\n",
           GEO_FULL, GEO_TOWER, GEO_BLOCK, GEO_FIBO_CLOCK);
    printf("  Stride=%u (coprime with %u and %u)\n",
           ICO20_STRIDE, GEO_FIBO_CLOCK, GEO_FULL);

    test_subdiv_levels();
    test_n6_mapping();
    test_n12_mapping();
    test_relationship();
    test_bijective();
    test_benchmark();

    printf("\n════════════════════════════════════════════════════════════════\n");
    printf("  RESULT: %d/%d PASS, %d FAIL\n", g_pass, g_pass + g_fail, g_fail);
    printf("════════════════════════════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
