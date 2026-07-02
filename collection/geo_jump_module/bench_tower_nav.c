#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "geo_field_icosphere.h"
#include "geo_jump.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static volatile uint64_t sink = 0;

#define N 10000000

int main(void) {
    gf_cap_lut_init();
    printf("=== Key\xe2\x86\x92trap/pos: old (recompute) vs LUT ===\n\n");

    /* OLD */
    double t0 = now_sec();
    for (int r = 0; r < N; r++) {
        uint32_t k = (uint32_t)(rand() % 300);
        uint8_t t = icosphere_capture_key_to_trapezoid(k);
        uint32_t p = icosphere_capture_key_to_tower_pos(k);
        sink += t + p;
    }
    double e0 = now_sec() - t0;
    printf("old (recompute) x%d: %.3fs  (%.0f/s)\n", N, e0, N / e0);

    /* LUT */
    t0 = now_sec();
    for (int r = 0; r < N; r++) {
        uint32_t k = (uint32_t)(rand() % 300);
        sink += gf_cap_trap(k) + gf_cap_pos(k);
    }
    double e1 = now_sec() - t0;
    printf("LUT (lookup)    x%d: %.3fs  (%.0f/s)\n", N, e1, N / e1);
    if (e0 > 0) printf("speedup: %.0fx\n", e0 / e1);

    /* Full pipeline: key \xe2\x86\x92 GEO_FULL via recompute vs LUT */
    printf("\n--- Full pipeline: key \xe2\x86\x92 GEO_FULL ---\n");
    t0 = now_sec();
    for (int r = 0; r < N / 2; r++) {
        uint32_t k = (uint32_t)(rand() % 300);
        uint8_t t = icosphere_capture_key_to_trapezoid(k);
        uint32_t p = icosphere_capture_key_to_tower_pos(k);
        sink += icosphere_trapezoid_to_geo_full(t, p);
    }
    e0 = now_sec() - t0;
    printf("old (recompute) x%d: %.3fs  (%.0f/s)\n", N/2, e0, N/2 / e0);

    t0 = now_sec();
    for (int r = 0; r < N; r++) {
        uint32_t k = (uint32_t)(rand() % 300);
        sink += icosphere_trapezoid_to_geo_full(gf_cap_trap(k), gf_cap_pos(k));
    }
    e1 = now_sec() - t0;
    printf("LUT (lookup)    x%d: %.3fs  (%.0f/s)\n", N, e1, N / e1);

    /* Capo + mirror with LUT */
    printf("\n--- Capo + Mirror with LUT ---\n");
    t0 = now_sec();
    for (int r = 0; r < N; r++) {
        uint32_t k = (uint32_t)(rand() % 300);
        uint8_t t = gf_cap_trap(k);
        uint32_t p = gf_cap_pos(k);
        sink += icosphere_trapezoid_to_geo_full_capo(t, p, r & 15);
        sink += icosphere_trap_fast_flip(t, p);
    }
    e0 = now_sec() - t0;
    printf("capo+mirror    x%d: %.3fs  (%.0f/s combined, %.0f/s each)\n",
           N, e0, N / e0, N * 2 / e0);

    printf("\n=== DONE (sink=%llu) ===\n", (unsigned long long)sink);
    return 0;
}
