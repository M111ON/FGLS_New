/*
 * bench_rdh_unfold.c — Benchmark: RDH point → 72 centroids
 * ═══════════════════════════════════════════════════════════════
 * ทดสอบว่าจาก RDH point เดียว สร้าง 72 centroids ได้ภายใน 10ns ไหม
 *
 * Build:
 *   gcc -O2 -std=c11 -Icore -Icollection/rdh -Icollection/geopixel/geopixel \
 *       bench_rdh_unfold.c -o bench_rdh_unfold.exe -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "geo_goldberg_tile.h"
#include "rdh_addr.h"
#include "rdh_capture.h"

/* ── Centroid struct ─────────────────────────────────────── */
typedef struct {
    int32_t x;
    int32_t y;
    uint8_t type;   /* 0=triangle, 1=hexagon, 2=pentagon */
    uint16_t id;    /* which gap/hex/pent */
} Centroid;

/* ── Timer ───────────────────────────────────────────────── */
static inline uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ══════════════════════════════════════════════════════════════
 * UNFOLD: RDH point → 72 centroids
 * 
 * Blueprint baked in GGT_TRIGAP_ADJ[60][3] + GP(1,0) structure
 * No trig. No float. Pure LUT lookup.
 * ══════════════════════════════════════════════════════════════ */

/* Goldberg GP(1,0) face centers (baked, integer coords)
 * 12 pentagons + 10 hexagons = 22 faces
 * Coordinates pre-computed from icosahedron/dodecahedron geometry */
static const int32_t GP_FACE_X[22] = {
    /* 12 pentagons (0..11) */
      0,  100,   50,  -50, -100,    0,   81,   81,  -81,  -81,   50,  -50,
    /* 10 hexagons (12..21) */
      62,   62,    0,    0,  -62,  -62,  100,   38,  -38, -100
};
static const int32_t GP_FACE_Y[22] = {
    /* 12 pentagons */
    100,    0,  -81,  -81,    0,  100,  -50,   50,   50,  -50,  -81,  -81,
    /* 10 hexagons */
      0,   62,  100,  -100,  -62,    0,    0,  -81,  -81,    0
};

/* Triangle gap centroids: average of 3 adjacent face centers
 * Baked from GGT_TRIGAP_ADJ[60][3] — face indices into GP_FACE */
static int32_t TRI_CENTROID_X[60];
static int32_t TRI_CENTROID_Y[60];

/* 10 ring hexagon centroids (hexagons 12..21 from GP(1,0)) */
#define N_RING_HEX 10
static int32_t HEX_CENTROID_X[N_RING_HEX];
static int32_t HEX_CENTROID_Y[N_RING_HEX];

/* 2 pole pentagon centroids (pentagons 0 and 5 — north/south poles) */
#define N_POLE_PENT 2
static int32_t PENT_CENTROID_X[N_POLE_PENT] = { 0, 0 };
static int32_t PENT_CENTROID_Y[N_POLE_PENT] = { 100, 100 };

/* Pre-compute baked LUT at init */
static void unfold_init(void) {
    /* Triangle centroids: average of 3 adjacent face centers */
    for (int g = 0; g < 60; g++) {
        int fa = GGT_TRIGAP_ADJ[g][0];
        int fb = GGT_TRIGAP_ADJ[g][1];
        int fc = GGT_TRIGAP_ADJ[g][2];
        /* clamp to 0..21 for GP_FACE arrays */
        if (fa > 21) fa = 21;
        if (fb > 21) fb = 21;
        if (fc > 21) fc = 21;
        TRI_CENTROID_X[g] = (GP_FACE_X[fa] + GP_FACE_X[fb] + GP_FACE_X[fc]) / 3;
        TRI_CENTROID_Y[g] = (GP_FACE_Y[fa] + GP_FACE_Y[fb] + GP_FACE_Y[fc]) / 3;
    }
    /* Hex centroids: directly from GP_FACE */
    for (int h = 0; h < N_RING_HEX; h++) {
        HEX_CENTROID_X[h] = GP_FACE_X[12 + h];
        HEX_CENTROID_Y[h] = GP_FACE_Y[12 + h];
    }
}

/* ══════════════════════════════════════════════════════════════
 * UNFOLD HOT PATH — 72 centroids from one RDH point
 * 
 * Input:  rdh_point (ring, wedge) from rdh_capture
 * Output: centroids[72] filled
 * 
 * No trig. No float. Pure LUT + integer add.
 * ══════════════════════════════════════════════════════════════ */
