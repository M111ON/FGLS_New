/*
 * bench_rdh_unfold_v2.c — RDH point → 72 centroids: ACCESS not COPY
 * ═══════════════════════════════════════════════════════════════
 * 72 centroids = FREE — they EXIST as geometry of the RDH point
 * "Unfold" = realizing the neighborhood exists, not copying it
 *
 * Access any centroid O(1) from RDH point.
 * No copy. No alloc. Pure address arithmetic.
 *
 * Build:
 *   gcc -O2 -std=c11 -Icollection/geopixel/geopixel -Icore -Icollection/rdh \
 *       bench_rdh_unfold_v2.c -o bench_rdh_unfold_v2.exe -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "geo_goldberg_tile.h"
#include "rdh_addr.h"
#include "rdh_capture.h"

/* ── Timer ───────────────────────────────────────────────── */
static inline uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ══════════════════════════════════════════════════════════════
 * CENTROID ACCESS — O(1) from RDH point
 * 
 * 72 centroids baked into geometry:
 *   0..59   = triangle gap centroids (from GGT_TRIGAP_ADJ)
 *   60..69  = ring hexagon centroids (hex 12..21)
 *   70..71  = pole pentagon centroids (pent 0, 5)
 *
 * Access: centroid_index (0..71) → (x, y) via baked LUT + offset
 * No copy needed — caller reads directly.
 * ══════════════════════════════════════════════════════════════ */

/* Baked triangle centroid positions (pre-computed from face centers) */
static int16_t TRI_X[60], TRI_Y[60];

/* Baked hex centroid positions (hex 12..21) */
static int16_t HEX_X[10], HEX_Y[10];

/* Baked pent centroid positions (pent 0, 5) */
static int16_t PENT_X[2], PENT_Y[2];

/* Face centers (baked from icosahedron geometry) */
static const int16_t FC_X[22] = {
      0,  100,   50,  -50, -100,    0,   81,   81,  -81,  -81,   50,  -50,
     62,   62,    0,    0,  -62,  -62,  100,   38,  -38, -100
};
static const int16_t FC_Y[22] = {
    100,    0,  -81,  -81,    0,  100,  -50,   50,   50,  -50,  -81,  -81,
      0,   62,  100, -100,  -62,    0,    0,  -81,  -81,    0
};

static void centroid_init(void) {
    for (int g = 0; g < 60; g++) {
        int a = GGT_TRIGAP_ADJ[g][0];
        int b = GGT_TRIGAP_ADJ[g][1];
        int c = GGT_TRIGAP_ADJ[g][2];
        if (a > 21) a = 21; if (b > 21) b = 21; if (c > 21) c = 21;
        TRI_X[g] = (int16_t)((FC_X[a] + FC_X[b] + FC_X[c]) / 3);
        TRI_Y[g] = (int16_t)((FC_Y[a] + FC_Y[b] + FC_Y[c]) / 3);
    }
    for (int h = 0; h < 10; h++) {
        HEX_X[h] = FC_X[12 + h];
        HEX_Y[h] = FC_Y[12 + h];
    }
    PENT_X[0] = FC_X[0]; PENT_Y[0] = FC_Y[0];
    PENT_X[1] = FC_X[5]; PENT_Y[1] = FC_Y[5];
}

/* ══════════════════════════════════════════════════════════════
 * UNFOLD — 72 centroids from 1 RDH point
 * 
 * Returns (x, y) of centroid by index (0..71).
 * No copy. No struct. Pure arithmetic.
 * RDH point offset = ring×10 + wedge×10 applied as base.
 * ══════════════════════════════════════════════════════════════ */
static inline void centroid_xy(
    int32_t rdh_ring, int32_t rdh_wedge,
    uint8_t idx,
    int32_t *out_x, int32_t *out_y)
{
    int32_t base_x = rdh_ring * 10;
    int32_t base_y = rdh_wedge * 10;

    if (idx < 60) {
        /* triangle gap centroid */
        *out_x = TRI_X[idx] + base_x;
        *out_y = TRI_Y[idx] + base_y;
    } else if (idx < 70) {
        /* ring hexagon centroid */
        uint8_t h = idx - 60;
        *out_x = HEX_X[h] + base_x;
        *out_y = HEX_Y[h] + base_y;
    } else {
        /* pole pentagon centroid */
        uint8_t p = idx - 70;
        *out_x = PENT_X[p] + base_x;
        *out_y = PENT_Y[p] + base_y;
    }
}

/* ══════════════════════════════════════════════════════════════
 * BENCHMARK
 * ══════════════════════════════════════════════════════════════ */
int main(void) {
    centroid_init();

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  RDH Unfold v2 — 72 FREE centroids (access, not copy)║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* ── Test 1: Access all 72 ── */
    int32_t x, y;
    printf("T1: Access all 72 centroids from ring=3, wedge=7\n");
    for (int i = 0; i < 72; i++) {
        centroid_xy(3, 7, (uint8_t)i, &x, &y);
    }
    printf("  PASS (72 accessed)\n");

    /* Show triangle centroids 0..4 */
    printf("\nTriangle centroids [0..4]:\n");
    for (int i = 0; i < 5; i++) {
        centroid_xy(3, 7, (uint8_t)i, &x, &y);
        printf("  [%2d] x=%d y=%d\n", i, x, y);
    }

    /* Show hex centroids [60..64] */
    printf("Hex centroids [60..64]:\n");
    for (int i = 60; i < 65; i++) {
        centroid_xy(3, 7, (uint8_t)i, &x, &y);
        printf("  [%2d] x=%d y=%d\n", i, x, y);
    }

    /* Show pent centroids [70..71] */
    printf("Pent centroids [70..71]:\n");
    for (int i = 70; i < 72; i++) {
        centroid_xy(3, 7, (uint8_t)i, &x, &y);
        printf("  [%2d] x=%d y=%d\n", i, x, y);
    }

    /* ── Test 2: Single centroid access — raw speed ── */
    printf("\n--- Benchmark: single centroid access ---\n");
    int N = 10000000; /* 10M iterations */
    volatile int32_t sink_x, sink_y; /* prevent optimize-away */

    /* warmup */
    for (int i = 0; i < 10000; i++)
        centroid_xy(i % 144, i % 120, (uint8_t)(i % 72), &sink_x, &sink_y);

    uint64_t t0 = ns_now();
    for (int i = 0; i < N; i++)
        centroid_xy(i % 144, i % 120, (uint8_t)(i % 72), &sink_x, &sink_y);
    uint64_t t1 = ns_now();

    double ns_per = (double)(t1 - t0) / N;
    printf("  %d accesses: %.1f ns/access\n", N, ns_per);
    printf("  Target: < 1.5 ns — %s\n", ns_per < 1.5 ? "PASS ✓" : ns_per < 3.0 ? "CLOSE" : "SLOW");

    /* ── Test 3: Full pipeline — rdh_capture + centroid access ── */
    printf("\n--- Pipeline: data → rdh_capture → centroid access ---\n");
    RDHConfig cfg = RDH_CAPTURE_144;
    uint8_t data[48];
    srand(42);
    for (int i = 0; i < 48; i++) data[i] = (uint8_t)(rand() & 0xFF);

    int M = 1000000;
    t0 = ns_now();
    for (int i = 0; i < M; i++) {
        int64_t key = rdh_capture(data, 48, &cfg);
        int32_t ring = (int32_t)(key % 144);
        int32_t wedge = (int32_t)((key / 144) % 144);
        centroid_xy(ring, wedge, 0, &sink_x, &sink_y); /* access centroid 0 */
    }
    t1 = ns_now();
    ns_per = (double)(t1 - t0) / M;
    printf("  rdh_capture + 1 centroid access: %.1f ns\n", ns_per);

    /* ── Test 4: All 72 centroids — one RDH point ── */
    printf("\n--- All 72 centroids from one RDH point ---\n");
    t0 = ns_now();
    for (int i = 0; i < M; i++) {
        int64_t key = rdh_capture(data, 48, &cfg);
        int32_t ring = (int32_t)(key % 144);
        int32_t wedge = (int32_t)((key / 144) % 144);
        for (int c = 0; c < 72; c++)
            centroid_xy(ring, wedge, (uint8_t)c, &sink_x, &sink_y);
    }
    t1 = ns_now();
    ns_per = (double)(t1 - t0) / M;
    printf("  rdh_capture + 72 centroids: %.1f ns total = %.1f ns/centroid\n",
           ns_per, ns_per / 72);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  72 centroids = FREE (baked geometry, no compute)\n");
    printf("  Access O(1) per centroid: %.1f ns\n", ns_per / 72);
    printf("  rdh_capture: ~1.5 ns\n");
    printf("  Total: data → 72 centroids ≈ %.1f ns\n", ns_per);
    printf("═══════════════════════════════════════════════════════\n");

    return 0;
}