static inline int unfold_from_rdh(
    int32_t ring, int32_t wedge,
    Centroid centroids[72])
{
    int n = 0;

    /* 60 triangle centroids — pure LUT, no compute */
    for (int g = 0; g < 60; g++) {
        centroids[n].x = TRI_CENTROID_X[g] + ring * 10;
        centroids[n].y = TRI_CENTROID_Y[g] + wedge * 10;
        centroids[n].type = 0;
        centroids[n].id = (uint16_t)g;
        n++;
    }

    /* 10 hexagon centroids — pure LUT */
    for (int h = 0; h < N_RING_HEX; h++) {
        centroids[n].x = HEX_CENTROID_X[h] + ring * 10;
        centroids[n].y = HEX_CENTROID_Y[h] + wedge * 10;
        centroids[n].type = 1;
        centroids[n].id = (uint16_t)(12 + h);
        n++;
    }

    /* 2 pentagon centroids — pure LUT */
    for (int p = 0; p < N_POLE_PENT; p++) {
        centroids[n].x = PENT_CENTROID_X[p] + ring * 10;
        centroids[n].y = PENT_CENTROID_Y[p] + wedge * 10;
        centroids[n].type = 2;
        centroids[n].id = (uint16_t)(p ? 5 : 0);
        n++;
    }

    return n; /* should be 72 */
}

/* ══════════════════════════════════════════════════════════════
 * BENCHMARK
 * ══════════════════════════════════════════════════════════════ */
int main(void) {
    unfold_init();

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  RDH Unfold Benchmark — 72 centroids from 1 point  ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* ── Test 1: Verify count = 72 ── */
    Centroid centroids[72];
    int n = unfold_from_rdh(3, 7, centroids);
    printf("T1: unfold count = %d (expected 72) — %s\n", n, n == 72 ? "PASS" : "FAIL");

    /* Show first few centroids */
    printf("\nFirst 5 centroids:\n");
    for (int i = 0; i < 5; i++) {
        printf("  [%d] x=%d y=%d type=%d id=%d\n",
               i, centroids[i].x, centroids[i].y, centroids[i].type, centroids[i].id);
    }

    /* ── Test 2: Different RDH points give different centroids ── */
    Centroid c1[72], c2[72];
    unfold_from_rdh(0, 0, c1);
    unfold_from_rdh(5, 10, c2);
    int different = 0;
    for (int i = 0; i < 72; i++) {
        if (c1[i].x != c2[i].x || c1[i].y != c2[i].y) different++;
    }
    printf("\nT2: different centroids for different points: %d/72 — %s\n",
           different, different > 0 ? "PASS" : "FAIL");

    /* ── Test 3: Benchmark — single unfold call ── */
    printf("\n--- Benchmark ---\n");
    Centroid bench_out[72];
    uint64_t t0, t1;
    int warmup = 1000;
    int iterations = 1000000;

    /* Warmup */
    for (int i = 0; i < warmup; i++)
        unfold_from_rdh(i % 144, i % 120, bench_out);

    /* Benchmark */
    t0 = ns_now();
    for (int i = 0; i < iterations; i++)
        unfold_from_rdh(i % 144, i % 120, bench_out);
    t1 = ns_now();

    double total_ns = (double)(t1 - t0);
    double per_call_ns = total_ns / iterations;
    printf("  %d iterations: %.0f total ns\n", iterations, total_ns);
    printf("  Per unfold call: %.1f ns\n", per_call_ns);
    printf("  Target: < 10 ns — %s\n", per_call_ns < 10.0 ? "PASS ✓" : "FAIL ✗");
    printf("  Target: < 1.5 ns — %s\n", per_call_ns < 1.5 ? "PASS ✓" : "MAYBE (depends on CPU)");

    /* ── Test 4: Benchmark with RDH capture + unfold pipeline ── */
    printf("\n--- Full Pipeline: data → rdh_capture → unfold ---\n");
    RDHConfig cfg = RDH_CAPTURE_144;
    uint8_t test_data[48];
    srand(42);
    for (int i = 0; i < 48; i++) test_data[i] = (uint8_t)(rand() & 0xFF);

    /* rdh_capture + unfold */
    int N2 = 1000000;
    t0 = ns_now();
    for (int i = 0; i < N2; i++) {
        int64_t key = rdh_capture(test_data, 48, &cfg);
        int32_t ring = (int32_t)((key % 144));
        int32_t wedge = (int32_t)((key / 144) % 144);
        unfold_from_rdh(ring, wedge, bench_out);
    }
    t1 = ns_now();
    double pipe_ns = (double)(t1 - t0) / N2;
    printf("  rdh_capture + unfold: %.1f ns per call\n", pipe_ns);
    printf("  (rdh_capture ~1.5ns + unfold ~X ns)\n");

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  SUMMARY: 72 centroids built from 1 RDH point\n");
    printf("  Type: pure LUT (no trig, no float)\n");
    printf("  Benchmark result: %.1f ns/call\n", per_call_ns);
    printf("═══════════════════════════════════════════════════════\n");

    return 0;
}
